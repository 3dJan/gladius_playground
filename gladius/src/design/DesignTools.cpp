#include "DesignTools.h"

#include "../Document.h"
#include "../EventLogger.h"
#include "../ExpressionParser.h"
#include "../ExpressionToGraphConverter.h"
#include "../FileSystemUtils.h"
#include "../ProgramSnippetApplier.h"
#include "../agent/Conversation.h"
#include "AgentToolOutput.h"
#include "../io/3mf/Lib3mfLoader.h"
#include "../io/3mf/LibraryMetadata.h"
#include "../nodes/Assembly.h"
#include "../nodes/GraphOptimizer.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <filesystem>
#include <set>

namespace gladius::design
{
    namespace
    {
        /// @brief Bytes a program listing may occupy; a tool result is re-prefilled every later turn
        constexpr std::size_t kMaxSnippetChars = 12000;

        /**
         * @brief Error text for an unresolvable graph id, listing the ids that do exist.
         * @details A bare "Unknown configurationId" costs a whole extra model turn: the model has to
         *          re-read the design summary before it can retry. The ids are a handful of short
         *          strings, so returning them makes the retry land on the first try.
         */
        template <typename Entry>
        [[nodiscard]] std::string unknownIdError(char const * field,
                                                 std::string const & given,
                                                 std::map<std::string, Entry> const & entries,
                                                 auto const & nameOf)
        {
            std::vector<std::string> options;
            for (auto const & [id, entry] : entries)
            {
                options.push_back(nameOf(entry) + " (" + id + ")");
            }

            std::ostringstream out;
            out << "Unknown " << field << ": " << given << ". Existing " << field << "s (pass the id "
                << "in parentheses, not the name): "
                << (options.empty() ? std::string("none - create one first")
                                    : [&]
                                    {
                                        std::string joined;
                                        for (auto const & option : options)
                                        {
                                            if (!joined.empty())
                                            {
                                                joined += ", ";
                                            }
                                            joined += option;
                                        }
                                        return joined;
                                    }())
                << '.';
            return out.str();
        }

        /**
         * @brief Reads a string array argument, tolerating a single string.
         * @details Models frequently emit "tags": "gyroid" where an array is declared, which makes
         *          nlohmann throw a type error and the tool call fail. A lone string is a valid
         *          one-element list, so accept it instead.
         */
        [[nodiscard]] std::vector<std::string> readStringArray(nlohmann::json const & value)
        {
            if (value.is_null())
            {
                return {};
            }
            if (value.is_string())
            {
                return {value.get<std::string>()};
            }
            if (!value.is_array())
            {
                return {};
            }
            std::vector<std::string> out;
            out.reserve(value.size());
            for (auto const & entry : value)
            {
                if (entry.is_string())
                {
                    out.push_back(entry.get<std::string>());
                }
                else if (entry.is_number() || entry.is_boolean())
                {
                    out.push_back(entry.dump());
                }
            }
            return out;
        }

        /// @brief Overload that reads the array stored under `key`
        [[nodiscard]] std::vector<std::string> readStringArray(nlohmann::json const & args,
                                                               char const * key)
        {
            auto const it = args.find(key);
            return readStringArray(it == args.end() ? nlohmann::json{} : *it);
        }

        /**
         * @brief Reads a numeric argument, tolerating a numeric string.
         * @details A model that writes "qualityScore": "0.8" instead of 0.8 makes nlohmann throw a
         *          type error, so the whole tool call fails. The value is unambiguous either way.
         */
        [[nodiscard]] double readNumber(nlohmann::json const & args, char const * key, double fallback)
        {
            auto const it = args.find(key);
            if (it == args.end())
            {
                return fallback;
            }
            if (it->is_number())
            {
                return it->get<double>();
            }
            if (it->is_string())
            {
                try
                {
                    return std::stod(it->get<std::string>());
                }
                catch (std::exception const &)
                {
                    return fallback;
                }
            }
            return fallback;
        }

        Evaluation defaultEvaluateConfiguration(DesignGraph const & graph,
                                                DesignCatalogue const & catalogue,
                                                Configuration const & cfg)
        {
            Evaluation eval;
            std::vector<std::string> violations;
            if (catalogue.checkCompatibility(cfg.selectedVariants, violations))
            {
                eval.deterministicPassed.push_back("All selected variants satisfy compatibility rules");
            }
            else
            {
                for (auto const & v : violations)
                {
                    eval.deterministicWarnings.push_back("Compatibility violation: " + v);
                }
            }

            if (cfg.selectedVariants.size() >= graph.nodes().size() && !graph.nodes().empty())
            {
                eval.deterministicPassed.push_back("All design nodes have an assigned variant");
            }
            else if (!graph.nodes().empty())
            {
                std::string missing;
                for (auto const & [nodeId, node] : graph.nodes())
                {
                    if (cfg.selectedVariants.contains(nodeId))
                    {
                        continue;
                    }
                    if (!missing.empty())
                    {
                        missing += ", ";
                    }
                    missing += nodeId + " (" + node.name + ")";
                }
                eval.deterministicWarnings.push_back(
                  "Some design nodes lack a selected variant: " + missing
                  + ". Pass a selectedVariants entry for each of them to create_configuration and "
                    "activate the result before calling finish_design again.");
            }

            eval.pruningScore = static_cast<double>(eval.deterministicPassed.size()) * 10.0
                                - static_cast<double>(eval.deterministicWarnings.size()) * 15.0;
            return eval;
        }
    } // namespace

