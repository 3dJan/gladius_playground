/**
 * @file CadGates_tests.cpp
 * @brief Tests for the deterministic implicit-CAD gates and the checklist they prove.
 */

#include "Document.h"
#include "EventLogger.h"
#include "design/CadGates.h"
#include "design/DesignCatalogue.h"
#include "design/DesignGraph.h"
#include "design/DesignTools.h"
#include "design/InterfaceCompliance.h"
#include "design/TemplateSeed.h"
#include "design/TodoStore.h"
#include "io/CancellationToken.h"
#include "tools/CheckpointManager.h"
#include "tools/ToolExecutor.h"
#include "tools/TransactionManager.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace gladius::design::tests
{
    namespace
    {
        /// @brief Facts carrying measured bounds, so gates can be tested without a GPU probe
        RenderabilityFacts factsWithBounds(std::array<double, 6> const & minMax)
        {
            RenderabilityFacts facts;
            facts.buildItemCount = 1;
            facts.levelSetCount = 1;
            facts.rootFunctionId = 3;
            facts.boundsUsable = true;
            facts.boundsNumeric = true;
            facts.boundsMinMax = minMax;
            return facts;
        }

        /// @brief Parent/child variants with an interface contract, wired into an active configuration
        struct ContractHarness
        {
            DesignGraph graph;
            std::string parentId;
            std::string childId;
            std::string parentVariantId;
            std::string childVariantId;
            std::string configId;

            explicit ContractHarness(nlohmann::json const & parentSpec, nlohmann::json const & childSpec)
                : parentId(graph.addNode(DesignNodeType::Component, "Chassis"))
                , childId(graph.addNode(DesignNodeType::Component, "Wheel", parentId))
                , parentVariantId(graph.createVariant(parentId, "Frame", "Box frame", FidelityLevel::Envelope))
                , childVariantId(graph.createVariant(childId, "Tyre", "Ring", FidelityLevel::Envelope))
            {
                EXPECT_TRUE(graph.defineInterface(
                  parentVariantId, {"if_p", "mount", "bore", parentSpec}));
                EXPECT_TRUE(graph.defineInterface(
                  childVariantId, {"if_c", "mount", "bore", childSpec}));
                configId = graph.createConfiguration("Concept A",
                                                    {{parentId, parentVariantId},
                                                     {childId, childVariantId}});
                EXPECT_TRUE(graph.activateConfiguration(configId));
            }

            /// @brief Copy of the activated configuration (the graph returns it by value)
            Configuration active() const
            {
                return graph.activeConfiguration().value();
            }
        };
    } // namespace

    // --- Domain bounds -------------------------------------------------------

    TEST(CadGates_DomainBounds, UnmeasuredBounds_FailsInsteadOfAssumingFit)
    {
        RenderabilityFacts facts;
        facts.boundsNumeric = false;

        auto const result = checkDomainBounds(facts);

        EXPECT_FALSE(result.passed);
        EXPECT_NE(result.message.find("could not be measured"), std::string::npos);
    }

    TEST(CadGates_DomainBounds, GeometryReachingTheDomain_FailsWithOverflow)
    {
        auto const result = checkDomainBounds(factsWithBounds({-50.0, -20.0, -10.0, 50.0, 20.0, 10.0}));

        EXPECT_FALSE(result.passed);
        EXPECT_DOUBLE_EQ(result.evidence.at("overflowMm").get<double>(), 0.0);
    }

    TEST(CadGates_DomainBounds, GeometryInsideTheDomain_PassesWithHeadroom)
    {
        auto const result = checkDomainBounds(factsWithBounds({-40.0, -20.0, -10.0, 30.0, 20.0, 10.0}));

        ASSERT_TRUE(result.passed);
        EXPECT_DOUBLE_EQ(result.evidence.at("headroomMm").get<double>(), 10.0);
    }

    // --- Geometry presence ---------------------------------------------------

    TEST(CadGates_GeometryPresence, DegenerateBounds_FailsAsNoInteriorVolume)
    {
        auto const result = checkGeometryPresence(factsWithBounds({-10.0, 5.0, -3.0, 10.0, 5.0, 3.0}));

        EXPECT_FALSE(result.passed);
        EXPECT_DOUBLE_EQ(result.evidence.at("smallestSpanMm").get<double>(), 0.0);
    }

    TEST(CadGates_GeometryPresence, MeasurableVolume_Passes)
    {
        auto const result = checkGeometryPresence(factsWithBounds({-10.0, -5.0, -3.0, 10.0, 5.0, 3.0}));

        ASSERT_TRUE(result.passed);
        EXPECT_DOUBLE_EQ(result.evidence.at("smallestSpanMm").get<double>(), 6.0);
    }

    TEST(CadGates_GeometryPresence, UnmeasuredBounds_FailsAsUnproven)
    {
        RenderabilityFacts facts;
        auto const result = checkGeometryPresence(facts);

        EXPECT_FALSE(result.passed);
        EXPECT_NE(result.message.find("no geometry is proven"), std::string::npos);
    }

    // --- Interface compliance ------------------------------------------------

    TEST(InterfaceCompliance_NumericValues, WithinTolerance_AreCompatible)
    {
        Interface const mount{{}, "mount", "bore", {{"diameter", 4.8}}};
        Interface const same{{}, "mount", "bore", {{"diameter", 4.800001}}};

        auto const result = checkInterfacePair(mount, same);

        EXPECT_TRUE(result.compatible) << result.describe();
        EXPECT_TRUE(result.mismatches.empty());
    }

    TEST(InterfaceCompliance_NumericValues, OutsideTolerance_AreRejectedAndNamed)
    {
        Interface const mount{{}, "mount", "bore", {{"diameter", 4.8}}};
        Interface const loose{{}, "mount", "bore", {{"diameter", 6.0}}};

        auto const result = checkInterfacePair(mount, loose);

        EXPECT_FALSE(result.compatible);
        ASSERT_EQ(result.mismatches.size(), 1u);
        EXPECT_NE(result.describe().find("'mount' (bore)"), std::string::npos) << result.describe();
    }

    TEST(InterfaceCompliance_NonNumericValues, DifferentStrings_AreRejected)
    {
        Interface const metric{{}, "thread", "screw", {{"profile", "M4"}}};
        Interface const imperial{{}, "thread", "screw", {{"profile", "UNC"}}};

        EXPECT_FALSE(checkInterfacePair(metric, imperial).compatible);
    }

    TEST(InterfaceCompliance_OneSidedKeys, AreUnconstrainedNotMismatched)
    {
        Interface const detailed{{}, "mount", "bore", {{"diameter", 4.8}, {"depth", 12.0}}};
        Interface const sparse{{}, "mount", "bore", {{"diameter", 4.8}}};

        auto const result = checkInterfacePair(detailed, sparse);

        EXPECT_TRUE(result.compatible) << result.describe();
        EXPECT_EQ(result.unconstrainedKeys.size(), 1u);
    }

    TEST(InterfaceCompliance_DifferentNames, AreNeverCompared)
    {
        Interface const a{{}, "mount", "bore", {{"diameter", 4.8}}};
        Interface const b{{}, "axle", "bore", {{"diameter", 90.0}}};

        EXPECT_TRUE(checkInterfacePair(a, b).compatible);
        EXPECT_TRUE(checkInterfacesAgainstConfiguration({a}, {b}).empty());
    }

    TEST(InterfaceCompliance_Envelope, ChildOutsideParentIsRejected)
    {
        auto const result = checkEnvelopeContainment(
          {{"envelope", {-20.0, -20.0, -20.0, 20.0, 20.0, 20.0}}},
          {{"envelope", {-30.0, -5.0, -5.0, 30.0, 5.0, 5.0}}});

        EXPECT_FALSE(result.compatible);
        EXPECT_NE(result.describe().find("sticks out"), std::string::npos);
    }

    TEST(InterfaceCompliance_Envelope, ChildInsideParentIsAccepted)
    {
        auto const result = checkEnvelopeContainment(
          {{"envelope", {-20.0, -20.0, -20.0, 20.0, 20.0, 20.0}}},
          {{"envelope", {{-10.0, -5.0, -5.0}, {10.0, 5.0, 5.0}}}});

        EXPECT_TRUE(result.compatible) << result.describe();
    }

    TEST(InterfaceCompliance_Envelope, MissingEnvelopeIsUnconstrained)
    {
        auto const result = checkEnvelopeContainment({{"diameter", 4.8}}, {{"diameter", 4.8}});

        EXPECT_TRUE(result.compatible);
        EXPECT_TRUE(result.mismatches.empty());
    }

    TEST(DesignGraph, SubstituteVariant_EnvelopeViolation_Rejected)
    {
        // Arrange: the parent reserves a 40 mm box, the replacement wheel needs 60 mm
        DesignGraph graph;
        auto const parentId = graph.addNode(DesignNodeType::Component, "Chassis");
        auto const wheelId = graph.addNode(DesignNodeType::Component, "Wheel", parentId);
        auto const frame = graph.createVariant(parentId, "Frame", "Box", FidelityLevel::Envelope);
        auto const tyreA = graph.createVariant(wheelId, "TyreA", "Slim", FidelityLevel::Envelope);
        auto const tyreB = graph.createVariant(wheelId, "TyreB", "Wide", FidelityLevel::Envelope);

        ASSERT_TRUE(graph.defineInterface(
          frame, {"ip", "mount", "bore", {{"diameter", 4.8}, {"envelope", {-20.0, -20.0, -20.0, 20.0, 20.0, 20.0}}}}));
        ASSERT_TRUE(graph.defineInterface(tyreA, {"ia", "mount", "bore", {{"diameter", 4.8}}}));
        ASSERT_TRUE(graph.defineInterface(
          tyreB, {"ib", "mount", "bore", {{"diameter", 4.8}, {"envelope", {-30.0, -30.0, -30.0, 30.0, 30.0, 30.0}}}}));

        auto const cfgId = graph.createConfiguration("Concept A", {{parentId, frame}, {wheelId, tyreA}});
        ASSERT_TRUE(graph.activateConfiguration(cfgId));

        // Act
        std::string err;
        bool const ok = graph.substituteVariant(cfgId, wheelId, tyreB, err);

        // Assert: the shared bore matches, so only the containment check can refuse it
        EXPECT_FALSE(ok) << "an envelope stick-out must not be accepted";
        EXPECT_NE(err.find("Interface mismatch"), std::string::npos) << err;
        EXPECT_EQ(graph.activeConfiguration()->selectedVariants.at(wheelId), tyreA);
    }

    TEST(CadGates_ScopedInterfaces, CompatibleContract_PassesWithEvidence)
    {
        ContractHarness harness{{{"diameter", 4.8}, {"envelope", {-30.0, -30.0, -30.0, 30.0, 30.0, 30.0}}},
                                {{"diameter", 4.8001}, {"envelope", {-25.0, -25.0, -25.0, 25.0, 25.0, 25.0}}}};

        auto const result = checkScopedInterfaces(harness.graph, harness.active(), harness.childId);

        ASSERT_TRUE(result.passed) << result.message;
        EXPECT_EQ(result.evidence.at("variantId").get<std::string>(), harness.childVariantId);
    }

    TEST(CadGates_ScopedInterfaces, MismatchingBore_FailsNamingTheKey)
    {
        ContractHarness harness{{{"diameter", 4.8}}, {{"diameter", 6.0}}};

        auto const result = checkScopedInterfaces(harness.graph, harness.active(), harness.childId);

        EXPECT_FALSE(result.passed);
        EXPECT_NE(result.evidence.dump().find("'mount' (bore)"), std::string::npos) << result.evidence.dump();
    }

    TEST(CadGates_ScopedInterfaces, ParentEnvelopeViolation_Fails)
    {
        ContractHarness harness{{{"envelope", {-20.0, -20.0, -20.0, 20.0, 20.0, 20.0}}, {"diameter", 4.8}},
                                {{"envelope", {-40.0, -5.0, -5.0, 40.0, 5.0, 5.0}}, {"diameter", 4.8}}};

        auto const result = checkScopedInterfaces(harness.graph, harness.active(), harness.childId);

        EXPECT_FALSE(result.passed);
        EXPECT_TRUE(result.evidence.contains("containment")) << result.evidence.dump();
    }

    TEST(CadGates_ScopedInterfaces, EmptyScope_FailsInsteadOfPassingVacuously)
    {
        ContractHarness harness{{{"diameter", 4.8}}, {{"diameter", 4.8}}};

        EXPECT_FALSE(checkScopedInterfaces(harness.graph, harness.active(), "").passed);
    }

    TEST(CadGates_ScopedInterfaces, VariantWithoutInterfaces_Fails)
    {
        DesignGraph graph;
        auto const nodeId = graph.addNode(DesignNodeType::Component, "Wheel");
        auto const variantId = graph.createVariant(nodeId, "Tyre", "Ring", FidelityLevel::Envelope);
        auto const cfgId = graph.createConfiguration("A", {{nodeId, variantId}});
        ASSERT_TRUE(graph.activateConfiguration(cfgId));

        auto const result = checkScopedInterfaces(graph, graph.activeConfiguration().value(), nodeId);

        EXPECT_FALSE(result.passed);
        EXPECT_NE(result.message.find("declares no interfaces"), std::string::npos);
    }

    // --- Gate dispatch -------------------------------------------------------

    TEST(CadGates_EvaluateGate, ManualAndUserKinds_AreRefused)
    {
        RenderabilityFacts facts;
        GateContext context{facts};

        EXPECT_FALSE(evaluateGate(ChecklistKind::Manual, context).passed);
        EXPECT_FALSE(evaluateGate(ChecklistKind::User, context).passed);
    }

    TEST(CadGates_EvaluateGate, KindsWithoutContext_FailRatherThanPass)
    {
        RenderabilityFacts facts;
        GateContext context{facts};

        EXPECT_FALSE(evaluateGate(ChecklistKind::FlatNodeBudget, context).passed);
        EXPECT_FALSE(evaluateGate(ChecklistKind::Manufacturing, context).passed);
        EXPECT_FALSE(evaluateGate(ChecklistKind::Interface, context).passed);
    }

    TEST(CadGates_EvaluateGate, RenderabilityOfBlockedScene_FailsWithBlockers)
    {
        RenderabilityFacts facts;
        facts.buildItemCount = 1;
        facts.blockers.push_back({"noShapeOutput", "The root function writes no shape", "Add an add_function with a shape"});

        auto const result = evaluateGate(ChecklistKind::Renderability, GateContext{facts});

        EXPECT_FALSE(result.passed);
        EXPECT_NE(result.message.find("noShapeOutput"), std::string::npos) << result.message;
    }

    TEST(CadGates_EvaluateGate, RenderabilityOfBoundScene_Passes)
    {
        RenderabilityFacts facts;
        facts.buildItemCount = 1;
        facts.levelSetCount = 1;
        facts.rootFunctionId = 3;

        ASSERT_TRUE(evaluateGate(ChecklistKind::Renderability, GateContext{facts}).passed);
    }

    // --- Checklist gate evaluator --------------------------------------------

    TEST(TodoStore_GateEvaluator, PassingVerdict_ChecksTheEntryWithEvidence)
    {
        TodoStore store;
        auto const id = store.addItem("Design the wheel", "",
                                      {ChecklistEntry{"Renders", false, ChecklistKind::Renderability}});

        RenderabilityFacts facts;
        facts.buildItemCount = 1;
        facts.levelSetCount = 1;
        facts.rootFunctionId = 3;
        facts.boundsNumeric = true;

        auto const gaps = store.uncheckedDeterministicEntries(
          buildChecklistGateEvaluator(DesignGraph{}, Configuration{}, nullptr, facts));

        EXPECT_TRUE(gaps.empty());
        auto const & entry = store.items()[0].checklist[0];
        EXPECT_TRUE(entry.checked);
        EXPECT_EQ(entry.evidence.at("buildItemCount").get<int>(), 1);
        (void)id;
    }

    TEST(TodoStore_GateEvaluator, FailingVerdict_KeepsTheEntryUnproved)
    {
        TodoStore store;
        store.addItem("Design the wheel", "",
                      {ChecklistEntry{"Fits the domain", false, ChecklistKind::DomainBounds}});

        RenderabilityFacts facts;
        auto const gaps = store.uncheckedDeterministicEntries(
          buildChecklistGateEvaluator(DesignGraph{}, Configuration{}, nullptr, facts));

        ASSERT_EQ(gaps.size(), 1u);
        EXPECT_EQ(gaps[0].kind, ChecklistKind::DomainBounds);
        EXPECT_FALSE(store.items()[0].checklist[0].checked);
    }

    TEST(TodoStore_GateEvaluator, InterfaceEntry_IsCheckedAgainstItsOwnScope)
    {
        ContractHarness harness{{{"diameter", 4.8}}, {{"diameter", 6.0}}};
        TodoStore store;
        store.addItem("Wheel contract", "",
                      {ChecklistEntry{"Mates with the chassis", false, ChecklistKind::Interface, harness.childId}});

        RenderabilityFacts facts;
        auto const gaps = store.uncheckedDeterministicEntries(
          buildChecklistGateEvaluator(harness.graph, harness.active(), nullptr, facts));

        ASSERT_EQ(gaps.size(), 1u);
        EXPECT_EQ(gaps[0].scopeNodeId, harness.childId);
    }

    TEST(TodoStore_GateEvaluator, ManualAndUserEntries_AreNeverTouched)
    {
        TodoStore store;
        store.addItem("Ask the user", "",
                      {ChecklistEntry{"Confirm the finish", false, ChecklistKind::Manual},
                       ChecklistEntry{"Pick a variant", false, ChecklistKind::User}});

        RenderabilityFacts facts;
        auto const gaps = store.uncheckedDeterministicEntries(
          buildChecklistGateEvaluator(DesignGraph{}, Configuration{}, nullptr, facts));

        EXPECT_TRUE(gaps.empty());
        EXPECT_FALSE(store.items()[0].checklist[0].checked);
        EXPECT_FALSE(store.items()[0].checklist[1].checked);
    }

    // --- Checklist model -----------------------------------------------------

    TEST(TodoStore, CheckByGate_StoresEvidenceAndRefusesManualEntries)
    {
        TodoStore store;
        auto const id = store.addItem("Wheel", "",
                                      {ChecklistEntry{"Volume", false, ChecklistKind::Geometry},
                                       ChecklistEntry{"Looks right", false}});

        ASSERT_TRUE(store.checkByGate(id, 0, {{"smallestSpanMm", 6.0}}));
        EXPECT_TRUE(store.items()[0].checklist[0].checked);
        EXPECT_DOUBLE_EQ(store.items()[0].checklist[0].evidence.at("smallestSpanMm").get<double>(), 6.0);

        std::string reason;
        EXPECT_FALSE(store.checkByGate(id, 1, {{"anything", true}}, &reason));
        EXPECT_NE(reason.find("manual"), std::string::npos) << reason;
    }

    TEST(TodoStore, ToggleChecklistEntry_DeterministicKind_IsRefused)
    {
        TodoStore store;
        auto const id = store.addItem("Wheel", "",
                                      {ChecklistEntry{"Volume", false, ChecklistKind::Geometry},
                                       ChecklistEntry{"Looks right", false}});

        EXPECT_FALSE(store.toggleChecklistEntry(id, 0, true));
        EXPECT_TRUE(store.toggleChecklistEntry(id, 1, true));
    }

    TEST(TodoStore, Json_RoundTripsKindScopeEvidenceAndAssumed)
    {
        TodoStore store;
        ChecklistEntry entry{"Mates with the chassis", false, ChecklistKind::Interface, "node_2"};
        entry.evidence = {{"compatible", true}};
        entry.assumed = true;
        store.addItem("Wheel", "Ring", {entry, ChecklistEntry{"Plain label"}});

        nlohmann::json const json = store.toJson();
        TodoStore restored;
        ASSERT_TRUE(restored.fromJson(json));

        ASSERT_EQ(restored.items().size(), 1u);
        auto const & checklist = restored.items()[0].checklist;
        ASSERT_EQ(checklist.size(), 2u);
        EXPECT_EQ(checklist[0].kind, ChecklistKind::Interface);
        EXPECT_EQ(checklist[0].scopeNodeId, "node_2");
        EXPECT_TRUE(checklist[0].assumed);
        EXPECT_TRUE(checklist[0].evidence.at("compatible").get<bool>());
        EXPECT_EQ(checklist[1].kind, ChecklistKind::Manual);
        EXPECT_EQ(checklist[1].label, "Plain label");
    }

    TEST(TodoStore, UncheckedDeterministicEntries_ScopedLookupIncludesGlobalEntries)
    {
        TodoStore store;
        store.addItem("Wheel", "",
                      {ChecklistEntry{"Scoped", false, ChecklistKind::Geometry, "node_2"},
                       ChecklistEntry{"Global", false, ChecklistKind::Renderability}});

        auto const scopedOnly = store.uncheckedDeterministicEntries("node_2", false);
        ASSERT_EQ(scopedOnly.size(), 1u);
        EXPECT_EQ(scopedOnly[0].label, "Scoped");

        EXPECT_EQ(store.uncheckedDeterministicEntries("node_2", true).size(), 2u);
        EXPECT_EQ(store.uncheckedDeterministicEntries("other", true).size(), 1u);
    }

    // --- finish_design gating ------------------------------------------------

    namespace
    {
        /// @brief Harness with a live document, a TodoStore, and the full design tool surface
        struct GatingHarness
        {
            events::SharedLogger logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
            std::unique_ptr<Document> document = std::make_unique<Document>(logger);
            DesignGraph graph;
            DesignCatalogue catalogue;
            tools::TransactionManager transactions;
            tools::CheckpointManager checkpoints;
            TodoStore todos;
            /// @brief Must outlive the registered tool lambdas, therefore a member
            DesignToolContext ctx;
            tools::ToolRegistry registry;
            tools::ToolExecutor executor;
            io::CancellationToken cancellation;

            GatingHarness()
                : ctx{graph,
                      catalogue,
                      transactions,
                      checkpoints,
                      document.get(),
                      [](int, int, std::string const &)
                      {
                          return std::vector<std::byte>{};
                      },
                      nullptr,
                      nullptr,
                      nullptr,
                      &todos,
                      nullptr,
                      false,
                      0}
                , executor(registry)
            {
                document->newModel();
                registerDesignTools(registry, ctx, DesignToolSurface::Full);
            }

            /// @brief Seeds the volumetric template so the scene really renders
            void seedRenderableScene()
            {
                auto const seed = design::seedVolumetricTemplate(document.get());
                EXPECT_TRUE(seed.success) << seed.error;
            }

            /// @brief One design node with a variant and an active configuration
            std::string makeActiveConfiguration()
            {
                auto const nodeId = graph.addNode(DesignNodeType::Component, "Wheel");
                auto const variantId = graph.createVariant(nodeId, "Tyre", "Ring", FidelityLevel::Envelope);
                auto const cfgId = graph.createConfiguration("Concept A", {{nodeId, variantId}});
                EXPECT_TRUE(graph.activateConfiguration(cfgId));
                return nodeId;
            }
        };
    } // namespace

    TEST(FinishDesign_UnprovedDeterministicChecklistEntry, ToolCall_BlocksCompletionAndNamesTheEntry)
    {
        // Arrange: a renderable scene and valid bookkeeping, but an interface requirement nothing has proved
        GatingHarness harness;
        harness.seedRenderableScene();
        auto const nodeId = harness.makeActiveConfiguration();
        auto const created = harness.executor.executeSync(
          "add_todo_item",
          {{"title", "Wheel contract"},
           {"checklist", {{{"label", "Mates with chassis"}, {"kind", "interface"}}}}},
          harness.cancellation);
        ASSERT_TRUE(created.value("success", false)) << created.dump();

        // Act
        auto const result = harness.executor.executeSync("finish_design", {}, harness.cancellation);

        // Assert: the design stays incomplete and the gap is fully described
        EXPECT_FALSE(result.value("success", true)) << result.dump();
        EXPECT_FALSE(result.value("valid", true));
        EXPECT_NE(toString(harness.graph.phase()), "Complete");
        ASSERT_TRUE(result.contains("uncheckedChecklistEntries"));
        ASSERT_EQ(result["uncheckedChecklistEntries"].size(), 1u);
        auto const & gap = result["uncheckedChecklistEntries"][0];
        EXPECT_EQ(gap.at("todoId").get<std::string>(), created.at("todoId").get<std::string>());
        EXPECT_EQ(gap.at("index").get<int>(), 0);
        EXPECT_EQ(gap.at("kind").get<std::string>(), "interface");
        EXPECT_NE(result.value("error", "").find("Mates with chassis"), std::string::npos) << result.dump();
        (void)nodeId;
    }

    TEST(FinishDesign_ProvedDeterministicChecklistEntry, ToolCall_CompletesAndChecksTheEntry)
    {
        // Arrange: the same scene, but every requirement is one a gate can prove
        GatingHarness harness;
        harness.seedRenderableScene();
        (void)harness.makeActiveConfiguration();
        auto const created = harness.executor.executeSync(
          "add_todo_item",
          {{"title", "Wheel"}, {"checklist", {{{"label", "Renders"}, {"kind", "renderability"}}}}},
          harness.cancellation);
        ASSERT_TRUE(created.value("success", false)) << created.dump();

        // Act
        auto const result = harness.executor.executeSync("finish_design", {}, harness.cancellation);

        // Assert: the gate checked the entry itself, with its measurement recorded
        EXPECT_TRUE(result.value("success", false)) << result.dump();
        EXPECT_EQ(toString(harness.graph.phase()), "Complete");
        auto const & entry = harness.todos.items()[0].checklist[0];
        EXPECT_TRUE(entry.checked);
        EXPECT_GE(entry.evidence.at("buildItemCount").get<int>(), 1);
    }

    TEST(FinishDesign_ManualChecklistEntry, ToolCall_DoesNotRequireGateEvidence)
    {
        // Arrange: a judgement entry cannot be gate proved, so it must not block completion
        GatingHarness harness;
        harness.seedRenderableScene();
        (void)harness.makeActiveConfiguration();
        ASSERT_TRUE(harness.executor.executeSync(
          "add_todo_item",
          {{"title", "Wheel"}, {"checklist", {{{"label", "Looks like a tyre"}}}}},
          harness.cancellation)
          .value("success", false));

        auto const result = harness.executor.executeSync("finish_design", {}, harness.cancellation);

        EXPECT_TRUE(result.value("success", false)) << result.dump();
    }

    TEST(CadGates_FlatNodeBudget, SeededScene_ReportsItsCountAndHonoursTheBudget)
    {
        GatingHarness harness;
        harness.seedRenderableScene();

        auto const measured = checkFlatNodeBudget(*harness.document, 0);
        ASSERT_FALSE(measured.passed) << measured.message;
        auto const count = measured.evidence.at("expectedNodeCount").get<std::size_t>();
        EXPECT_GE(count, 1u);

        auto const generous = checkFlatNodeBudget(*harness.document, count + 100);
        EXPECT_TRUE(generous.passed) << generous.message;
        EXPECT_EQ(generous.evidence.at("expectedNodeCount").get<std::size_t>(), count);
    }
} // namespace gladius::design::tests
