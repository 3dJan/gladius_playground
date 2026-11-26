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

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace gladius::ui::tests
{
    namespace
    {
        /// @brief Geometry and layout facts measured from one rendered workflow frame
        struct FrameMetrics
        {
            int vertices = 0;
            float scrollMaxY = 0.f;
            std::array<AgentWorkflowView::RowLayout, kWorkflowPhaseCount> layout{};
            /// @brief Rail legs drawn by the view, as (fromY, toY) of each centre-to-centre stroke
            std::array<std::pair<float, float>, kWorkflowPhaseCount - 1> spine{};
            /// @brief X of the cursor the view gave each phase's artifact renderer
            std::array<float, kWorkflowPhaseCount> contentX{};
            /// @brief X of the first line the Understand section started after its own content
            float understandFirstLineX = 0.f;
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

                // Scroll extents are finalised at the end of a frame, so this reads the settled
                // value of the previous frame: callers render each configuration twice.
                FrameMetrics metrics;

                // The view owns where content starts; record it where the callback is invoked.
                std::array<AgentWorkflowView::ArtifactRenderer, kWorkflowPhaseCount> renderers;
                for (std::size_t i = 0; i < kWorkflowPhaseCount; ++i)
                {
                    renderers[i] = [this, i, &metrics]() {
                        metrics.contentX[i] = ImGui::GetCursorScreenPos().x;
                        if (i == 0)
                        {
                            // Proxy items: the first consumes the cursor the view just set, the
                            // second starts a fresh line. Sections that draw their own text
                            // return the cursor to the line start, so that x is what a section
                            // label ends up using.
                            ImGui::Dummy(ImVec2(1.0f, 1.0f));
                            ImGui::Dummy(ImVec2(1.0f, 1.0f));
                            metrics.understandFirstLineX = ImGui::GetCursorScreenPos().x;
                        }
                        switch (i)
                        {
                        case 0: m_artifacts.renderUnderstand(m_animation); break;
                        case 1: m_artifacts.renderDecompose(m_animation); break;
                        case 2: m_artifacts.renderExplore(m_animation); break;
                        case 3: m_artifacts.renderDevelop(m_animation); break;
                        default: m_artifacts.renderValidate(m_animation); break;
                        }
                    };
                }

                ImGui::BeginChild("WorkflowScroll", ImVec2(0.f, -120.f), true);
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
                metrics.layout = m_view.lastLayout();
                metrics.spine = m_view.lastSpine();
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
    namespace
    {
        /// @brief True when a rail leg reaches `y`, i.e. the stroke runs past the circle edge
        bool spineReaches(std::array<std::pair<float, float>, kWorkflowPhaseCount - 1> const & spine,
                          float y)
        {
            return std::any_of(spine.begin(),
                               spine.end(),
                               [y](std::pair<float, float> const & leg) {
                                   return leg.first <= y && leg.second >= y;
                               });
        }
    } // namespace

    TEST_F(ImGuiFrameFixture, Render_RailLineRunsThroughEveryPhaseCircle)
    {
        design::DesignGraph graph;
        design::TodoStore todos;
        tools::CheckpointManager checkpoints;
        graph.setPhase(design::DesignPhase::DevelopComponents);
        populateDesign(graph, todos);
        WorkflowPanelHarness harness(graph, todos, checkpoints);

        std::array<bool, kWorkflowPhaseCount> expanded{};
        expanded.fill(false);
        auto const metrics = harness.render(graph, WorkflowSignals{}, expanded, 360.f, 700.f, 0.f);

        // One spine: every circle sits on the same vertical axis...
        for (std::size_t i = 0; i < kWorkflowPhaseCount; ++i)
        {
            EXPECT_NEAR(metrics.layout[i].railX, metrics.layout[0].railX, 0.01f) << "phase " << i;
            EXPECT_NEAR(metrics.layout[i].center.x, metrics.layout[0].railX, 0.01f) << "phase " << i;
            if (i > 0)
            {
                EXPECT_LT(metrics.layout[i - 1].center.y, metrics.layout[i].center.y)
                  << "phase " << i;
            }
        }

        // ...and the stroke reaches each centre instead of stopping at the circle edge, which is
        // what made the line look broken in the previous design.
        for (std::size_t i = 0; i + 1 < kWorkflowPhaseCount; ++i)
        {
            // The leg is drawn from centre to centre, so it covers both circles completely rather
            // than stopping at their edges.
            EXPECT_TRUE(spineReaches(metrics.spine, metrics.layout[i].center.y))
              << "leg into phase " << i;
            EXPECT_TRUE(spineReaches(metrics.spine, metrics.layout[i + 1].center.y))
              << "leg into phase " << i + 1;
            EXPECT_NEAR(metrics.spine[i].first, metrics.layout[i].center.y, 0.01f) << "phase " << i;
            EXPECT_NEAR(metrics.spine[i].second, metrics.layout[i + 1].center.y, 0.01f)
              << "phase " << i;
        }
        for (auto const & leg : metrics.spine)
        {
            EXPECT_GT(leg.second, leg.first);
        }
    }

    TEST_F(ImGuiFrameFixture, Render_ExpandedPhaseContentStartsRightOfTheRail)
    {
        design::DesignGraph graph;
        design::TodoStore todos;
        tools::CheckpointManager checkpoints;
        graph.setPhase(design::DesignPhase::DecomposeFunctions);
        populateDesign(graph, todos);
        WorkflowPanelHarness harness(graph, todos, checkpoints);

        std::array<bool, kWorkflowPhaseCount> expanded{};
        expanded.fill(true);
        float const panelWidth = 360.f;
        (void) harness.render(graph, WorkflowSignals{}, expanded, panelWidth, 700.f, 0.f);
        auto const metrics = harness.render(graph, WorkflowSignals{}, expanded, panelWidth, 700.f,
                                            0.02f);

        float const rightEdge = panelWidth - ImGui::GetStyle().WindowPadding.x;
        for (std::size_t i = 0; i < kWorkflowPhaseCount; ++i)
        {
            auto const & row = metrics.layout[i];
            EXPECT_GT(row.contentX, row.railX + row.radius) << "phase " << i;
            EXPECT_LT(row.railX, rightEdge) << "phase " << i;
            EXPECT_LT(metrics.contentX[i], rightEdge) << "phase " << i;
            EXPECT_NEAR(metrics.contentX[i], row.contentX, 0.01f) << "phase " << i;
        }
    }

    TEST_F(ImGuiFrameFixture, Render_SectionsAfterContentRowRestartAtTheContentColumn)
    {
        design::DesignGraph graph;
        design::TodoStore todos;
        tools::CheckpointManager checkpoints;
        graph.addRequirement("Diameter of the filter disc is 100 mm", "dimension", true);
        graph.setPhase(design::DesignPhase::DevelopComponents);
        populateDesign(graph, todos);
        WorkflowPanelHarness harness(graph, todos, checkpoints);

        std::array<bool, kWorkflowPhaseCount> expanded{};
        expanded[0] = true;
        (void) harness.render(graph, WorkflowSignals{}, expanded, 360.f, 700.f, 0.f);
        auto const metrics = harness.render(graph, WorkflowSignals{}, expanded, 360.f, 700.f, 0.02f);

        // Sections move the cursor by drawing their own rows, so the indent that puts content
        // right of the rail has to survive them; otherwise every section after the first snaps
        // back to the window's left edge.
        auto const & row = metrics.layout[0];
        EXPECT_GT(metrics.understandFirstLineX, row.railX + row.radius);
        EXPECT_NEAR(metrics.understandFirstLineX, row.contentX, 0.01f);
    }

    TEST_F(ImGuiFrameFixture, Render_SpineStaysContinuousAtEveryWidthAndScale)
    {
        design::DesignGraph graph;
        design::TodoStore todos;
        tools::CheckpointManager checkpoints;
        graph.setPhase(design::DesignPhase::DevelopComponents);
        populateDesign(graph, todos);
        WorkflowPanelHarness harness(graph, todos, checkpoints);

        ImGuiIO & io = ImGui::GetIO();
        float const baseFontScale = io.FontGlobalScale;
        float const uiScales[] = {0.75f, 1.f, 2.f};
        float const widths[] = {280.f, 420.f, 900.f};
        for (float const scale : uiScales)
        {
            io.FontGlobalScale = baseFontScale * scale;
            for (float const width : widths)
            {
                // Expanded phases add tall artifact sections: the spine must still connect every
                // circle, not just the ones of a collapsed rail.
                std::array<bool, kWorkflowPhaseCount> expanded{};
                expanded.fill(true);
                auto const metrics = harness.render(graph, WorkflowSignals{}, expanded, width,
                                                    380.f, scale);
                for (std::size_t i = 0; i + 1 < kWorkflowPhaseCount; ++i)
                {
                    EXPECT_TRUE(spineReaches(metrics.spine, metrics.layout[i].center.y))
                      << "scale " << scale << " width " << width << " phase " << i;
                }
            }
        }
        io.FontGlobalScale = baseFontScale;
    }
} // namespace gladius::ui::tests
