/**
 * @file AgentConversationView_tests.cpp
 * @brief Backend-less ImGui tests for the Design Agent transcript view.
 * @details The transcript is draw-list composition (measured rows, role chips, wrapped text), so a
 *          headless frame is the only way to check it: these tests assert that the turn column lines
 *          up with the phase rail above, that a collapsed transcript keeps only the newest turns,
 *          that a repeated speaker does not get a second role line, and that the tool trace stays
 *          out of the way unless it was asked for.
 */

#include "ui/AgentConversationView.h"
#include "ui/AgentPanelMetrics.h"

#include "../agent/Conversation.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

namespace gladius::ui::tests
{
    namespace
    {
        /// @brief Conversation with the parts a real run produces, tool calls and results included
        agent::Conversation makeConversation(std::size_t userTurns)
        {
            agent::Conversation conversation;
            for (std::size_t i = 0; i < userTurns; ++i)
            {
                conversation.appendUserText("Request " + std::to_string(i));
                conversation.appendAssistant(
                  "Answer " + std::to_string(i),
                  {agent::ToolCall{"call_" + std::to_string(i),
                                   "set_requirements",
                                   nlohmann::json::object()}});
                conversation.appendToolResult("call_" + std::to_string(i),
                                              nlohmann::json{{"status", "ok"}});
            }
            return conversation;
        }

        /// @brief Renders the transcript the way the panel does and collects what it drew
        class TranscriptFrame
        {
          public:
            TranscriptFrame(float panelWidth, float panelHeight)
              : m_panelWidth(panelWidth),
                m_panelHeight(panelHeight)
            {
            }

            /// @brief Renders twice; the second frame is the settled one.
            struct Facts
            {
                float rowStartX = 0.f;
                float chipX = 0.f;
                float textX = 0.f;
                float rightX = 0.f;
                std::vector<AgentConversationView::TurnLayout> turns;
                int vertices = 0;
            };

            Facts const & render(std::vector<AgentConversationView::Turn> const & turns,
                                 bool & expanded)
            {
                renderOnce(turns, expanded);
                renderOnce(turns, expanded);
                return m_facts;
            }

            /// @brief Renders one frame with the mouse at pos and the left button pressed
            Facts const & clickAt(ImVec2 pos,
                                  std::vector<AgentConversationView::Turn> const & turns,
                                  bool & expanded)
            {
                ImGuiIO & io = ImGui::GetIO();
                io.AddMousePosEvent(pos.x, pos.y);
                io.AddMouseButtonEvent(0, true);
                renderOnce(turns, expanded);
                io.AddMouseButtonEvent(0, false);
                renderOnce(turns, expanded);
                return m_facts;
            }

          private:
            void renderOnce(std::vector<AgentConversationView::Turn> const & turns, bool & expanded)
            {
                ImGuiIO & io = ImGui::GetIO();
                io.DisplaySize = ImVec2(m_panelWidth, m_panelHeight);
                io.DeltaTime = 1.f / 60.f;
                ImGui::NewFrame();

                ImGui::SetNextWindowSize(ImVec2(m_panelWidth, m_panelHeight));
                ImGui::Begin("Design Agent");
                ImGui::BeginChild("WorkflowScroll", ImVec2(0.f, -120.f), true);

                m_facts = Facts{};
                m_facts.rowStartX = ImGui::GetCursorScreenPos().x;
                m_view.render(turns, expanded);
                m_facts.chipX = m_view.lastChipX();
                m_facts.textX = m_view.lastTextX();
                m_facts.turns = m_view.lastTurns();
                m_facts.rightX = ImGui::GetCursorScreenPos().x
                               + ImGui::GetContentRegionAvail().x;

                ImGui::EndChild();
                ImGui::End();
                ImGui::Render();

                for (ImDrawList const * list : ImGui::GetDrawData()->CmdLists)
                {
                    m_facts.vertices += static_cast<int>(list->VtxBuffer.size());
                }
            }

            float m_panelWidth;
            float m_panelHeight;
            Facts m_facts;
            AgentConversationView m_view;
        };

        class TranscriptFrameFixture : public ::testing::Test
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

          private:
            ImGuiContext * m_context = nullptr;
        };

        /// @brief Column offsets of the phase rail, so the transcript can be checked against them
        struct RailColumns
        {
            float chip = 0.f;
            float title = 0.f;
        };

