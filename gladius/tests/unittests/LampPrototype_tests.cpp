#include "agent/AgentLoop.h"
#include "design/DesignTools.h"

#include <gtest/gtest.h>
#include <queue>

namespace gladius::design::tests
{
    namespace
    {
        class ScriptedLampProvider final : public agent::IAiProvider
        {
          public:
            std::queue<agent::CompletionResponse> turns;

            std::string name() const override
            {
                return "scripted_lamp";
            }

            coro::task<agent::CompletionResponse>
            complete(agent::CompletionRequest const & /*request*/,
                     agent::TextDeltaCallback onDelta,
                     io::CancellationToken const & /*cancellation*/) override
            {
                if (turns.empty())
                {
                    co_return agent::CompletionResponse{"Complete", {}, "stop", true, ""};
                }
                auto resp = std::move(turns.front());
                turns.pop();
                if (onDelta && !resp.text.empty())
                {
                    onDelta(resp.text);
                }
                co_return resp;
            }
        };
    } // namespace

    TEST(LampPrototype, EndToEndDesignExploration_ThreeConceptsToValidatedHybrid_Completes)
    {
        // Arrange: design graph, catalogue, transaction & checkpoint managers, tool registry
        DesignGraph graph;
        DesignCatalogue catalogue;
        nlohmann::json const catalogueDoc = {
          {"entries",
           {{{"id", "lamp.stand.pedestal"}, {"functions", {"Stand"}}},
            {{"id", "lamp.stand.tripod"}, {"functions", {"Stand"}}},
            {{"id", "lamp.shade.cone"}, {"functions", {"Shade"}}},
            {{"id", "lamp.shade.ribbed"}, {"functions", {"Shade"}}},
            {{"id", "lamp.cable.rear_slot"}, {"functions", {"CableRoute"}}}}}
        };
        ASSERT_TRUE(catalogue.loadFromJson(catalogueDoc));
        catalogue.addCompatibilityRule(
          {{{"node_1", "var_2"}, {"node_2", "var_4"}}, false, "Incompatible stand/shade interface"});

        tools::TransactionManager tx;
        tools::CheckpointManager checkpoints;
        bool viewportCaptured = false;

        DesignToolContext ctx{
          graph,
          catalogue,
          tx,
          checkpoints,
          nullptr,
          [&](int /*w*/, int /*h*/, std::string const & /*cam*/) {
              viewportCaptured = true;
              return std::vector<std::byte>{std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47}};
          },
          nullptr};

        tools::ToolRegistry registry;
        registerDesignTools(registry, ctx);
        tools::ToolExecutor executor(registry);

        ScriptedLampProvider provider;
        // Turn 1: requirements + function decomposition
        provider.turns.push(agent::CompletionResponse{
          "Decomposing table lamp functions.",
          {{"c1", "add_requirement", {{"description", "Support-free FDM table lamp"}, {"category", "manufacturing"}}},
           {"c2", "add_function", {{"name", "Stand"}, {"type", "component"}}},
           {"c3", "add_function", {{"name", "Shade"}, {"type", "component"}}},
           {"c4", "add_function", {{"name", "CableRoute"}, {"type", "function"}}}},
          "tool_calls",
          true,
          ""});

        // Turn 2: envelope variants + configurations
        provider.turns.push(agent::CompletionResponse{
          "Creating envelope variants.",
          {{"c5", "create_variant", {{"nodeId", "node_1"}, {"name", "Pedestal"}, {"fidelity", "envelope"}}},
           {"c6", "create_variant", {{"nodeId", "node_1"}, {"name", "Tripod"}, {"fidelity", "envelope"}}},
           {"c7", "create_variant", {{"nodeId", "node_2"}, {"name", "Cone"}, {"fidelity", "envelope"}}},
           {"c8", "create_variant", {{"nodeId", "node_2"}, {"name", "Ribbed"}, {"fidelity", "envelope"}}},
           {"c9", "create_variant", {{"nodeId", "node_3"}, {"name", "RearSlot"}, {"fidelity", "envelope"}}}},
          "tool_calls",
          true,
          ""});

        // Turn 3: build 3 concepts, activate Concept A, freeze Stand, capture viewport, hybridize, finish
        provider.turns.push(agent::CompletionResponse{
          "Evaluating concepts and hybridizing.",
          {{"c10", "create_configuration", {{"name", "Concept A"}, {"selectedVariants", {{"node_1", "var_1"}, {"node_2", "var_3"}, {"node_3", "var_5"}}}}},
           {"c11", "create_configuration", {{"name", "Concept B"}, {"selectedVariants", {{"node_1", "var_2"}, {"node_2", "var_4"}, {"node_3", "var_5"}}}}},
           {"c12", "activate_configuration", {{"configurationId", "cfg_1"}}},
           {"c13", "freeze_component", {{"nodeId", "node_1"}, {"frozen", true}}},
           {"c14", "capture_viewport", {{"width", 512}, {"height", 512}, {"camera", "isometric"}}},
           {"c15", "hybridize_variants", {{"name", "Hybrid Lamp"}, {"baseConfigId", "cfg_1"}, {"overrideConfigId", "cfg_2"}, {"overrideNodeIds", {"node_2"}}}},
           {"c16", "activate_configuration", {{"configurationId", "cfg_3"}}},
           {"c17", "finish_design", nlohmann::json::object()}},
          "tool_calls",
          true,
          ""});

        agent::Conversation conv;
        conv.appendUserText("Design a support-free FDM parametric table lamp.");
        agent::ContextBuilder ctxBuilder;
        agent::PermissionPolicy policy(agent::AutonomyLevel::WorkAutonomously, agent::PrivacySettings{});
        agent::AgentEventQueue queue;
        agent::AgentLimits limits;
        io::CancellationToken cancel;
        agent::AgentLoop loop;

        // Act
        auto const res = coro::sync_wait(loop.run(
          conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel,
          [&]() { return graph.phase() == DesignPhase::Complete; }));

        // Assert
        EXPECT_TRUE(res.completed);
        EXPECT_TRUE(viewportCaptured);
        EXPECT_EQ(graph.phase(), DesignPhase::Complete);
        EXPECT_EQ(graph.nodes().size(), 3u);
        EXPECT_EQ(graph.variants().size(), 5u);
        EXPECT_EQ(graph.configurations().size(), 3u);
        auto const active = graph.activeConfiguration();
        ASSERT_TRUE(active.has_value());
        EXPECT_EQ(active->name, "Hybrid Lamp");
        EXPECT_EQ(active->selectedVariants.at("node_1"), "var_1"); // Frozen Pedestal preserved
        EXPECT_EQ(active->selectedVariants.at("node_2"), "var_4"); // Hybridized Ribbed shade
    }
} // namespace gladius::design::tests
