#include "agent/providers/ImageProvider.h"
#include "design/DesignCatalogue.h"
#include "design/DesignGraph.h"
#include "design/DesignTools.h"
#include "design/ManufacturingValidator.h"
#include "design/TodoStore.h"

#include <gtest/gtest.h>

namespace gladius::design::tests
{
    TEST(DesignGraph, FreezeAndHybridize_WithFrozenNode_PreservesFrozenVariant)
    {
        // Arrange
        DesignGraph graph;
        auto const standId = graph.addNode(DesignNodeType::Component, "Stand");
        auto const shadeId = graph.addNode(DesignNodeType::Component, "Shade");

        auto const standA = graph.createVariant(standId, "Tripod", "Stable", FidelityLevel::Envelope);
        auto const standB = graph.createVariant(standId, "Pedestal", "Compact", FidelityLevel::Envelope);
        auto const shadeA = graph.createVariant(shadeId, "Cone", "Simple", FidelityLevel::Envelope);
        auto const shadeB = graph.createVariant(shadeId, "Ribbed", "Diffused", FidelityLevel::Envelope);

        auto const cfgA = graph.createConfiguration("Concept A", {{standId, standA}, {shadeId, shadeA}});
        auto const cfgB = graph.createConfiguration("Concept B", {{standId, standB}, {shadeId, shadeB}});

        ASSERT_TRUE(graph.activateConfiguration(cfgA));
        ASSERT_TRUE(graph.freezeNode(standId));

        // Act: hybridize Concept A + Concept B attempting to override both, standId is frozen
        auto const hybridId = graph.hybridizeConfigurations("Hybrid", cfgA, cfgB, {standId, shadeId});
        ASSERT_TRUE(hybridId.has_value());
        ASSERT_TRUE(graph.activateConfiguration(*hybridId));

        // Assert
        auto const active = graph.activeConfiguration();
        ASSERT_TRUE(active.has_value());
        EXPECT_EQ(active->selectedVariants.at(standId), standA);
        EXPECT_EQ(active->selectedVariants.at(shadeId), shadeB);
    }

    TEST(DesignCatalogue, LoadAndPruneBeam_IncompatibleCombination_FiltersAndRanks)
    {
        // Arrange
        DesignCatalogue catalogue;
        nlohmann::json doc = {
            {"entries", {{
                {"id", "lamp.shade.ribbed"},
                {"functions", {"diffuse_light"}},
                {"parameters", {{"wallThickness", {{"type", "length"}, {"minimum", 1.2}, {"maximum", 4.0}}}}},
                {"manufacturing", {{"processes", {"FDM"}}, {"supportsRequired", false}}},
                {"interfaces", {"stand_mount"}}
            }}}
        };
        ASSERT_TRUE(catalogue.loadFromJson(doc));
        catalogue.addCompatibilityRule({{{"shade", "heavy_glass"}, {"stand", "thin_tripod"}}, false, "Too top-heavy"});

        std::vector<std::string> paramErrors;
        EXPECT_TRUE(catalogue.validateParameters("lamp.shade.ribbed", {{"wallThickness", 1.6}}, paramErrors));
        EXPECT_FALSE(catalogue.validateParameters("lamp.shade.ribbed", {{"wallThickness", 0.5}}, paramErrors));

        Configuration c1{"c1", "Bad", {{"shade", "heavy_glass"}, {"stand", "thin_tripod"}}, Evaluation{{}, {}, {}, {}, 10.0}, false};
        Configuration c2{"c2", "Good Low", {{"shade", "ribbed"}, {"stand", "pedestal"}}, Evaluation{{}, {}, {}, {}, 5.0}, false};
        Configuration c3{"c3", "Good High", {{"shade", "cone"}, {"stand", "pedestal"}}, Evaluation{{}, {}, {}, {}, 8.0}, false};

        // Act
        auto const beam = catalogue.pruneBeam({c1, c2, c3}, 2);

        // Assert
        ASSERT_EQ(beam.size(), 2u);
        EXPECT_EQ(beam[0].id, "c3");
        EXPECT_EQ(beam[1].id, "c2");
    }

