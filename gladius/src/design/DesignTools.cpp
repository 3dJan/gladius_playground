#include "DesignTools.h"

#include "../Document.h"
#include "../EventLogger.h"
#include "../ExpressionParser.h"
#include "../ExpressionToGraphConverter.h"
#include "../FileSystemUtils.h"
#include "../ProgramSnippetApplier.h"
#include "../agent/Conversation.h"
#include "../io/3mf/Lib3mfLoader.h"
#include "../io/3mf/LibraryMetadata.h"
#include "../nodes/Assembly.h"
#include "../nodes/GraphOptimizer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

namespace gladius::design
{
    namespace
    {
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
                eval.deterministicWarnings.push_back("Some design nodes lack a selected variant");
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
              nlohmann::json res = ctx.graph.toJson();
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
                  return {{"success", false}, {"error", "Unknown nodeId: " + nodeId}};
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
              return ok ? nlohmann::json{{"success", true}}
                        : nlohmann::json{{"success", false}, {"error", "Unknown variantId: " + vid}};
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
              return ok ? nlohmann::json{{"success", true}}
                        : nlohmann::json{{"success", false}, {"error", "Unknown nodeId: " + nid}};
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
              return ok ? nlohmann::json{{"success", true}, {"configurationId", cid}}
                        : nlohmann::json{{"success", false}, {"error", "Unknown configurationId: " + cid}};
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
                  return {{"success", false}, {"error", "No active configuration to validate"}};
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
           "Read the current open document's implicit function program listing.",
           {{"type", "object"}, {"properties", nlohmann::json::object()}},
           tools::ToolPolicy::ReadOnly},
          [&ctx](nlohmann::json const &) -> nlohmann::json {
              if (!ctx.document || !ctx.document->getAssembly())
              {
                  return {{"success", false}, {"error", "No active document"}};
              }
              auto const assembly = ctx.document->getAssembly();
              std::string const snippet =
                ExpressionToGraphConverter::convertProgramToSnippet(*assembly);
              return {{"success", true}, {"snippet", snippet}};
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
                                  {"failed", std::move(errs)}};
                      }
                      for (auto & [fid, funcModel] : assembly->getFunctions())
                      {
                          if (funcModel)
                          {
                              (void) nodes::GraphOptimizer::optimize(*funcModel);
                          }
                      }
                      appliedCount = report.parsed.size();
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
                              {"snippet", normalizedSnippet}};
                  }
                  return {{"success", true},
                          {"appliedFunctions", appliedCount},
                          {"snippet", normalizedSnippet}};
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
    }
} // namespace gladius::design
