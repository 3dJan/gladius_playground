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
        constexpr ImVec2 kPhoneSize{390.f, 700.f};
        constexpr char const * kPreviewWindowName = "Preview";

        /// @brief Layout pass the frame should run after creating the dockspace
        enum class LayoutPass
        {
            None,
            Default,
            Compact
        };

        /// @brief Extra frame options: a second docked window and a main-space handover
        struct FrameOptions
        {
            bool preSplitRoot{false};
            ImVec2 size{kDockspaceSize};
            /// @brief Window submitted in addition to the panel; nullptr submits only the panel
            char const * extraWindowName{nullptr};
            /// @brief Hand the main space to extraWindowName during this frame
            bool handOverMainSpace{false};
            /// @brief Window that must give the main space up during the handover
            char const * handoverSurrender{nullptr};
        };

        /// @brief One frame of the main dockspace plus the Design Agent panel
        class DockedPanelFrame
        {
          public:
            DockedPanelFrame(LayoutPass layoutPass, FrameOptions options = {})
            {
                ImGuiIO & io = ImGui::GetIO();
                io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
                io.DisplaySize = options.size;

                ImGui::NewFrame();

                // A zero padded, full size host window makes the dockspace area exactly kDockspaceSize.
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
                ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
                ImGui::SetNextWindowSize(options.size);
                ImGui::Begin("MainWindowDockingArea",
                             nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking);
                const ImGuiID dockspaceID = ImGui::GetID("MainDockingSpace");
                ImGui::DockSpace(dockspaceID,
                                 ImVec2(0.f, 0.f),
                                 ImGuiDockNodeFlags_None | ImGuiDockNodeFlags_PassthruCentralNode);
                if (options.preSplitRoot)
                {
                    // The shipped imgui.ini already hands us a split dockspace tree.
                    ImGuiID leftID = 0;
                    ImGuiID restID = 0;
                    ImGui::DockBuilderSplitNode(dockspaceID, ImGuiDir_Left, 0.25f, &leftID, &restID);
                    ImGui::DockBuilderFinish(dockspaceID);
                }
                switch (layoutPass)
                {
                  case LayoutPass::Default:
                    applyAgentPanelDefaultLayout(dockspaceID);
                    break;
                  case LayoutPass::Compact:
                    applyCompactLayout(dockspaceID);
                    break;
                  case LayoutPass::None:
                    break;
                }
                if (options.handOverMainSpace)
                {
                    if (options.handoverSurrender != nullptr)
                    {
                        showWindowInMainSpace(dockspaceID,
                                              options.extraWindowName,
                                              {options.handoverSurrender});
                    }
                    else
                    {
                        showWindowInMainSpace(dockspaceID, options.extraWindowName);
                    }
                }
                ImGui::End();
                ImGui::PopStyleVar();

                ImGui::Begin(kAgentPanelWindowName, nullptr, ImGuiWindowFlags_NoTitleBar);
                ImGui::TextUnformatted("panel");
                ImGui::End();

                if (options.extraWindowName != nullptr)
                {
                    ImGui::Begin(options.extraWindowName, nullptr, ImGuiWindowFlags_NoTitleBar);
                    ImGui::TextUnformatted("extra");
                    ImGui::End();
                }

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
        DockedPanelFrame frame(LayoutPass::Default);

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
        DockedPanelFrame first(LayoutPass::Default);
        DockedPanelFrame second(LayoutPass::None);

        ImGuiWindow * panel = panelWindow();
        ASSERT_NE(panel, nullptr);
        EXPECT_NEAR(panel->Size.x, expectedPanelWidth(), 8.f);
    }

    TEST_F(AgentPanelDockingFixture, ApplyDefaultLayout_DockedNode_HasNoTabBar)
    {
        DockedPanelFrame first(LayoutPass::Default);
        DockedPanelFrame second(LayoutPass::None);

        ImGuiWindow * panel = panelWindow();
        ASSERT_NE(panel, nullptr);
        ASSERT_NE(panel->DockNode, nullptr);
        // The panel draws its own header, so its dock node must not present a tab bar.
        EXPECT_TRUE(panel->DockNode->IsNoTabBar());
    }

    TEST_F(AgentPanelDockingFixture, DockedPanel_FramesWithoutDockBuilder_KeepDockIdAndWidth)
    {
        DockedPanelFrame first(LayoutPass::Default);
        // ImGui needs one more frame before the dock node hosts the window, so settle before measuring.
        DockedPanelFrame settled(LayoutPass::None);
        ImGuiWindow * afterSettled = panelWindow();
        ASSERT_NE(afterSettled, nullptr);
        ImGuiID const dockID = afterSettled->DockId;
        float const width = afterSettled->Size.x;
        ASSERT_NE(dockID, 0u);

        // Following frames without the DockBuilder pass must not merge or collapse the node again.
        DockedPanelFrame next(LayoutPass::None);
        ImGuiWindow * afterNext = panelWindow();
        ASSERT_NE(afterNext, nullptr);
        EXPECT_EQ(afterNext->DockId, dockID);
        EXPECT_NEAR(afterNext->Size.x, width, 1.f);
    }

    TEST_F(AgentPanelDockingFixture, ApplyDefaultLayout_AlreadySplitDockspace_PanelStillDockedRight)
    {
        DockedPanelFrame first(LayoutPass::Default, FrameOptions{.preSplitRoot = true});
        DockedPanelFrame second(LayoutPass::None, FrameOptions{.preSplitRoot = true});

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
        DockedPanelFrame frame(LayoutPass::None);

        ImGuiWindow * panel = panelWindow();
        ASSERT_NE(panel, nullptr);
        // Without the one-shot default the panel keeps ImGui's implicit floating behaviour.
        EXPECT_EQ(panel->DockId, 0u);
    }

    TEST_F(AgentPanelDockingFixture, ApplyCompactLayout_PanelFillsTheWholeMainSpace)
    {
        FrameOptions const options{.size = kPhoneSize};
        DockedPanelFrame first(LayoutPass::Compact, options);
        DockedPanelFrame settled(LayoutPass::None, options);

        ImGuiWindow * panel = panelWindow();
        ASSERT_NE(panel, nullptr);
        ASSERT_NE(panel->DockNode, nullptr);
        EXPECT_NEAR(panel->Size.x, kPhoneSize.x, 8.f);
        EXPECT_NEAR(panel->Size.y, kPhoneSize.y, 8.f);

        // Nothing may be split off: the panel's node is the root of the dockspace.
        EXPECT_EQ(ImGui::DockNodeGetRootNode(panel->DockNode)->ID, settled.dockspaceID());
        EXPECT_EQ(panel->DockNode->ParentNode, nullptr);
        EXPECT_TRUE(panel->DockNode->IsNoTabBar());
    }

    TEST_F(AgentPanelDockingFixture, ApplyCompactLayout_AlreadySplitDockspace_SplitIsRemoved)
    {
        DockedPanelFrame first(LayoutPass::Compact, FrameOptions{.preSplitRoot = true,
                                                                 .size = kPhoneSize});
        // Later frames no longer hand us the saved split: clearStoredLayout dropped it.
        DockedPanelFrame settled(LayoutPass::None, FrameOptions{.size = kPhoneSize});

        ImGuiWindow * panel = panelWindow();
        ASSERT_NE(panel, nullptr);
        ASSERT_NE(panel->DockNode, nullptr);
        EXPECT_EQ(panel->DockNode->ParentNode, nullptr);
        EXPECT_NEAR(panel->Size.x, kPhoneSize.x, 8.f);
    }

    TEST_F(AgentPanelDockingFixture, ApplyCompactLayout_DockspaceNarrowerThanDesktopMinWidth_StillFillsIt)
    {
        // The desktop path clamps the panel to a minimum width that is wider than a phone; the
        // compact path must not inherit that clamp or the panel would overflow the screen.
        ImVec2 const narrowSize{320.f, 600.f};
        DockedPanelFrame first(LayoutPass::Compact, FrameOptions{.size = narrowSize});
        DockedPanelFrame settled(LayoutPass::None, FrameOptions{.size = narrowSize});

        ImGuiWindow * panel = panelWindow();
        ASSERT_NE(panel, nullptr);
        EXPECT_NEAR(panel->Size.x, narrowSize.x, 8.f);
        EXPECT_LT(panel->Size.x, agentMetrics::panelMinWidthPx);
    }

    TEST_F(AgentPanelDockingFixture, ClearStoredLayout_AppliedSplit_PanelIsUndockedAgain)
    {
        DockedPanelFrame first(LayoutPass::Default);
        ImGuiWindow * docked = panelWindow();
        ASSERT_NE(docked, nullptr);
        ASSERT_NE(docked->DockId, 0u);

        DockedPanelFrame cleared(LayoutPass::None);
        // A profile switch has to win over the layout that was just stored, otherwise a phone
        // keeps the desktop side column it saved moments earlier.
        clearStoredLayout(cleared.dockspaceID(), {kAgentPanelWindowName});
        DockedPanelFrame afterClear(LayoutPass::Compact);

        ImGuiWindow * panel = panelWindow();
        ASSERT_NE(panel, nullptr);
        EXPECT_NE(panel->DockId, 0u);
        ASSERT_NE(panel->DockNode, nullptr);
        // The old right-hand split is gone, so the panel now owns the root node.
        EXPECT_EQ(panel->DockNode->ParentNode, nullptr);
        EXPECT_NEAR(panel->Size.x, kDockspaceSize.x, 8.f);
    }

    TEST_F(AgentPanelDockingFixture, ShowWindowInMainSpace_SecondWindowTakesTheMainSpace)
    {
        FrameOptions const options{.size = kPhoneSize,
                                    .extraWindowName = kPreviewWindowName,
                                    .handOverMainSpace = true,
                                    .handoverSurrender = kAgentPanelWindowName};
        DockedPanelFrame first(LayoutPass::Compact, options);
        DockedPanelFrame settled(LayoutPass::None, options);

        ImGuiWindow * preview = ImGui::FindWindowByName(kPreviewWindowName);
        ASSERT_NE(preview, nullptr);
        ASSERT_NE(preview->DockNode, nullptr);
        EXPECT_EQ(ImGui::DockNodeGetRootNode(preview->DockNode)->ID, settled.dockspaceID());
        EXPECT_NEAR(preview->Size.x, kPhoneSize.x, 8.f);

        // The compact layout gave the main space to the panel, so taking it away must leave the
        // panel without a dock reference; otherwise it would float back on its next frame.
        ImGuiWindow * panel = panelWindow();
        ASSERT_NE(panel, nullptr);
        EXPECT_EQ(panel->DockId, 0u);
    }
} // namespace gladius::ui::tests