    TEST(DesignGraph, UpdateAndRemoveRequirement_TracksPendingChangesAndClearsOnConsume)
    {
        // Arrange
        DesignGraph graph;
        auto const r1 = graph.addRequirement("Height >= 50mm", "dimensions", true);
        auto const r2 = graph.addRequirement("PLA printable", "manufacturing", true);

        // Act
        ASSERT_TRUE(graph.updateRequirement(r1, "Height >= 60mm", "dimensions", true, true));
        ASSERT_TRUE(graph.removeRequirement(r2, true));
        EXPECT_TRUE(graph.hasPendingRequirementChanges());
        auto const changes = graph.consumePendingRequirementChanges();

        // Assert
        ASSERT_EQ(changes.size(), 2u);
        EXPECT_FALSE(graph.hasPendingRequirementChanges());
        ASSERT_EQ(graph.requirements().size(), 1u);
        EXPECT_EQ(graph.requirements()[0].description, "Height >= 60mm");
    }

    TEST(DesignGraph, ReopenForFollowUpRequest_CompletedDesign_ResetsPhaseAndRecordsChange)
    {
        // Arrange
        DesignGraph graph;
        graph.addRequirement("Height >= 50mm", "dimensions", true);
        graph.setPhase(DesignPhase::Complete);

        // Act
        ASSERT_TRUE(graph.reopenForFollowUpRequest("Build a house out of the brick I added"));

        // Assert
        EXPECT_EQ(graph.phase(), DesignPhase::Refine);
        EXPECT_TRUE(graph.hasPendingRequirementChanges());
        auto const changes = graph.consumePendingRequirementChanges();
        ASSERT_EQ(changes.size(), 1u);
        EXPECT_NE(changes[0].find("Build a house out of the brick I added"), std::string::npos);
        EXPECT_EQ(graph.requirements().size(), 1u);
    }

    TEST(DesignGraph, ReopenForFollowUpRequest_IncompleteDesign_DoesNothing)
    {
        // Arrange
        DesignGraph graph;
        graph.setPhase(DesignPhase::DevelopComponents);

        // Act / Assert
        EXPECT_FALSE(graph.reopenForFollowUpRequest("Make it taller"));
        EXPECT_EQ(graph.phase(), DesignPhase::DevelopComponents);
        EXPECT_FALSE(graph.hasPendingRequirementChanges());
    }

    TEST(DesignGraph, ToAndFromJson_RoundTripsFeedbackAndConfigurations)
    {
        // Arrange
        DesignGraph graph;
        auto const n1 = graph.addNode(DesignNodeType::Component, "Base");
        auto const v1 = graph.createVariant(n1, "Hex", "Strong", FidelityLevel::Structural);
        ASSERT_TRUE(graph.setVariantFeedback(v1, "Looks sturdy"));
        auto const c1 = graph.createConfiguration("Config 1", {{n1, v1}});
        ASSERT_TRUE(graph.setConfigurationFeedback(c1, "Prefer thinner walls"));
        ASSERT_TRUE(graph.activateConfiguration(c1));

        // Act
        nlohmann::json const serialized = graph.toJson();
        DesignGraph restored;
        ASSERT_TRUE(restored.fromJson(serialized));

        // Assert
        ASSERT_EQ(restored.variants().size(), 1u);
        EXPECT_EQ(restored.variants().at(v1).userFeedback, "Looks sturdy");
        ASSERT_TRUE(restored.activeConfiguration().has_value());
        EXPECT_EQ(restored.activeConfiguration()->userFeedback, "Prefer thinner walls");
    }

    TEST(DesignGraph, SetGoal_JsonRoundTrip_SummarizesGoal)
    {
        // Arrange
        DesignGraph graph;
        graph.setGoal("Periodic filter disc, 100 mm envelope");

        // Act
        nlohmann::json const serialized = graph.toJson();
        DesignGraph restored;
        ASSERT_TRUE(restored.fromJson(serialized));

        // Assert
        EXPECT_EQ(restored.goal(), "Periodic filter disc, 100 mm envelope");
        EXPECT_NE(restored.summarize().find("Goal: Periodic filter disc"), std::string::npos);

        // Documents saved before the goal existed stay loadable and simply have no goal.
        DesignGraph legacy;
        nlohmann::json legacyJson = serialized;
        legacyJson.erase("goal");
        ASSERT_TRUE(legacy.fromJson(legacyJson));
        EXPECT_TRUE(legacy.goal().empty());
        EXPECT_EQ(legacy.summarize().find("Goal:"), std::string::npos);
    }

