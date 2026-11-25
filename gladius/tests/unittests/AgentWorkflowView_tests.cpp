/**
 * @file AgentWorkflowView_tests.cpp
 * @brief Headless ImGui smoke tests for the Design Agent workflow rail and its artifacts.
 * @details The rail and artifact sections are custom ImDrawList code that the pure-logic tests
 *          cannot reach. A backend-less ImGui frame proves the drawing actually produces geometry,
 *          that expanding phases adds content, and that the panel keeps scrolling when the content
 *          is taller or narrower than the window.
 */

#include <gtest/gtest.h>
#include <imgui.h>

#include "ui/AgentWorkflow.h"
#include "ui/AgentWorkflowAnimation.h"
#include "ui/AgentWorkflowView.h"
#include "ui/WorkflowArtifacts.h"

#include "design/DesignGraph.h"
#include "design/TodoStore.h"
#include "tools/CheckpointManager.h"

#include <array>
#include <string>

namespace gladius::ui::tests
{
    namespace
    {
        /// @brief Geometry and layout facts measured from one rendered workflow frame
        struct FrameMetrics
        {
            int vertices = 0;
            float scrollMaxY = 0.f;
        };

        /// @brief Drives the rail and all five artifact sections the way AgentPanels does
        class WorkflowPanelHarness
        {
          public:
            WorkflowPanelHarness(design::DesignGraph & graph,
                                 design::TodoStore & todos,
                                 tools::CheckpointManager & checkpoints)
            {
                m_artifacts.bind(graph, todos, checkpoints, nullptr, nullptr, nullptr);
            }

            [[nodiscard]] FrameMetrics render(design::DesignGraph const & graph,
                                              WorkflowSignals const & signals,
                                              std::array<bool, kWorkflowPhaseCount> & expanded,
                                              float panelWidth,
                                              float panelHeight,
                                              float timeSeconds)
            {
                ImGuiIO & io = ImGui::GetIO();
                io.DisplaySize = ImVec2(panelWidth, panelHeight);
                io.DeltaTime = 1.f / 60.f;
                ImGui::NewFrame();

                ImGui::SetNextWindowSize(ImVec2(panelWidth, panelHeight));
                ImGui::Begin("Design Agent");

                auto const states = derivePhaseStates(graph, signals);
                std::size_t const activeIndex = indexOf(activePhaseOf(graph));
                std::array<std::string, kWorkflowPhaseCount> summaries;
                for (std::size_t i = 0; i < kWorkflowPhaseCount; ++i)
                {
                    summaries[i] = "2 constraints, 1 preference";
                }
                m_animation.update(activeIndex, states, timeSeconds);

                std::array<AgentWorkflowView::ArtifactRenderer, kWorkflowPhaseCount> const renderers
                  = {
                    [&]() { m_artifacts.renderUnderstand(m_animation); },
                    [&]() { m_artifacts.renderDecompose(m_animation); },
                    [&]() { m_artifacts.renderExplore(m_animation); },
                    [&]() { m_artifacts.renderDevelop(m_animation); },
                    [&]() { m_artifacts.renderValidate(m_animation); }};

                ImGui::BeginChild("WorkflowScroll", ImVec2(0.f, -120.f), true);
                // Scroll extents are finalised at the end of a frame, so this reads the settled
                // value of the previous frame: callers render each configuration twice.
                FrameMetrics metrics;
                metrics.scrollMaxY = ImGui::GetScrollMaxY();
                m_view.render(states, activeIndex, summaries, expanded, renderers, m_animation);
                ImGui::EndChild();
                ImGui::End();
                ImGui::Render();

                ImDrawData const * drawData = ImGui::GetDrawData();
                if (drawData != nullptr)
                {
                    for (ImDrawList const * list : drawData->CmdLists)
                    {
                        metrics.vertices += static_cast<int>(list->VtxBuffer.size());
                    }
                }
                return metrics;
            }

          private:
            AgentWorkflowView m_view;
            AgentWorkflowAnimationState m_animation;
            WorkflowArtifacts m_artifacts;
        };

        class ImGuiFrameFixture : public ::testing::Test
        {
          protected:
            void SetUp() override
            {
                m_context = ImGui::CreateContext();
                ImGuiIO & io = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.DisplayFramebufferScale = ImVec2(1.f, 1.f);
                // A render backend normally builds the atlas; without one we build it in memory.
                unsigned char * pixels = nullptr;
                int width = 0;
                int height = 0;
                io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
                ASSERT_TRUE(io.Fonts->IsBuilt());
            }

            void TearDown() override
            {
                ImGui::DestroyContext(m_context);
            }

            ImGuiContext * m_context = nullptr;
        };