        /// @brief Smallest vertex x of anything drawn inside a row band of the last frame
        float minVertexXInRowBand(ImDrawData const * drawData, float rowTop, float rowBottom)
        {
            float minX = 1e9f;
            bool any = false;
            for (ImDrawList const * list : drawData->CmdLists)
            {
                for (ImDrawVert const & vert : list->VtxBuffer)
                {
                    if (vert.pos.y >= rowTop && vert.pos.y <= rowBottom)
                    {
                        minX = std::min(minX, vert.pos.x);
                        any = true;
                    }
                }
            }
            return any ? minX : 0.f;
        }

        RailColumns expectedColumns()
        {
            float const fs = ImGui::GetFontSize();
            return RailColumns{fs * agentMetrics::railOffset,
                               fs * (agentMetrics::railOffset + agentMetrics::railTextGap
                                     + agentMetrics::chevronGutter)};
        }
    } // namespace

    TEST_F(TranscriptFrameFixture, BuildTurns_ToolTraceExcluded_DropsToolAndSystemMessages)
    {
        agent::Conversation const conversation = makeConversation(2u);

        std::vector<AgentConversationView::Turn> const folded
          = AgentConversationView::buildTurns(conversation, false);
        std::vector<AgentConversationView::Turn> const trace
          = AgentConversationView::buildTurns(conversation, true);

        EXPECT_EQ(folded.size(), 4u);
        EXPECT_EQ(trace.size(), 6u);
        EXPECT_EQ(trace.back().kind, AgentConversationView::TurnKind::Tool);
        for (auto const & turn : folded)
        {
            EXPECT_NE(turn.kind, AgentConversationView::TurnKind::Tool);
        }
    }

    TEST_F(TranscriptFrameFixture, BuildTurns_ToolCallWithoutText_ShowsTheOperation)
    {
        agent::Conversation conversation;
        conversation.appendAssistant("", {agent::ToolCall{"c1", "set_requirements", {}}});

        auto const turns = AgentConversationView::buildTurns(conversation, false);

        ASSERT_EQ(turns.size(), 1u);
        EXPECT_EQ(turns.front().kind, AgentConversationView::TurnKind::Agent);
        EXPECT_EQ(turns.front().body, "Called Set Requirements");
    }

    TEST_F(TranscriptFrameFixture, BuildTurns_ImageOnlyMessage_KeepsTheTurn)
    {
        agent::Conversation conversation;
        conversation.appendUserImage(std::vector<std::byte>(8, std::byte{0x21}));

        auto const turns = AgentConversationView::buildTurns(conversation, false);

        ASSERT_EQ(turns.size(), 1u);
        EXPECT_TRUE(turns.front().hasImage);
        EXPECT_TRUE(turns.front().body.empty());
    }

    TEST_F(TranscriptFrameFixture, RenderTurns_TurnsFitTurnColumn_AliveWithPhaseRailColumns)
    {
        std::vector<AgentConversationView::Turn> const turns
          = AgentConversationView::buildTurns(makeConversation(1u), false);
        TranscriptFrame frame(420.f, 600.f);
        bool expanded = true;
        TranscriptFrame::Facts const & facts = frame.render(turns, expanded);
        RailColumns const columns = expectedColumns();

        EXPECT_NEAR(facts.chipX - facts.rowStartX, columns.chip, 1.f);
        EXPECT_NEAR(facts.textX - facts.rowStartX, columns.title, 1.f);
        ASSERT_FALSE(facts.turns.empty());
        for (auto const & turn : facts.turns)
        {
            EXPECT_GE(turn.min.x, facts.rowStartX);
            EXPECT_LE(turn.max.x, facts.rightX);
            EXPECT_GT(turn.max.y, turn.min.y);
        }
        EXPECT_GT(facts.vertices, 0);
    }

    TEST_F(TranscriptFrameFixture, RenderTurns_MoreTurnsThanBudget_DrawsOnlyTheNewest)
    {
        std::vector<AgentConversationView::Turn> const turns
          = AgentConversationView::buildTurns(makeConversation(6u), false);
        TranscriptFrame frame(420.f, 600.f);
        bool collapsed = false;
        TranscriptFrame::Facts const & collapsedFacts = frame.render(turns, collapsed);

        EXPECT_EQ(turns.size(), 12u);
        EXPECT_EQ(collapsedFacts.turns.size(), 4u);
        EXPECT_EQ(collapsedFacts.turns.back().kind, AgentConversationView::TurnKind::Agent);
    }

    TEST_F(TranscriptFrameFixture, RenderTurns_ClickCollapsedTurn_ExpandsTheTranscript)
    {
        std::vector<AgentConversationView::Turn> const turns
          = AgentConversationView::buildTurns(makeConversation(6u), false);
        TranscriptFrame frame(420.f, 600.f);
        bool expanded = false;
        TranscriptFrame::Facts const & collapsed = frame.render(turns, expanded);
        ASSERT_FALSE(expanded);

        AgentConversationView::TurnLayout const & first = collapsed.turns.front();
        TranscriptFrame::Facts const & after
          = frame.clickAt(ImVec2(first.textX + 10.f, (first.min.y + first.max.y) * 0.5f),
                          turns,
                          expanded);

        EXPECT_TRUE(expanded);
        EXPECT_EQ(after.turns.size(), turns.size());
    }

    TEST_F(TranscriptFrameFixture, RenderTurns_LongBodyWhileCollapsed_IsMarkedTruncated)
    {
        std::vector<AgentConversationView::Turn> turns;
        turns.push_back(AgentConversationView::Turn{
          AgentConversationView::TurnKind::Agent,
          "Agent",
          "The screw axis has to be fixed before the jaw angle, because the opening requirement "
          "follows from the throat length and not the other way round, and a thin jaw root is a "
          "sign the throat is too short rather than that the jaw needs more material.",
          false});
        TranscriptFrame frame(420.f, 600.f);
        bool collapsed = false;
        TranscriptFrame::Facts const & facts = frame.render(turns, collapsed);

        ASSERT_EQ(facts.turns.size(), 1u);
        EXPECT_TRUE(facts.turns.front().truncated);
    }

    TEST_F(TranscriptFrameFixture, RenderTurns_SecondTurnOfSameSpeaker_SkipsTheRoleLine)
    {
        using Turn = AgentConversationView::Turn;
        using TurnKind = AgentConversationView::TurnKind;
        std::vector<Turn> turns{
          Turn{TurnKind::User, "You", "one", false},
          Turn{TurnKind::User, "You", "two", false},
        };

        TranscriptFrame frame(420.f, 600.f);
        bool expanded = true;
        TranscriptFrame::Facts const & facts = frame.render(turns, expanded);
        ASSERT_EQ(facts.turns.size(), 2u);

        float const lineHeight = ImGui::GetTextLineHeightWithSpacing();
        float const fs = ImGui::GetFontSize();
        auto heightOf = [](AgentConversationView::TurnLayout const & t) {
            return t.max.y - t.min.y;
        };
        float const withRole = heightOf(facts.turns.front());
        float const withoutRole = heightOf(facts.turns.back());
        EXPECT_NEAR(withRole - withoutRole,
                    lineHeight + fs * agentMetrics::turnRoleLineGap,
                    0.01f);
    }

    TEST_F(TranscriptFrameFixture, RenderTurns_NarrowPanelRowsStayInsideChildWindow)
    {
        std::vector<AgentConversationView::Turn> const turns
          = AgentConversationView::buildTurns(makeConversation(2u), true);
        TranscriptFrame frame(240.f, 600.f);
        bool expanded = false;
        TranscriptFrame::Facts const & facts = frame.render(turns, expanded);

        ASSERT_FALSE(facts.turns.empty());
        for (auto const & turn : facts.turns)
        {
            EXPECT_GT(turn.max.x, turn.textX);
            EXPECT_LE(turn.max.x, facts.rightX);
        }
    }

    TEST_F(TranscriptFrameFixture, RenderEmptyHint_NoTurns_ReservesRowAtTurnColumn)
    {
        AgentConversationView view;
        ImGuiIO & io = ImGui::GetIO();
        io.DisplaySize = ImVec2(420.f, 600.f);
        io.DeltaTime = 1.f / 60.f;
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(420.f, 600.f));
        ImGui::Begin("Design Agent");
        ImGui::BeginChild("WorkflowScroll", ImVec2(0.f, -120.f), true);
        float const rowStartX = ImGui::GetCursorScreenPos().x;
        view.renderEmptyHint();
        float const rowTop = ImGui::GetItemRectMin().y;
        float const rowBottom = ImGui::GetItemRectMax().y;
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();

        RailColumns const columns = expectedColumns();
        EXPECT_TRUE(view.lastTurns().empty());
        EXPECT_NEAR(view.lastChipX() - rowStartX, columns.chip, 1.f);
        EXPECT_GT(view.lastTextX(), view.lastChipX());
        // The hint is drawn, not submitted, so only the geometry proves where it landed. Nothing
        // else occupies that row, so no glyph may start left of the turn column.
        EXPECT_GE(minVertexXInRowBand(ImGui::GetDrawData(), rowTop, rowBottom),
                  view.lastTextX() - 1.f);
    }
} // namespace gladius::ui::tests