    TEST(TodoStore, AddUpdateAndJson_RoundTripsChecklistState)
    {
        // Arrange
        TodoStore store;
        auto const id = store.addItem(
          "Build base",
          "Create mounting flange",
          {ChecklistEntry{"Sketch", false}, ChecklistEntry{"Validate", false}});

        // Act
        ASSERT_TRUE(store.updateItem(id, TodoStatus::InProgress));
        ASSERT_TRUE(store.toggleChecklistEntry(id, 0, true));
        nlohmann::json const j = store.toJson();
        TodoStore restored;
        ASSERT_TRUE(restored.fromJson(j));

        // Assert
        ASSERT_EQ(restored.items().size(), 1u);
        EXPECT_EQ(restored.items()[0].status, TodoStatus::InProgress);
        EXPECT_TRUE(restored.items()[0].checklist[0].checked);
        EXPECT_FALSE(restored.items()[0].checklist[1].checked);
    }

    TEST(DesignGraph, SubstituteVariant_InterfaceMismatch_RejectedAndPreservesConfig)
    {
        // Arrange
        DesignGraph graph;
        auto const standId = graph.addNode(DesignNodeType::Component, "Stand");
        auto const shadeId = graph.addNode(DesignNodeType::Component, "Shade");
        auto const standA = graph.createVariant(standId, "Pedestal", "Base", FidelityLevel::Envelope);
        auto const shadeA = graph.createVariant(shadeId, "Cone", "Conical", FidelityLevel::Envelope);
        auto const shadeB = graph.createVariant(shadeId, "Ribbed", "Ribbed", FidelityLevel::Envelope);

        ASSERT_TRUE(graph.defineInterface(standA, {"if1", "spigot", "circular", {{"diameter", 30.0}}}));
        ASSERT_TRUE(graph.defineInterface(shadeA, {"if2", "spigot", "circular", {{"diameter", 30.0}}}));
        ASSERT_TRUE(graph.defineInterface(shadeB, {"if3", "spigot", "circular", {{"diameter", 45.0}}}));

        auto const cfgId = graph.createConfiguration("Concept A", {{standId, standA}, {shadeId, shadeA}});
        ASSERT_TRUE(graph.activateConfiguration(cfgId));

        // Act
        std::string err;
        bool const ok = graph.substituteVariant(cfgId, shadeId, shadeB, err);

        // Assert
        EXPECT_FALSE(ok);
        EXPECT_NE(err.find("Interface mismatch"), std::string::npos);
        EXPECT_EQ(graph.activeConfiguration()->selectedVariants.at(shadeId), shadeA);
    }

    TEST(DesignCatalogue, LoadDefaultCatalogue_PopulatesEntriesAndCompatibilityRules)
    {
        // Arrange
        DesignCatalogue catalogue;

        // Act
        ASSERT_TRUE(catalogue.loadDefaultCatalogue());

        // Assert
        EXPECT_GE(catalogue.size(), 10u);
        EXPECT_FALSE(catalogue.findByFunction("Enclosure").empty());
        EXPECT_FALSE(catalogue.findByFunction("Stand").empty());
        std::vector<std::string> violations;
        EXPECT_FALSE(catalogue.checkCompatibility(
          {{"Stand", "stand.clamp.desk"}, {"Shade", "shade.diffuser.ribbed"}}, violations));
        EXPECT_FALSE(violations.empty());
    }

