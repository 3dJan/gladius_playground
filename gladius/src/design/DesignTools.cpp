#include "DesignTools.h"

#include "../Document.h"
#include "../agent/Conversation.h"
#include "../nodes/Assembly.h"

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
              std::string const id =
                ctx.checkpoints.createCheckpoint(args.value("label", ""), args.value("level", 0), target);
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
              return {{"success", true},
                      {"valid", true},
                      {"phase", toString(ctx.graph.phase())},
                      {"passed", eval.deterministicPassed}};
          });
    }
} // namespace gladius::design
