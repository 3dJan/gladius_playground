/**
 * @file AgentPanelDocking_tests.cpp
 * @brief Backend-less ImGui tests for the Design Agent panel default dock layout.
 * @details The default layout is built with the DockBuilder API on the first frame that has no stored
 *          window settings, so these frames prove the panel ends up docked to the right of the main
 *          dockspace, keeps a stable width across frames, and never gets a tab bar.
 */

#include "ui/AgentPanelDocking.h"
#include "ui/AgentPanelMetrics.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace gladius::ui::tests
{
    namespace
    {
        constexpr ImVec2 kDockspaceSize{1600.f, 900.f};

        /// @brief One frame of the main dockspace plus the Design Agent panel
        class DockedPanelFrame
        {
          public:
            DockedPanelFrame(bool applyDefaultLayout, bool preSplitRoot = false)
            {
                ImGuiIO & io = ImGui::GetIO();
                io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
                io.DisplaySize = kDockspaceSize;

                ImGui::NewFrame();

                // A zero padded, full size host window makes the dockspace area exactly kDockspaceSize.
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
                ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
                ImGui::SetNextWindowSize(kDockspaceSize);
                ImGui::Begin("MainWindowDockingArea",
                             nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking);
                const ImGuiID dockspaceID = ImGui::GetID("MainDockingSpace");
                ImGui::DockSpace(dockspaceID,
                                 ImVec2(0.f, 0.f),
                                 ImGuiDockNodeFlags_None | ImGuiDockNodeFlags_PassthruCentralNode);
                if (preSplitRoot)
                {
                    // The shipped imgui.ini already hands us a split dockspace tree.
                    ImGuiID leftID = 0;
                    ImGuiID restID = 0;
                    ImGui::DockBuilderSplitNode(dockspaceID, ImGuiDir_Left, 0.25f, &leftID, &restID);
                    ImGui::DockBuilderFinish(dockspaceID);
                }
                if (applyDefaultLayout)
                {
                    applyAgentPanelDefaultLayout(dockspaceID);
                }
                ImGui::End();
                ImGui::PopStyleVar();

                ImGui::Begin(kAgentPanelWindowName, nullptr, ImGuiWindowFlags_NoTitleBar);
                ImGui::TextUnformatted("panel");
                ImGui::End();

                ImGui::Render();
                m_dockspaceID = dockspaceID;
            }

            [[nodiscard]] ImGuiID dockspaceID() const noexcept
            {
                return m_dockspaceID;
            }

          private:
            ImGuiID m_dockspaceID = 0;
        };

        /// @brief Design Agent window of the frame rendered last
        [[nodiscard]] ImGuiWindow * panelWindow()
        {
            return ImGui::FindWindowByName(kAgentPanelWindowName);
        }

        /// @brief Width the docking helper asks the dockspace for, derived from the dockspace width
        [[nodiscard]] float expectedPanelWidth()
        {
            return std::clamp(kDockspaceSize.x * agentMetrics::panelDefaultWidthRatio,
                               agentMetrics::panelMinWidthPx,
                               agentMetrics::panelMaxWidthPx);
        }

        /// @brief Backend-less ImGui context with docking enabled and no ini file
        class AgentPanelDockingFixture : public ::testing::Test
        {
          protected:
            void SetUp() override
            {
                m_context = ImGui::CreateContext();
                ImGuiIO & io = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.DisplayFramebufferScale = ImVec2(1.f, 1.f);
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
    } // namespace

    TEST_F(AgentPanelDockingFixture, ApplyDefaultLayout_FirstFrame_PanelDockedToRightOfDockspace)
    {
        DockedPanelFrame frame(true);

        ImGuiWindow * panel = panelWindow();
        ASSERT_NE(panel, nullptr);
        EXPECT_NE(panel->DockId, 0u);
        ASSERT_NE(panel->DockNode, nullptr);
        EXPECT_EQ(ImGui::DockNodeGetRootNode(panel->DockNode)->ID, frame.dockspaceID());

        ImGuiDockNode * parent = panel->DockNode->ParentNode;
        ASSERT_NE(parent, nullptr);
        EXPECT_EQ(parent->SplitAxis, ImGuiAxis_X);
        // The panel sits in the child node toward the right edge of the dockspace.
        EXPECT_EQ(parent->ChildNodes[1], panel->DockNode);
        EXPECT_GT(panel->DockNode->Pos.x, parent->ChildNodes[0]->Pos.x);
    }

    TEST_F(AgentPanelDockingFixture, ApplyDefaultLayout_PanelWidthFollowsPixelClamp)
    {
        DockedPanelFrame first(true);
        DockedPanelFrame second(false);

        ImGuiWindow * panel = panelWindow();
        ASSERT_NE(panel, nullptr);
        EXPECT_NEAR(panel->Size.x, expectedPanelWidth(), 8.f);
    }

    TEST_F(AgentPanelDockingFixture, ApplyDefaultLayout_DockedNode_HasNoTabBar)
    {
        DockedPanelFrame first(true);
        DockedPanelFrame second(false);

        ImGuiWindow * panel = panelWindow();
        ASSERT_NE(panel, nullptr);
        ASSERT_NE(panel->DockNode, nullptr);
        // The panel draws its own header, so its dock node must not present a tab bar.
        EXPECT_TRUE(panel->DockNode->IsNoTabBar());
    }

    TEST_F(AgentPanelDockingFixture, DockedPanel_FramesWithoutDockBuilder_KeepDockIdAndWidth)
    {
        DockedPanelFrame first(true);
        // ImGui needs one more frame before the dock node hosts the window, so settle before measuring.
        DockedPanelFrame settled(false);
        ImGuiWindow * afterSettled = panelWindow();
        ASSERT_NE(afterSettled, nullptr);
        ImGuiID const dockID = afterSettled->DockId;
        float const width = afterSettled->Size.x;
        ASSERT_NE(dockID, 0u);

        // Following frames without the DockBuilder pass must not merge or collapse the node again.
        DockedPanelFrame next(false);
        ImGuiWindow * afterNext = panelWindow();
        ASSERT_NE(afterNext, nullptr);
        EXPECT_EQ(afterNext->DockId, dockID);
        EXPECT_NEAR(afterNext->Size.x, width, 1.f);
    }

    TEST_F(AgentPanelDockingFixture, ApplyDefaultLayout_AlreadySplitDockspace_PanelStillDockedRight)
    {
        DockedPanelFrame first(true, true);
        DockedPanelFrame second(false, true);

        ImGuiWindow * panel = panelWindow();
        ASSERT_NE(panel, nullptr);
        ASSERT_NE(panel->DockNode, nullptr);
        EXPECT_NE(panel->DockId, 0u);
        EXPECT_NEAR(panel->Size.x, expectedPanelWidth(), 8.f);

        // Nothing may sit to the right of the panel, and the central node stays on the other side.
        ImGuiDockNode * root = ImGui::DockNodeGetRootNode(panel->DockNode);
        EXPECT_NE(root->CentralNode, nullptr);
        EXPECT_NE(root->CentralNode, panel->DockNode);
        EXPECT_LE(root->CentralNode->Pos.x + root->CentralNode->Size.x, panel->DockNode->Pos.x);
    }

    TEST_F(AgentPanelDockingFixture, NoDefaultLayout_PanelStaysFloating)
    {
        DockedPanelFrame frame(false);

        ImGuiWindow * panel = panelWindow();
        ASSERT_NE(panel, nullptr);
        // Without the one-shot default the panel keeps ImGui's implicit floating behaviour.
        EXPECT_EQ(panel->DockId, 0u);
    }
} // namespace gladius::ui::tests