    TEST(ManufacturingValidator, AnalyzeTriangles_ThinBox_DetectsWallThicknessAndBounds)
    {
        // Arrange: closed axis-aligned box [0, 20] x [0, 10] x [0, 0.8] (thin 0.8mm in Z)
        std::vector<MeshTriangle> tris;
        auto addQuad = [&](std::array<double, 3> a,
                           std::array<double, 3> b,
                           std::array<double, 3> c,
                           std::array<double, 3> d,
                           std::array<double, 3> n) {
            tris.push_back(MeshTriangle{{a, b, c}, n});
            tris.push_back(MeshTriangle{{a, c, d}, n});
        };
        double const X = 20.0;
        double const Y = 10.0;
        double const Z = 0.8;
        addQuad({0, 0, 0}, {0, Y, 0}, {X, Y, 0}, {X, 0, 0}, {0, 0, -1});
        addQuad({0, 0, Z}, {X, 0, Z}, {X, Y, Z}, {0, Y, Z}, {0, 0, 1});
        addQuad({0, 0, 0}, {X, 0, 0}, {X, 0, Z}, {0, 0, Z}, {0, -1, 0});
        addQuad({0, Y, 0}, {0, Y, Z}, {X, Y, Z}, {X, Y, 0}, {0, 1, 0});
        addQuad({0, 0, 0}, {0, 0, Z}, {0, Y, Z}, {0, Y, 0}, {-1, 0, 0});
        addQuad({X, 0, 0}, {X, Y, 0}, {X, Y, Z}, {X, 0, Z}, {1, 0, 0});

        PrintabilityLimits limits;
        limits.minWallThicknessMm = 1.2;

        // Act
        auto const report = ManufacturingValidator::analyzeTriangles(tris, limits);

        // Assert
        EXPECT_TRUE(report.hasBounds);
        EXPECT_TRUE(report.withinBuildVolume);
        EXPECT_TRUE(report.hasMeshMetrics);
        EXPECT_NEAR(report.dimensionsMm[2], 0.8, 1e-6);
        EXPECT_NEAR(report.minWallThicknessMm, 0.8, 1e-3);
        EXPECT_FALSE(report.warnings.empty());
    }

    TEST(DesignTools, DecompositionCompareAndForkTools_EndToEnd_Succeeds)
    {
        // Arrange
        DesignGraph graph;
        DesignCatalogue catalogue;
        ASSERT_TRUE(catalogue.loadDefaultCatalogue());
        tools::TransactionManager tx;
        tools::CheckpointManager checkpoints;
        DesignToolContext ctx{
          graph,
          catalogue,
          tx,
          checkpoints,
          nullptr,
          [](int, int, std::string const &) {
              return std::vector<std::byte>{std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47}};
          },
          nullptr};
        tools::ToolRegistry registry;
        registerDesignTools(registry, ctx, DesignToolSurface::Full);

        auto callTool = [&](std::string const & name, nlohmann::json const & args) {
            auto tool = registry.getTool(name);
            EXPECT_NE(tool, nullptr) << "Missing tool: " << name;
            return coro::sync_wait(tool->execute(args, io::CancellationToken{}));
        };

        // Act & Assert: decomposition + resource link
        auto const sys = callTool("add_function", {{"type", "system"}, {"name", "Lamp"}});
        auto const stand = callTool("add_function",
                                    {{"type", "component"}, {"name", "Stand"}, {"parentNodeId", sys["nodeId"]}});
        auto const v1 = callTool("create_variant",
                                 {{"nodeId", stand["nodeId"]},
                                  {"name", "Pedestal"},
                                  {"functionName", "stand_pedestal"},
                                  {"modelResourceId", 10}});
        auto const v2 = callTool("create_variant",
                                 {{"nodeId", stand["nodeId"]},
                                  {"name", "Tripod"},
                                  {"functionName", "stand_tripod"},
                                  {"modelResourceId", 11}});
        ASSERT_TRUE(v1["success"].get<bool>());
        ASSERT_TRUE(v2["success"].get<bool>());

        auto const children = callTool("get_children", {{"nodeId", sys["nodeId"]}});
        ASSERT_EQ(children["children"].size(), 1u);

        auto const nodeInfo = callTool("get_node", {{"nodeId", stand["nodeId"]}});
        ASSERT_EQ(nodeInfo["variants"].size(), 2u);

        auto const catRes = callTool("query_catalogue", {{"function", "Stand"}});
        EXPECT_GE(catRes["count"].get<int>(), 2);