        void populateDesign(design::DesignGraph & graph, design::TodoStore & todos)
        {
            for (int i = 0; i < 12; ++i)
            {
                graph.addRequirement("Requirement number " + std::to_string(i), "functional", true);
            }
            graph.addRequirement("Diameter is 100 mm", "dimension", false);

            std::string const root = graph.addNode(design::DesignNodeType::System, "Filter disc");
            std::string const ribField
              = graph.addNode(design::DesignNodeType::Component, "Rib field", root);
            graph.addNode(design::DesignNodeType::Component, "Rim", root);
            graph.addNode(design::DesignNodeType::Feature, "Envelope", ribField);
            for (int i = 0; i < 5; ++i)
            {
                graph.createVariant(ribField,
                                    "Alternative " + std::to_string(i),
                                    "Alternating wave geometry with a tuned wavelength");
            }
            std::string const configId = graph.createConfiguration("Concept A", {{ribField, "var_1"}});
            (void) graph.activateConfiguration(configId);

            todos.addItem("Set initial wavelength");
            std::string const blocked = todos.addItem("Optimize minimum wall");
            todos.updateItem(blocked, design::TodoStatus::Blocked);
        }
    } // namespace

    TEST_F(ImGuiFrameFixture, Render_EmptyDesign_DrawsPhaseRail)
    {
        design::DesignGraph graph;
        design::TodoStore todos;
        tools::CheckpointManager checkpoints;
        WorkflowPanelHarness harness(graph, todos, checkpoints);

        std::array<bool, kWorkflowPhaseCount> expanded{};
        expanded.fill(true);
        auto const metrics = harness.render(graph, WorkflowSignals{}, expanded, 360.f, 700.f, 0.f);

        EXPECT_GT(metrics.vertices, 0);
    }

    TEST_F(ImGuiFrameFixture, Render_PopulatedDesign_ExpandedPhasesDrawArtifacts)
    {
        design::DesignGraph graph;
        design::TodoStore todos;
        tools::CheckpointManager checkpoints;
        graph.setPhase(design::DesignPhase::DevelopComponents);
        populateDesign(graph, todos);
        WorkflowPanelHarness harness(graph, todos, checkpoints);

        std::array<bool, kWorkflowPhaseCount> collapsed{};
        collapsed.fill(false);
        std::array<bool, kWorkflowPhaseCount> expanded{};
        expanded.fill(true);

        auto const collapsedMetrics
          = harness.render(graph, WorkflowSignals{}, collapsed, 360.f, 700.f, 0.f);
        (void) harness.render(graph, WorkflowSignals{}, expanded, 360.f, 700.f, 0.02f);
        auto const expandedMetrics
          = harness.render(graph, WorkflowSignals{}, expanded, 360.f, 700.f, 0.04f);

        EXPECT_GT(expandedMetrics.vertices, collapsedMetrics.vertices);
        // Requirement, structure and variant content does not fit a short panel: it must scroll.
        EXPECT_GT(expandedMetrics.scrollMaxY, 0.f);
    }

    TEST_F(ImGuiFrameFixture, Render_VaryingPanelWidthsAndUiScale_KeepDrawing)
    {
        design::DesignGraph graph;
        design::TodoStore todos;
        tools::CheckpointManager checkpoints;
        graph.setPhase(design::DesignPhase::Validate);
        populateDesign(graph, todos);
        WorkflowPanelHarness harness(graph, todos, checkpoints);

        WorkflowSignals signals;
        signals.running = true;
        signals.planningBlocked = hasBlockedPlanning(todos);
        std::size_t passed = 0;
        countDeterministicResults(graph, passed, signals.deterministicWarnings);
        EXPECT_EQ(signals.deterministicWarnings, std::size_t{0});

        float const uiScales[] = {0.75f, 1.f, 1.5f, 2.f};
        float const widths[] = {280.f, 360.f, 520.f, 900.f};
        ImGuiIO & io = ImGui::GetIO();
        float const baseFontScale = io.FontGlobalScale;
        for (float const scale : uiScales)
        {
            // Scaling is how a DPI change reaches the rail: every size derives from GetFontSize.
            io.FontGlobalScale = baseFontScale * scale;
            for (float const width : widths)
            {
                std::array<bool, kWorkflowPhaseCount> expanded{};
                expanded.fill(true);
                (void) harness.render(graph, signals, expanded, width, 380.f, scale);
                auto const metrics = harness.render(graph, signals, expanded, width, 380.f, scale);
                EXPECT_GT(metrics.vertices, 0) << "scale " << scale << " width " << width;
            }
        }
        io.FontGlobalScale = baseFontScale;
    }

    TEST_F(ImGuiFrameFixture, Render_PhaseAdvanceKeepsRailUsableWhileTransitionRuns)
    {
        design::DesignGraph graph;
        design::TodoStore todos;
        tools::CheckpointManager checkpoints;
        graph.setPhase(design::DesignPhase::DecomposeFunctions);
        populateDesign(graph, todos);
        WorkflowPanelHarness harness(graph, todos, checkpoints);

        std::array<bool, kWorkflowPhaseCount> expanded{};
        expanded.fill(true);
        (void) harness.render(graph, WorkflowSignals{}, expanded, 420.f, 640.f, 0.f);

        graph.setPhase(design::DesignPhase::DevelopComponents);
        (void) harness.render(graph, WorkflowSignals{}, expanded, 420.f, 640.f, 0.1f);
        auto const metrics = harness.render(graph, WorkflowSignals{}, expanded, 420.f, 640.f, 0.2f);

        // Layout never waits for the animation: the rail is already at its final place while the
        // line transition is still travelling.
        EXPECT_GT(metrics.vertices, 0);
        EXPECT_GT(metrics.scrollMaxY, 0.f);
    }
} // namespace gladius::ui::tests