    void registerDesignTools(tools::ToolRegistry & registry, DesignToolContext & ctx)
    {
        registry.registerSyncTool(
          {"get_design_summary",
           "Get a summary of the current design phase, requirements, nodes, variants, and active configuration.",
           {{"type", "object"}, {"properties", nlohmann::json::object()}},
           tools::ToolPolicy::ReadOnly},
          [&ctx](nlohmann::json const &) -> nlohmann::json {
              // Compact: the full dump (previews included) rode along in history for every later
              // turn and cost more prompt tokens than any other single tool result.
              nlohmann::json res = agent_tool_output::compactDesignGraph(ctx.graph.toJson());
              res["success"] = true;
              res["summary"] = ctx.graph.summarize();
              return res;
          });

        registry.registerSyncTool(
          {"add_requirement",
           "Add a functional or manufacturing requirement to the design graph.",
           {{"type", "object"},
            {"required", {"description"}},
            {"properties",
             {{"description", {{"type", "string"}}},
              {"category", {{"type", "string"}}},
              {"mandatory", {{"type", "boolean"}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              std::string const desc = args.value("description", "");
              if (desc.empty())
              {
                  return {{"success", false}, {"error", "description is required"}};
              }
              std::string const id = ctx.graph.addRequirement(
                desc, args.value("category", "functional"), args.value("mandatory", true));
              return {{"success", true}, {"requirementId", id}};
          });

        registry.registerSyncTool(
          {"add_function",
           "Add a function or component node to the hierarchical design graph.",
           {{"type", "object"},
            {"required", {"name"}},
            {"properties",
             {{"name", {{"type", "string"}}},
              {"type", {{"type", "string"}, {"enum", {"system", "function_group", "function", "component", "feature"}}}},
              {"parentNodeId", {{"type", "string"}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              std::string const name = args.value("name", "");
              if (name.empty())
              {
                  return {{"success", false}, {"error", "name is required"}};
              }
              std::string const typeStr = args.value("type", "function");
              DesignNodeType nodeType = DesignNodeType::Function;
              if (typeStr == "system")
                  nodeType = DesignNodeType::System;
              else if (typeStr == "function_group")
                  nodeType = DesignNodeType::FunctionGroup;
              else if (typeStr == "component")
                  nodeType = DesignNodeType::Component;
              else if (typeStr == "feature")
                  nodeType = DesignNodeType::Feature;

              std::string const id = ctx.graph.addNode(nodeType, name, args.value("parentNodeId", ""));
              return {{"success", true}, {"nodeId", id}};
          });

        registry.registerSyncTool(
          {"create_variant",
           "Create a solution principle or geometric variant for a design node.",
           {{"type", "object"},
            {"required", {"nodeId", "name"}},
            {"properties",
             {{"nodeId", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"rationale", {{"type", "string"}}},
              {"fidelity", {{"type", "string"}, {"enum", {"symbolic", "envelope", "structural", "detailed"}}}},
              {"parameters", {{"type", "object"}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              std::string const nodeId = args.value("nodeId", "");
              std::string const name = args.value("name", "");
              std::string const fidStr = args.value("fidelity", "symbolic");
              FidelityLevel fid = FidelityLevel::Symbolic;
              if (fidStr == "envelope")
                  fid = FidelityLevel::Envelope;
              else if (fidStr == "structural")
                  fid = FidelityLevel::Structural;
              else if (fidStr == "detailed")
                  fid = FidelityLevel::Detailed;

              nlohmann::json const params =
                args.contains("parameters") && args["parameters"].is_object()
                  ? args["parameters"]
                  : nlohmann::json::object();

              std::string const id =
                ctx.graph.createVariant(nodeId, name, args.value("rationale", ""), fid, params);
              if (id.empty())
              {
                  return {{"success", false},
                          {"error",
                           unknownIdError("nodeId", nodeId, ctx.graph.nodes(),
                                          [](DesignNode const & n) { return n.name; })}};
              }
              return {{"success", true}, {"variantId", id}};
          });

        registry.registerSyncTool(
          {"reject_variant",
           "Reject a weak or incompatible variant while preserving its rationale.",
           {{"type", "object"},
            {"required", {"variantId"}},
            {"properties", {{"variantId", {{"type", "string"}}}, {"reason", {{"type", "string"}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              std::string const vid = args.value("variantId", "");
              bool const ok = ctx.graph.rejectVariant(vid, args.value("reason", ""));
              if (ok)
              {
                  return nlohmann::json{{"success", true}};
              }
              return {{"success", false},
                      {"error", unknownIdError("variantId", vid, ctx.graph.variants(),
                                               [](DesignVariant const & v) { return v.name; })}};
          });

        registry.registerSyncTool(
          {"freeze_component",
           "Freeze a design node so configuration switches or hybridizations do not alter it.",
           {{"type", "object"},
            {"required", {"nodeId"}},
            {"properties", {{"nodeId", {{"type", "string"}}}, {"frozen", {{"type", "boolean"}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              std::string const nid = args.value("nodeId", "");
              bool const freeze = args.value("frozen", true);
              bool const ok = freeze ? ctx.graph.freezeNode(nid) : ctx.graph.unfreezeNode(nid);
              if (ok)
              {
                  return nlohmann::json{{"success", true}};
              }
              return {{"success", false},
                      {"error", unknownIdError("nodeId", nid, ctx.graph.nodes(),
                                               [](DesignNode const & n) { return n.name; })}};
          });

        registry.registerSyncTool(
          {"define_interface",
           "Attach an explicit component interface specification to a variant.",
           {{"type", "object"},
            {"required", {"variantId", "name", "kind"}},
            {"properties",
             {{"variantId", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"kind", {{"type", "string"}}},
              {"specification", {{"type", "object"}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              std::string const vid = args.value("variantId", "");
              Interface iface{
                vid + "_" + args.value("name", "iface"),
                args.value("name", ""),
                args.value("kind", "mount"),
                args.value("specification", nlohmann::json::object())};
              bool const ok = ctx.graph.defineInterface(vid, std::move(iface));
              return ok ? nlohmann::json{{"success", true}}
                        : nlohmann::json{{"success", false}, {"error", "Unknown variantId: " + vid}};
          });

        registry.registerSyncTool(
          {"create_configuration",
           "Create a named system configuration mapping nodeIds to variantIds.",
           {{"type", "object"},
            {"required", {"name", "selectedVariants"}},
            {"properties",
             {{"name", {{"type", "string"}}}, {"selectedVariants", {{"type", "object"}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              std::unordered_map<std::string, std::string> sel;
              if (args.contains("selectedVariants") && args["selectedVariants"].is_object())
              {
                  for (auto const & [k, v] : args["selectedVariants"].items())
                  {
                      if (v.is_string())
                      {
                          sel[k] = v.get<std::string>();
                      }
                  }
              }
              std::vector<std::string> violations;
              bool const compat = ctx.catalogue.checkCompatibility(sel, violations);
              std::string const cid = ctx.graph.createConfiguration(args.value("name", "Concept"), std::move(sel));
              return {{"success", true},
                      {"configurationId", cid},
                      {"compatible", compat},
                      {"violations", violations}};
          });

        registry.registerSyncTool(
          {"activate_configuration",
           "Activate a system configuration in the design graph (preserving frozen nodes).",
           {{"type", "object"},
            {"required", {"configurationId"}},
            {"properties", {{"configurationId", {{"type", "string"}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              std::string const cid = args.value("configurationId", "");
              bool const ok = ctx.graph.activateConfiguration(cid);
              if (ok && ctx.captureViewport)
              {
                  auto png = ctx.captureViewport(256, 256, "isometric");
                  if (!png.empty())
                  {
                      ctx.graph.setConfigurationPreview(cid, std::move(png));
                  }
              }
              if (ok)
              {
                  return nlohmann::json{{"success", true}, {"configurationId", cid}};
              }
              return {{"success", false},
                      {"error", unknownIdError("configurationId", cid, ctx.graph.configurations(),
                                               [](Configuration const & c) { return c.name; })}};
          });

        registry.registerSyncTool(
          {"hybridize_variants",
           "Combine two configurations by overriding specified non-frozen nodes.",
           {{"type", "object"},
            {"required", {"name", "baseConfigId", "overrideConfigId", "overrideNodeIds"}},
            {"properties",
             {{"name", {{"type", "string"}}},
              {"baseConfigId", {{"type", "string"}}},
              {"overrideConfigId", {{"type", "string"}}},
              {"overrideNodeIds", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              std::vector<std::string> nodes;
              if (args.contains("overrideNodeIds") && args["overrideNodeIds"].is_array())
              {
                  for (auto const & item : args["overrideNodeIds"])
                  {
                      if (item.is_string())
                      {
                          nodes.push_back(item.get<std::string>());
                      }
                  }
              }
              auto const res = ctx.graph.hybridizeConfigurations(
                args.value("name", "Hybrid"),
                args.value("baseConfigId", ""),
                args.value("overrideConfigId", ""),
                nodes);
              if (!res.has_value())
              {
                  return {{"success", false}, {"error", "Invalid baseConfigId or overrideConfigId"}};
              }
              return {{"success", true}, {"configurationId", *res}};
          });

        registry.registerSyncTool(
          {"begin_edit",
           "Begin a grouped edit transaction so multiple model edits form a single undo step.",
           {{"type", "object"},
            {"required", {"description"}},
            {"properties", {{"description", {{"type", "string"}}}}}},
           tools::ToolPolicy::ModelModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.document || !ctx.document->getAssembly())
              {
                  return {{"success", false}, {"error", "No active document assembly"}};
              }
              bool const ok =
                ctx.transactions.beginEdit(args.value("description", "Agent edit"), *ctx.document->getAssembly());
              return ok ? nlohmann::json{{"success", true}}
                        : nlohmann::json{{"success", false}, {"error", "Transaction already active"}};
          });

        registry.registerSyncTool(
          {"commit_edit",
           "Commit the active edit transaction to the user undo history.",
           {{"type", "object"}, {"properties", nlohmann::json::object()}},
           tools::ToolPolicy::ModelModification},
          [&ctx](nlohmann::json const &) -> nlohmann::json {
              if (!ctx.document || !ctx.document->getAssembly())
              {
                  return {{"success", false}, {"error", "No active document assembly"}};
              }
              bool const ok = ctx.transactions.commitEdit(*ctx.document->getAssembly());
              return ok ? nlohmann::json{{"success", true}}
                        : nlohmann::json{{"success", false}, {"error", "No active transaction"}};
          });

        registry.registerSyncTool(
          {"rollback_edit",
           "Roll back the active edit transaction, restoring the pre-edit assembly.",
           {{"type", "object"}, {"properties", nlohmann::json::object()}},
           tools::ToolPolicy::ModelModification},
          [&ctx](nlohmann::json const &) -> nlohmann::json {
              if (!ctx.document || !ctx.document->getAssembly())
              {
                  return {{"success", false}, {"error", "No active document assembly"}};
              }
              bool const ok = ctx.transactions.rollbackEdit(*ctx.document->getAssembly());
              return ok ? nlohmann::json{{"success", true}}
                        : nlohmann::json{{"success", false}, {"error", "No active transaction"}};
          });

        registry.registerSyncTool(
          {"create_checkpoint",
           "Create a named design checkpoint of the current assembly.",
           {{"type", "object"},
            {"required", {"label"}},
            {"properties", {{"label", {{"type", "string"}}}, {"level", {{"type", "integer"}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              nodes::Assembly emptyAssembly;
              nodes::Assembly const & target =
                (ctx.document && ctx.document->getAssembly()) ? *ctx.document->getAssembly() : emptyAssembly;
              std::vector<std::byte> png;
              if (ctx.captureViewport)
              {
                  png = ctx.captureViewport(256, 256, "isometric");
              }
              std::string const id = ctx.checkpoints.createCheckpoint(
                args.value("label", ""), args.value("level", 0), target, std::move(png));
              return {{"success", true}, {"checkpointId", id}};
          });

        registry.registerSyncTool(
          {"restore_checkpoint",
           "Restore a previously saved assembly checkpoint.",
           {{"type", "object"},
            {"required", {"checkpointId"}},
            {"properties", {{"checkpointId", {{"type", "string"}}}}}},
           tools::ToolPolicy::ModelModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.document || !ctx.document->getAssembly())
              {
                  return {{"success", false}, {"error", "No active document assembly"}};
              }
              std::string const id = args.value("checkpointId", "");
              bool const ok = ctx.checkpoints.restoreCheckpoint(id, *ctx.document->getAssembly());
              return ok ? nlohmann::json{{"success", true}, {"checkpointId", id}}
                        : nlohmann::json{{"success", false}, {"error", "Unknown checkpointId: " + id}};
          });

        registry.registerSyncTool(
          {"capture_viewport",
           "Capture the current 3D viewport as a PNG image for visual assessment.",
           {{"type", "object"},
            {"properties",
             {{"width", {{"type", "integer"}}},
              {"height", {{"type", "integer"}}},
              {"camera", {{"type", "string"}}}}}},
           tools::ToolPolicy::ReadOnly},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.captureViewport)
              {
                  return {{"success", false}, {"error", "Viewport capture is not attached"}};
              }
              int const w = args.value("width", 512);
              int const h = args.value("height", 512);
              std::string const cam = args.value("camera", "isometric");
              auto const png = ctx.captureViewport(w, h, cam);
              if (png.empty())
              {
                  return {{"success", false}, {"error", "Viewport capture returned empty image"}};
              }
              return {{"success", true},
                      {"width", w},
                      {"height", h},
                      {"mimeType", "image/png"},
                      {"base64", agent::encodeBase64(png)}};
          });

        registry.registerSyncTool(
          {"finish_design",
           "Run final deterministic validation and mark the design complete if valid.",
           {{"type", "object"}, {"properties", nlohmann::json::object()}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const &) -> nlohmann::json {
              auto const active = ctx.graph.activeConfiguration();
              if (!active.has_value())
              {
                  return {{"success", false},
                          {"error",
                           "No active configuration to validate. Create one with "
                           "create_configuration (a selectedVariants entry for every design node), "
                           "then call activate_configuration, then finish_design."}};
              }
              Evaluation const eval = ctx.validateModel
                                        ? ctx.validateModel(ctx.graph, *active)
                                        : defaultEvaluateConfiguration(ctx.graph, ctx.catalogue, *active);
              if (!eval.deterministicWarnings.empty())
              {
                  return {{"success", false},
                          {"valid", false},
                          {"passed", eval.deterministicPassed},
                          {"warnings", eval.deterministicWarnings}};
              }
              ctx.graph.setPhase(DesignPhase::Complete);
              if (ctx.captureViewport)
              {
                  auto png = ctx.captureViewport(256, 256, "isometric");
                  if (!png.empty())
                  {
                      ctx.graph.setConfigurationPreview(active->id, std::move(png));
                  }
              }
              return {{"success", true},
                      {"valid", true},
                      {"phase", toString(ctx.graph.phase())},
                      {"passed", eval.deterministicPassed}};
          });

        registry.registerSyncTool(
          {"get_model_summary",
           "Inspect the currently open 3MF document's functions, node counts, and build items.",
           {{"type", "object"}, {"properties", nlohmann::json::object()}},
           tools::ToolPolicy::ReadOnly},
          [&ctx](nlohmann::json const &) -> nlohmann::json {
              if (!ctx.document || !ctx.document->getAssembly())
              {
                  return {{"success", false}, {"error", "No active document"}};
              }
              auto const assembly = ctx.document->getAssembly();
              nlohmann::json funcs = nlohmann::json::array();
              for (auto const & [fid, model] : assembly->getFunctions())
              {
                  if (!model)
                  {
                      continue;
                  }
                  funcs.push_back({
                      {"id", fid},
                      {"name", model->getDisplayName().value_or("")},
                      {"nodeCount", model->getSize()}
                  });
              }
              return {
                  {"success", true},
                  {"functionCount", funcs.size()},
                  {"functions", std::move(funcs)},
                  {"assemblyModelId", assembly->getAssemblyModelId()}
              };
          });

        registry.registerSyncTool(
          {"get_program_snippet",
           "Read the current open document's implicit function program listing. Pass functions to "
           "read only the named ones; otherwise the listing is trimmed to keep the prompt small.",
           {{"type", "object"},
            {"properties",
             {{"functions", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}},
           tools::ToolPolicy::ReadOnly},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.document || !ctx.document->getAssembly())
              {
                  return {{"success", false}, {"error", "No active document"}};
              }
              auto const assembly = ctx.document->getAssembly();
              std::string const snippet =
                ExpressionToGraphConverter::convertProgramToSnippet(*assembly);
              std::vector<std::string> wanted;
              if (args.contains("functions") && args["functions"].is_array())
              {
                  wanted = args["functions"].get<std::vector<std::string>>();
              }
              if (!wanted.empty())
              {
                  std::string const filtered = agent_tool_output::filterProgramListing(snippet, wanted);
                  if (filtered.empty())
                  {
                      return {{"success", false},
                              {"error", "No function matched the requested names. Read the listing "
                                        "without 'functions' to see the names, then ask again."}};
                  }
                  return {{"success", true}, {"snippet", filtered}};
              }
              return {{"success", true},
                      {"snippet", agent_tool_output::capProgramListing(snippet, kMaxSnippetChars)}};
          });

        registry.registerSyncTool(
          {"set_program_snippet",
           "Apply a multi-function implicit geometry program snippet directly to the open document and refresh the viewport.",
           {{"type", "object"},
            {"required", {"snippet"}},
            {"properties", {{"snippet", {{"type", "string"}}}}}},
           tools::ToolPolicy::ModelModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.document || !ctx.document->getAssembly())
              {
                  return {{"success", false}, {"error", "No active document"}};
              }
              std::string const snippet = args.value("snippet", "");
              if (snippet.empty())
              {
                  return {{"success", false}, {"error", "snippet is required"}};
              }
              try
              {
                  std::string normalizedSnippet;
                  std::size_t appliedCount = 0;
                  std::vector<std::string> appliedNames;
                  {
                      auto const token = ctx.document->waitForAssemblyToken();
                      auto assembly = ctx.document->getAssembly();
                      ExpressionParser parser;
                      auto const report = ProgramSnippetApplier::apply(snippet, *assembly, parser);
                      if (!report.failed.empty())
                      {
                          nlohmann::json errs = nlohmann::json::array();
                          for (auto const & f : report.failed)
                          {
                              errs.push_back({{"name", f.displayName}, {"message", f.message}});
                          }
                          return {{"success", false},
                                  {"error", report.failed.front().message},
                                  {"failed", std::move(errs)},
                                  {"hint", "Nothing was applied: the snippet is only accepted when "
                                           "every listed function converts. List each function with "
                                           "its own "// Function: name (ID: n)" header, use the "
                                           "qualified name name_ID when calling another function, and "
                                           "give every called parameter a source in the caller."}};
                      }
                      for (auto & [fid, funcModel] : assembly->getFunctions())
                      {
                          if (funcModel)
                          {
                              (void) nodes::GraphOptimizer::optimize(*funcModel);
                          }
                      }
                      appliedCount = report.parsed.size();
                      for (auto const & parsed : report.parsed)
                      {
                          appliedNames.push_back(parsed.displayName);
                      }
                      normalizedSnippet =
                        ExpressionToGraphConverter::convertProgramToSnippet(*assembly);
                  }
                  auto const logger = ctx.document->getSharedLogger();
                  std::size_t const prevLogSize = logger ? logger->size() : 0u;

                  ctx.document->update3mfModel();
                  ctx.document->rebuildResourceDependencyGraph();
                  ctx.document->refreshModelBlocking();
                  ctx.document->markFileAsChanged();
                  ctx.document->signalStructuralEdit();
                  if (ctx.onDocumentModified)
                  {
                      ctx.onDocumentModified();
                  }

                  std::vector<std::string> errors;
                  if (logger && logger->size() > prevLogSize)
                  {
                      for (auto it = logger->cbegin() + static_cast<long>(prevLogSize);
                           it != logger->cend();
                           ++it)
                      {
                          if (it->getSeverity() == events::Severity::Error
                              || it->getSeverity() == events::Severity::FatalError)
                          {
                              errors.push_back(it->getMessage());
                          }
                      }
                  }
                  if (!errors.empty())
                  {
                      return {{"success", false},
                              {"error", errors.front()},
                              {"errors", errors},
                              {"appliedFunctions", appliedCount},
                              {"snippet", agent_tool_output::capProgramListing(normalizedSnippet,
                                                                              kMaxSnippetChars)}};
                  }
                  // The snippet is not echoed back on success: the model just wrote it, and the copy
                  // would sit in history and be re-prefilled for every later turn.
                  return {{"success", true},
                          {"appliedFunctions", appliedCount},
                          {"appliedNames", appliedNames}};
              }
              catch (std::exception const & ex)
              {
                  return {{"success", false}, {"error", ex.what()}};
              }
          });

        registry.registerSyncTool(
          {"insert_image_to_document",
           "Insert an image file into the currently open 3MF document as an image stack resource.",
           {{"type", "object"},
            {"required", {"path"}},
            {"properties", {{"path", {{"type", "string"}}}}}},
           tools::ToolPolicy::ModelModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.document)
              {
                  return {{"success", false}, {"error", "No active document"}};
              }
              std::filesystem::path const imgPath(args.value("path", ""));
              if (imgPath.empty() || !std::filesystem::exists(imgPath))
              {
                  return {{"success", false}, {"error", "Image file does not exist"}};
              }
              try
              {
                  auto const key = ctx.document->addImageStackResource(imgPath);
                  ctx.document->refreshModelBlocking();
                  ctx.document->markFileAsChanged();
                  ctx.document->signalStructuralEdit();
                  if (ctx.onDocumentModified)
                  {
                      ctx.onDocumentModified();
                  }
                  return {{"success", true},
                          {"resourceId", key.getResourceId().value_or(0)},
                          {"displayName", key.getDisplayName()}};
              }
              catch (std::exception const & ex)
              {
                  return {{"success", false}, {"error", ex.what()}};
              }
          });

        registry.registerSyncTool(
          {"list_skills",
           "List available built-in Gladius design skills.",
           {{"type", "object"}, {"properties", nlohmann::json::object()}},
           tools::ToolPolicy::ReadOnly},
          [&ctx](nlohmann::json const &) -> nlohmann::json {
              if (!ctx.skills)
              {
                  return {{"success", true}, {"skills", nlohmann::json::array()}};
              }
              nlohmann::json arr = nlohmann::json::array();
              for (auto const & s : ctx.skills->skills())
              {
                  arr.push_back({{"name", s.name}, {"description", s.description}});
              }
              return {{"success", true}, {"skills", std::move(arr)}};
          });

        registry.registerSyncTool(
          {"load_skill",
           "Load detailed instructions for a built-in Gladius design skill by name.",
           {{"type", "object"},
            {"required", {"name"}},
            {"properties", {{"name", {{"type", "string"}}}}}},
           tools::ToolPolicy::ReadOnly},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.skills)
              {
                  return {{"success", false}, {"error", "Skills registry unavailable"}};
              }
              std::string const name = args.value("name", "");
              auto const skill = ctx.skills->findSkill(name);
              if (!skill.has_value())
              {
                  return {{"success", false}, {"error", "Unknown skill: " + name}};
              }
              return {{"success", true},
                      {"name", skill->name},
                      {"description", skill->description},
                      {"content", skill->content}};
          });

        registry.registerSyncTool(
          {"list_library_items",
           "Search the Gladius 3MF component/function library by optional category or query keyword.",
           {{"type", "object"},
            {"properties",
             {{"category", {{"type", "string"}}}, {"query", {{"type", "string"}}}}}},
           tools::ToolPolicy::ReadOnly},
          [](nlohmann::json const & args) -> nlohmann::json {
              std::string const catFilter = args.value("category", "");
              std::string query = args.value("query", "");
              std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) {
                  return static_cast<char>(std::tolower(c));
              });

              auto toLower = [](std::string s) {
                  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                      return static_cast<char>(std::tolower(c));
                  });
                  return s;
              };

              std::set<std::string> categories;
              std::error_code ec;
              for (auto const & root : {getUserLibraryDir(), getShippedLibraryDir()})
              {
                  if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec))
                  {
                      continue;
                  }
                  for (auto const & dir : std::filesystem::directory_iterator(root, ec))
                  {
                      if (dir.is_directory(ec))
                      {
                          std::string const name = dir.path().filename().string();
                          if (!name.empty() && name[0] != '.'
                              && (catFilter.empty() || catFilter == name))
                          {
                              categories.insert(name);
                          }
                      }
                  }
              }

              nlohmann::json items = nlohmann::json::array();
              std::set<std::string> seen;
              for (auto const & cat : categories)
              {
                  for (auto const & root : {getUserLibraryDir(), getShippedLibraryDir()})
                  {
                      auto const catDir = root / cat;
                      if (!std::filesystem::exists(catDir, ec)
                          || !std::filesystem::is_directory(catDir, ec))
                      {
                          continue;
                      }
                      for (auto const & file : std::filesystem::directory_iterator(catDir, ec))
                      {
                          if (!file.is_regular_file(ec) || file.path().extension() != ".3mf")
                          {
                              continue;
                          }
                          std::string const entryName = file.path().stem().string();
                          std::string const key = cat + "/" + entryName;
                          if (!seen.insert(key).second)
                          {
                              continue;
                          }

                          std::string desc;
                          std::string tags;
                          try
                          {
                              auto wrapper = io::loadLib3mfScoped();
                              auto model = wrapper->CreateModel();
                              auto reader = model->QueryReader("3mf");
                              reader->ReadFromFile(file.path().string());
                              if (auto meta = io::readLibraryMetadata(model))
                              {
                                  desc = meta->libraryDescription;
                                  tags = meta->libraryTags;
                              }
                          }
                          catch (...)
                          {
                          }

                          if (!query.empty())
                          {
                              std::string const haystack =
                                toLower(cat + " " + entryName + " " + desc + " " + tags);
                              if (haystack.find(query) == std::string::npos)
                              {
                                  continue;
                              }
                          }

                          items.push_back({{"category", cat},
                                           {"name", entryName},
                                           {"description", desc},
                                           {"tags", tags}});
                      }
                  }
              }
              return {{"success", true}, {"count", items.size()}, {"items", std::move(items)}};
          });

        registry.registerSyncTool(
          {"import_library_item",
           "Import a reusable library item into the currently open document and return its function signature.",
           {{"type", "object"},
            {"required", {"category", "name"}},
            {"properties",
             {{"category", {{"type", "string"}}}, {"name", {{"type", "string"}}}}}},
           tools::ToolPolicy::ModelModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.document || !ctx.document->getAssembly())
              {
                  return {{"success", false}, {"error", "No active document"}};
              }
              std::string const category = args.value("category", "");
              std::string const name = args.value("name", "");
              if (category.empty() || name.empty())
              {
                  return {{"success", false}, {"error", "category and name are required"}};
              }

              std::filesystem::path entryPath;
              std::error_code ec;
              for (auto const & root : {getUserLibraryDir(), getShippedLibraryDir()})
              {
                  auto const candidate = root / category / (name + ".3mf");
                  if (std::filesystem::exists(candidate, ec))
                  {
                      entryPath = candidate;
                      break;
                  }
              }
              if (entryPath.empty())
              {
                  return {{"success", false},
                          {"error", "Library item not found: " + category + "/" + name}};
              }

              try
              {
                  auto wrapper = io::loadLib3mfScoped();
                  auto libModel = wrapper->CreateModel();
                  auto reader = libModel->QueryReader("3mf");
                  reader->ReadFromFile(entryPath.string());
                  auto metadata = io::readLibraryMetadata(libModel);

                  std::string targetFuncName;
                  if (metadata.has_value())
                  {
                      auto taggedIds = io::parseResourceIds(metadata->libraryFunctions);
                      auto funcIter = libModel->GetFunctions();
                      while (funcIter->MoveNext())
                      {
                          auto func = funcIter->GetCurrentFunction();
                          if (!taggedIds.empty()
                              && std::find(taggedIds.begin(),
                                           taggedIds.end(),
                                           func->GetModelResourceID())
                                   != taggedIds.end())
                          {
                              targetFuncName = func->GetDisplayName();
                              break;
                          }
                      }
                  }
                  if (targetFuncName.empty())
                  {
                      auto funcIter = libModel->GetFunctions();
                      if (funcIter->MoveNext())
                      {
                          targetFuncName = funcIter->GetCurrentFunction()->GetDisplayName();
                      }
                  }
                  if (targetFuncName.empty())
                  {
                      return {{"success", false},
                              {"error", "Library item contains no functions"}};
                  }

                  auto const match = ctx.document->mergeAndResolve(entryPath, targetFuncName);
                  if (match.id == 0)
                  {
                      return {{"success", false},
                              {"error", "Failed to import library function: " + targetFuncName}};
                  }

                  ctx.document->update3mfModel();
                  ctx.document->rebuildResourceDependencyGraph();
                  ctx.document->refreshModelBlocking();
                  ctx.document->markFileAsChanged();
                  ctx.document->signalStructuralEdit();
                  if (ctx.onDocumentModified)
                  {
                      ctx.onDocumentModified();
                  }

                  std::string importedSnippet;
                  if (auto const assembly = ctx.document->getAssembly())
                  {
                      if (auto const it = assembly->getFunctions().find(match.id);
                          it != assembly->getFunctions().end() && it->second)
                      {
                          importedSnippet =
                            ExpressionToGraphConverter::convertGraphToSnippet(*it->second);
                      }
                  }

                  return {{"success", true},
                          {"functionId", match.id},
                          {"functionName",
                           match.model ? match.model->getDisplayName().value_or(targetFuncName)
                                       : targetFuncName},
                          {"snippet", importedSnippet}};
              }
              catch (std::exception const & ex)
              {
                  return {{"success", false}, {"error", ex.what()}};
              }
          });

        registry.registerSyncTool(
          {"hybridize_configurations",
           "Recombine two existing system configurations into a new hybrid configuration by overriding selected node IDs.",
           {{"type", "object"},
            {"required", {"newName", "baseConfigId", "overrideConfigId", "overrideNodeIds"}},
            {"properties",
             {{"newName", {{"type", "string"}}},
              {"baseConfigId", {{"type", "string"}}},
              {"overrideConfigId", {{"type", "string"}}},
              {"overrideNodeIds", {{"type", "array"}, {"items", {{"type", "string"}}}}},
              {"activate", {{"type", "boolean"}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              std::string const newName = args.value("newName", "");
              std::string const baseId = args.value("baseConfigId", "");
              std::string const overId = args.value("overrideConfigId", "");
              auto const nodeIds = readStringArray(args, "overrideNodeIds");
              if (newName.empty() || baseId.empty() || overId.empty())
              {
                  return {{"success", false},
                          {"error", "newName, baseConfigId, and overrideConfigId are required"}};
              }
              auto const newId = ctx.graph.hybridizeConfigurations(newName, baseId, overId, nodeIds);
              if (!newId.has_value())
              {
                  return {{"success", false}, {"error", "Base or override configuration not found"}};
              }
              if (args.value("activate", false))
              {
                  ctx.graph.activateConfiguration(*newId);
              }
              return {{"success", true}, {"configurationId", *newId}};
          });

        registry.registerSyncTool(
          {"add_variant_feedback",
           "Attach feedback or review comments to a design variant or configuration.",
           {{"type", "object"},
            {"required", {"targetId", "feedback"}},
            {"properties",
             {{"targetId", {{"type", "string"}}},
              {"feedback", {{"type", "string"}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              std::string const targetId = args.value("targetId", "");
              std::string const feedback = args.value("feedback", "");
              if (targetId.empty())
              {
                  return {{"success", false}, {"error", "targetId is required"}};
              }
              if (ctx.graph.setVariantFeedback(targetId, feedback)
                  || ctx.graph.setConfigurationFeedback(targetId, feedback)
                  || ctx.checkpoints.setFeedback(targetId, feedback))
              {
                  return {{"success", true}, {"targetId", targetId}};
              }
              return {{"success", false}, {"error", "Target ID not found: " + targetId}};
          });

        registry.registerSyncTool(
          {"score_component_reusability",
           "Score a finished component/function for engineering quality and library reusability (0.0-1.0).",
           {{"type", "object"},
            {"required", {"componentName", "qualityScore", "reusabilityScore"}},
            {"properties",
             {{"componentName", {{"type", "string"}}},
              {"qualityScore", {{"type", "number"}}},
              {"reusabilityScore", {{"type", "number"}}},
              {"rationale", {{"type", "string"}}},
              {"suggestedCategory", {{"type", "string"}}}}}},
           tools::ToolPolicy::ReadOnly},
          [](nlohmann::json const & args) -> nlohmann::json {
              double const quality = std::clamp(readNumber(args, "qualityScore", 0.0), 0.0, 1.0);
              double const reusability = std::clamp(readNumber(args, "reusabilityScore", 0.0), 0.0, 1.0);
              bool const recommendExport = (quality >= 0.75 && reusability >= 0.75);
              return {{"success", true},
                      {"componentName", args.value("componentName", "")},
                      {"qualityScore", quality},
                      {"reusabilityScore", reusability},
                      {"recommendLibraryExport", recommendExport},
                      {"suggestedCategory", args.value("suggestedCategory", "components")},
                      {"rationale", args.value("rationale", "")}};
          });

        registry.registerSyncTool(
          {"export_library_item",
           "Non-destructively export a reusable function from the active document into the user 3MF library without modifying the open document.",
           {{"type", "object"},
            {"required", {"functionId", "category", "name", "description"}},
            {"properties",
             {{"functionId", {{"type", "integer"}}},
              {"category", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"description", {{"type", "string"}}},
              {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}},
              {"overwrite", {{"type", "boolean"}}}}}},
           tools::ToolPolicy::ReadOnly},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.document || !ctx.document->getAssembly() || !ctx.document->get3mfModel())
              {
                  return {{"success", false}, {"error", "No active document"}};
              }
              uint32_t const functionId = args.value("functionId", 0u);
              std::string const category = args.value("category", "");
              std::string const name = args.value("name", "");
              std::string const description = args.value("description", "");
              bool const overwrite = args.value("overwrite", false);
              if (functionId == 0 || category.empty() || name.empty())
              {
                  return {{"success", false},
                          {"error", "functionId, category, and name are required"}};
              }

              auto const assembly = ctx.document->getAssembly();
              if (assembly->getFunctions().find(functionId) == assembly->getFunctions().end())
              {
                  return {{"success", false},
                          {"error", "Function ID not found in active document"}};
              }

              std::filesystem::path const targetDir = getUserLibraryDir() / category;
              std::filesystem::path const targetPath = targetDir / (name + ".3mf");
              std::error_code ec;
              if (std::filesystem::exists(targetPath, ec) && !overwrite)
              {
                  // Naming the existing entry tells the model the goal is already met, so it moves on
                  // instead of retrying the export for several more turns.
                  return {{"success", false},
                          {"error", "Library item already exists at " + targetPath.string()
                                    + ". The component is already reusable: import_library_item to use "
                                    "it, or call export_library_item again with overwrite=true only "
                                    "if the geometry actually changed."}};
              }

              try
              {
                  std::filesystem::create_directories(targetDir, ec);
                  std::filesystem::path const tempPath =
                    std::filesystem::temp_directory_path()
                    / ("gladius_lib_export_" + std::to_string(functionId) + ".3mf");

                  ctx.document->update3mfModel();
                  {
                      auto writer = ctx.document->get3mfModel()->QueryWriter("3mf");
                      writer->WriteToFile(tempPath.string());
                  }

                  auto wrapper = io::loadLib3mfScoped();
                  auto exportedModel = wrapper->CreateModel();
                  {
                      auto reader = exportedModel->QueryReader("3mf");
                      reader->ReadFromFile(tempPath.string());
                  }

                  io::LibraryMetadata meta;
                  meta.libraryFunctions =
                    io::serializeResourceIds({static_cast<Lib3MF_uint32>(functionId)});
                  meta.libraryDescription = description;
                  auto const tagsVec = readStringArray(args, "tags");
                  for (std::size_t i = 0; i < tagsVec.size(); ++i)
                  {
                      if (i > 0)
                      {
                          meta.libraryTags += ",";
                      }
                      meta.libraryTags += tagsVec[i];
                  }
                  io::writeLibraryMetadata(exportedModel, meta);

                  if (ctx.captureViewport)
                  {
                      auto pngBytes = ctx.captureViewport(256, 256, "iso");
                      if (!pngBytes.empty())
                      {
                          std::vector<Lib3MF_uint8> buf(pngBytes.size());
                          std::transform(pngBytes.begin(),
                                         pngBytes.end(),
                                         buf.begin(),
                                         [](std::byte b) { return static_cast<Lib3MF_uint8>(b); });
                          if (exportedModel->HasPackageThumbnailAttachment())
                          {
                              exportedModel->RemovePackageThumbnailAttachment();
                          }
                          auto thumb = exportedModel->CreatePackageThumbnailAttachment();
                          thumb->ReadFromBuffer(buf);
                      }
                  }

                  {
                      auto writer = exportedModel->QueryWriter("3mf");
                      writer->WriteToFile(targetPath.string());
                  }
                  std::filesystem::remove(tempPath, ec);

                  return {{"success", true},
                          {"category", category},
                          {"name", name},
                          {"path", targetPath.string()},
                          {"functionId", functionId}};
              }
              catch (std::exception const & ex)
              {
                  return {{"success", false}, {"error", ex.what()}};
              }
          });

        registry.registerSyncTool(
          {"create_todo_list",
           "Replace or initialize the planning todo list with structured items and optional checklists.",
           {{"type", "object"},
            {"required", {"items"}},
            {"properties",
             {{"items",
               {{"type", "array"},
                {"items",
                 {{"type", "object"},
                  {"required", {"title"}},
                  {"properties",
                   {{"title", {{"type", "string"}}},
                    {"description", {{"type", "string"}}},
                    {"checklist", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}}}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.todos)
              {
                  return {{"success", false}, {"error", "TodoStore unavailable"}};
              }
              ctx.todos->clear();
              nlohmann::json created = nlohmann::json::array();
              if (args.contains("items") && args["items"].is_array())
              {
                  for (auto const & item : args["items"])
                  {
                      std::vector<ChecklistEntry> checks;
                      for (auto const & c : readStringArray(item, "checklist"))
                      {
                          checks.push_back(ChecklistEntry{c, false});
                      }
                      std::string const id = ctx.todos->addItem(
                        item.value("title", ""), item.value("description", ""), std::move(checks));
                      created.push_back(id);
                  }
              }
              return {{"success", true}, {"todoIds", std::move(created)}};
          });

        registry.registerSyncTool(
          {"add_todo_item",
           "Add a single planning todo item with optional checklist steps.",
           {{"type", "object"},
            {"required", {"title"}},
            {"properties",
             {{"title", {{"type", "string"}}},
              {"description", {{"type", "string"}}},
              {"checklist", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.todos)
              {
                  return {{"success", false}, {"error", "TodoStore unavailable"}};
              }
              std::vector<ChecklistEntry> checks;
              for (auto const & c : readStringArray(args, "checklist"))
              {
                  checks.push_back(ChecklistEntry{c, false});
              }
              std::string const id = ctx.todos->addItem(
                args.value("title", ""), args.value("description", ""), std::move(checks));
              return {{"success", true}, {"todoId", id}};
          });

        registry.registerSyncTool(
          {"update_todo_item",
           "Update a planning todo item's status or checklist step completion.",
           {{"type", "object"},
            {"required", {"todoId", "status"}},
            {"properties",
             {{"todoId", {{"type", "string"}}},
              {"status", {{"type", "string"}, {"enum", {"pending", "in_progress", "done", "blocked"}}}},
              {"description", {{"type", "string"}}},
              {"checkIndex", {{"type", "integer"}}},
              {"checked", {{"type", "boolean"}}}}}},
           tools::ToolPolicy::DesignGraphModification},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.todos)
              {
                  return {{"success", false}, {"error", "TodoStore unavailable"}};
              }
              std::string const id = args.value("todoId", "");
              TodoStatus const st = todoStatusFromString(args.value("status", "pending"));
              bool ok = ctx.todos->updateItem(id, st, args.value("description", ""));
              if (ok && args.contains("checkIndex"))
              {
                  ok = ctx.todos->toggleChecklistEntry(
                    id, args.value("checkIndex", 0u), args.value("checked", true));
              }
              return {{"success", ok}, {"todoId", id}};
          });

        registry.registerSyncTool(
          {"list_todos",
           "List all planning todos and their checklist items.",
           {{"type", "object"}, {"properties", nlohmann::json::object()}},
           tools::ToolPolicy::ReadOnly},
          [&ctx](nlohmann::json const &) -> nlohmann::json {
              if (!ctx.todos)
              {
                  return {{"success", false}, {"error", "TodoStore unavailable"}};
              }
              nlohmann::json res = ctx.todos->toJson();
              res["success"] = true;
              return res;
          });

        registry.registerSyncTool(
          {"store_knowledge",
           "Store a concise, reusable engineering learning or pattern in persistent cross-platform knowledge storage.",
           {{"type", "object"},
            {"required", {"title", "summary"}},
            {"properties",
             {{"title", {{"type", "string"}}},
              {"category", {{"type", "string"}}},
              {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}},
              {"summary", {{"type", "string"}}}}}},
           tools::ToolPolicy::ReadOnly},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.knowledge)
              {
                  return {{"success", false}, {"error", "KnowledgeStore unavailable"}};
              }
              std::string const id = ctx.knowledge->store(
                args.value("title", ""),
                args.value("category", "general"),
                readStringArray(args, "tags"),
                args.value("summary", ""));
              return {{"success", true}, {"knowledgeId", id}};
          });

        registry.registerSyncTool(
          {"search_knowledge",
           "Search persistent agent knowledge and past learnings by keyword or category.",
           {{"type", "object"},
            {"properties",
             {{"query", {{"type", "string"}}},
              {"category", {{"type", "string"}}},
              {"limit", {{"type", "integer"}}}}}},
           tools::ToolPolicy::ReadOnly},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.knowledge)
              {
                  return {{"success", false}, {"error", "KnowledgeStore unavailable"}};
              }
              auto const hits = ctx.knowledge->search(
                args.value("query", ""),
                args.value("category", ""),
                static_cast<std::size_t>(std::max(1, args.value("limit", 8))));
              nlohmann::json arr = nlohmann::json::array();
              for (auto const & e : hits)
              {
                  arr.push_back({{"id", e.id},
                                 {"title", e.title},
                                 {"category", e.category},
                                 {"tags", e.tags},
                                 {"summary", e.summary},
                                 {"useCount", e.useCount}});
              }
              return {{"success", true}, {"count", arr.size()}, {"entries", std::move(arr)}};
          });

        registry.registerSyncTool(
          {"update_knowledge",
           "Update an existing persistent knowledge entry.",
           {{"type", "object"},
            {"required", {"id"}},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"title", {{"type", "string"}}},
              {"category", {{"type", "string"}}},
              {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}},
              {"summary", {{"type", "string"}}}}}},
           tools::ToolPolicy::ReadOnly},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.knowledge)
              {
                  return {{"success", false}, {"error", "KnowledgeStore unavailable"}};
              }
              bool const ok = ctx.knowledge->update(
                args.value("id", ""),
                args.value("title", ""),
                args.value("category", ""),
                readStringArray(args, "tags"),
                args.value("summary", ""));
              return {{"success", ok}};
          });

        registry.registerSyncTool(
          {"delete_knowledge",
           "Delete an outdated or incorrect knowledge entry by ID.",
           {{"type", "object"},
            {"required", {"id"}},
            {"properties", {{"id", {{"type", "string"}}}}}},
           tools::ToolPolicy::ReadOnly},
          [&ctx](nlohmann::json const & args) -> nlohmann::json {
              if (!ctx.knowledge)
              {
                  return {{"success", false}, {"error", "KnowledgeStore unavailable"}};
              }
              return {{"success", ctx.knowledge->remove(args.value("id", ""))}};
          });
    }
} // namespace gladius::design