        auto const cmpVars = callTool("compare_variants", {{"nodeId", stand["nodeId"]}});
        ASSERT_EQ(cmpVars["variants"].size(), 2u);

        auto const cfg1 = callTool("create_configuration",
                                   {{"name", "Concept A"}, {"selectedVariants", {{stand["nodeId"], v1["variantId"]}}}});
        auto const cfg2 = callTool("create_configuration",
                                   {{"name", "Concept B"}, {"selectedVariants", {{stand["nodeId"], v2["variantId"]}}}});
        ASSERT_TRUE(callTool("activate_configuration", {{"configurationId", cfg1["configurationId"]}})["success"].get<bool>());

        auto const activeCfg = callTool("get_active_configuration", nlohmann::json::object());
        EXPECT_TRUE(activeCfg["active"].get<bool>());
        EXPECT_EQ(activeCfg["configurationId"], cfg1["configurationId"]);

        auto const sub = callTool("substitute_variant",
                                  {{"nodeId", stand["nodeId"]}, {"variantId", v2["variantId"]}});
        EXPECT_TRUE(sub["success"].get<bool>());

        auto const cap = callTool("capture_viewport", {{"variant", v2["variantId"]}});
        EXPECT_TRUE(cap["success"].get<bool>());
        EXPECT_EQ(cap["functionName"], "stand_tripod");

        auto const beam = callTool("prune_beam", {{"beamWidth", 1}});
        ASSERT_EQ(beam["survivors"].size(), 1u);
        (void)cfg2;

        auto const cp = callTool("create_checkpoint", {{"label", "Baseline"}, {"level", 1}});
        auto const cpDiff = callTool("compare_checkpoint", {{"checkpointId", cp["checkpointId"]}});
        EXPECT_TRUE(cpDiff["success"].get<bool>());

        auto const fork = callTool("fork_from_checkpoint",
                                   {{"checkpointId", cp["checkpointId"]}, {"name", "Branch B"}});
        EXPECT_TRUE(fork["success"].get<bool>());
        EXPECT_EQ(graph.activeConfiguration()->id, fork["configurationId"].get<std::string>());
    }

    namespace
    {
        class MockImageHttpTransport final : public agent::IHttpTransport
        {
          public:
            std::string lastUrl;
            std::string lastBody;
            agent::HttpResponse nextResponse{200, "", "", false};

            agent::HttpResponse postJson(agent::HttpRequest const & request,
                                         io::CancellationToken const &) override
            {
                lastUrl = request.url;
                lastBody = request.body;
                return nextResponse;
            }

            agent::HttpResponse postSse(agent::HttpRequest const &,
                                        agent::SseEventCallback,
                                        io::CancellationToken const &) override
            {
                return agent::HttpResponse{0, "", "unused", false};
            }
        };
    } // namespace

    TEST(ImageProvider, AllBackends_ParseExpectedResponses)
    {
        // "iVBORw==" decodes to PNG magic bytes 0x89, 0x50, 0x4E, 0x47
        std::string const pngB64 = "iVBORw==";
        auto transport = std::make_shared<MockImageHttpTransport>();

        // 1. OpenAI-compatible
        transport->nextResponse = {200, nlohmann::json{{"data", {{{"b64_json", pngB64}}}}}.dump(), "", false};
        agent::ImageProviderConfig openAiCfg{
          agent::ImageApiType::OpenAiCompatible, "https://api.example.com/v1", "k", "gpt-image-1", "512x512"};
        auto openAi = agent::createImageProvider(openAiCfg, transport);
        auto const r1 = coro::sync_wait(openAi->generate({"a desk lamp", "512x512"}, io::CancellationToken{}));
        EXPECT_TRUE(r1.success);
        EXPECT_EQ(transport->lastUrl, "https://api.example.com/v1/images/generations");
        ASSERT_GE(r1.image.data.size(), 4u);
        EXPECT_EQ(r1.image.data[0], std::byte{0x89});

        // 2. SD WebUI (A1111/Forge)
        transport->nextResponse = {200, nlohmann::json{{"images", {"data:image/png;base64," + pngB64}}}.dump(), "", false};
        agent::ImageProviderConfig sdCfg{
          agent::ImageApiType::SdWebUi, "http://127.0.0.1:7860", "", "sdxl", "768x512"};
        auto sd = agent::createImageProvider(sdCfg, transport);
        auto const r2 = coro::sync_wait(sd->generate({"a desk lamp", "768x512"}, io::CancellationToken{}));
        EXPECT_TRUE(r2.success);
        EXPECT_EQ(transport->lastUrl, "http://127.0.0.1:7860/sdapi/v1/txt2img");
        auto const sdPayload = nlohmann::json::parse(transport->lastBody);
        EXPECT_EQ(sdPayload["width"].get<int>(), 768);
        EXPECT_EQ(sdPayload["height"].get<int>(), 512);

        // 3. Google Gemini / Imagen
        transport->nextResponse = {
          200, nlohmann::json{{"predictions", {{{"bytesBase64Encoded", pngB64}, {"mimeType", "image/png"}}}}}.dump(), "", false};
        agent::ImageProviderConfig gemCfg{
          agent::ImageApiType::GeminiImagen, "", "goog-key", "imagen-3.0-generate-002", "1024x1024"};
        auto gem = agent::createImageProvider(gemCfg, transport);
        auto const r3 = coro::sync_wait(gem->generate({"a desk lamp", "1024x1024"}, io::CancellationToken{}));
        EXPECT_TRUE(r3.success);
        EXPECT_NE(transport->lastUrl.find(":predict"), std::string::npos);
        EXPECT_EQ(r3.image.data[1], std::byte{0x50});
    }

    TEST(DesignGraph, ReferenceImages_RoundTripsJson)
    {
        DesignGraph graph;
        auto const refId = graph.addReferenceImage(
          "minimalist lamp",
          "Concept sketch",
          {std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47}},
          "image/png",
          "node_1",
          "var_1");
        ASSERT_FALSE(refId.empty());

        auto const json = graph.toJson();
        DesignGraph restored;
        ASSERT_TRUE(restored.fromJson(json));
        ASSERT_EQ(restored.referenceImages().size(), 1u);
        auto const * ref = restored.findReferenceImage(refId);
        ASSERT_NE(ref, nullptr);
        EXPECT_EQ(ref->prompt, "minimalist lamp");
        EXPECT_EQ(ref->linkedNodeId, "node_1");
        EXPECT_EQ(ref->pngBytes.size(), 4u);
    }

    TEST(DesignTools, GenerateReferenceImage_StoresInDesignGraph)
    {
        DesignGraph graph;
        DesignCatalogue catalogue;
        tools::TransactionManager tx;
        tools::CheckpointManager checkpoints;

        auto transport = std::make_shared<MockImageHttpTransport>();
        transport->nextResponse = {200, nlohmann::json{{"data", {{{"b64_json", "iVBORw=="}}}}}.dump(), "", false};
        agent::ImageProviderConfig cfg{agent::ImageApiType::OpenAiCompatible, "https://api.example.com/v1", "k", "gpt-image-1", "512x512"};

        DesignToolContext ctx{graph, catalogue, tx, checkpoints};
        ctx.generateImage = [cfg, transport](agent::ImageGenerationRequest const & req,
                                             io::CancellationToken const & cancel)
          -> coro::task<agent::ImageGenerationResponse> {
            auto provider = agent::createImageProvider(cfg, transport);
            co_return co_await provider->generate(req, cancel);
        };

        tools::ToolRegistry registry;
        registerDesignTools(registry, ctx, DesignToolSurface::Full);
        auto tool = registry.getTool("generate_reference_image");
        ASSERT_NE(tool, nullptr);

        auto const res = coro::sync_wait(tool->execute(
          {{"prompt", "sculptural bracket"}, {"caption", "Bracket concept"}},
          io::CancellationToken{}));
        EXPECT_TRUE(res["success"].get<bool>());
        EXPECT_EQ(graph.referenceImages().size(), 1u);
        EXPECT_EQ(graph.referenceImages()[0].caption, "Bracket concept");
    }
} // namespace gladius::design::tests
