/**
 * @file PromptInputCard_render_tests.cpp
 * @brief Backend-less ImGui tests for the prompt composer card.
 * @details The card is real widget composition (child window, borderless field, placeholder,
 *          toolbar rows), so these frames prove it draws, that the placeholder only appears while
 *          the draft is empty, and that the model pickers wrap onto their own toolbar row instead
 *          of being squeezed or clipped on a narrow panel.
 */

#include "ui/PromptInput.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <utility>
#include <vector>

namespace gladius::ui::tests
{
    namespace
    {
        /// @brief Item spacing the default theme has in a headless frame
        constexpr float kItemSpacingX = 8.f;
        constexpr float kItemSpacingY = 4.f;

        /// @brief Layout facts measured from one rendered composer frame
        struct CardFrameFacts
        {
            PromptInput::CardMetrics metrics;
            ImVec2 fieldSize;
            /// @brief Row index, usable combo width and row host width per submitted combo
            struct ComboFacts
            {
                int row = 0;
                float width = 0.f;
                float hostWidth = 0.f;
                float leadingWidth = 0.f;
                ImVec2 rectMin;
                ImVec2 rectMax;
            };
            std::vector<ComboFacts> combos;
            int vertices = 0;
        };

        /**
         * @brief Renders the composer card inside a plain window, backend-less, the way the
         *        Design Agent footer does.
         * @details The attachment slot is used as a probe: it runs directly after the text field,
         *          so the last submitted item is the field itself.
         */
        class CardFrame
        {
          public:
            explicit CardFrame(float panelWidth)
              : m_panelWidth(panelWidth)
            {
            }

            /// @brief Renders twice and returns the facts of the settled frame.
            /// @details ImGui clamps a window it has just created on its first frame, so the first
            ///          frame of a context draws slightly differently than the steady state.
            CardFrameFacts const & render(PromptInput & prompt, bool sendEnabled = true)
            {
                renderOnce(prompt, sendEnabled);
                renderOnce(prompt, sendEnabled);
                return m_facts;
            }

          private:
            void renderOnce(PromptInput & prompt, bool sendEnabled)
            {
                ImGuiIO & io = ImGui::GetIO();
                io.DisplaySize = ImVec2(m_panelWidth, 600.f);
                io.DeltaTime = 1.f / 60.f;
                ImGui::NewFrame();
                ImGui::SetNextWindowSize(ImVec2(m_panelWidth, 600.f));
                ImGui::Begin("Design Agent");

                m_facts = CardFrameFacts{};
                m_facts.metrics = prompt.measureCard(0.f);

                PromptInput::SlotRenderer const attachments = [this](float) {
                    // The slot runs directly after the text field, so the item rect is the field.
                    m_facts.fieldSize = ImGui::GetItemRectSize();
                };

                PromptInput::ToolbarRenderer const toolbar
                  = [this](PromptInput::ToolbarLayout const & layout, int row) {
                        if (row + 1 != layout.rowCount || !layout.drawCombos)
                        {
                            return;
                        }
                        // Mirrors AgentPanels::renderComposerToolbar: the combos continue the
                        // leading row, and the second combo continues the first one.
                        if (row == 0)
                        {
                            ImGui::SameLine();
                        }
                        bool first = true;
                        for (char const * id : {"##PrimaryModel", "##FallbackModels"})
                        {
                            if (!first)
                            {
                                ImGui::SameLine();
                            }
                            first = false;
                            ImGui::SetNextItemWidth(layout.comboWidth);
                            int current = 0;
                            ImGui::Combo(id, &current, "Alpha\0Beta\0");
                            m_facts.combos.push_back({row,
                                                      ImGui::GetItemRectSize().x,
                                                      layout.hostWidth,
                                                      layout.leadingWidth,
                                                      ImGui::GetItemRectMin(),
                                                      ImGui::GetItemRectMax()});
                        }
                    };

                (void) prompt.renderCard(m_facts.metrics, attachments, toolbar, sendEnabled);
                ImGui::End();
                ImGui::Render();

                for (ImDrawList const * list : ImGui::GetDrawData()->CmdLists)
                {
                    m_facts.vertices += static_cast<int>(list->VtxBuffer.size());
                }
            }

            float m_panelWidth = 0.f;
            CardFrameFacts m_facts;
        };

        /// @brief Backend-less ImGui context, enough to submit widgets and read back draw data
        class PromptCardFixture : public ::testing::Test
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

    TEST_F(PromptCardFixture, RenderCard_EmptyDraft_PlaceholderAddsGlyphs)
    {
        CardFrame withPlaceholder(420.f);
        PromptInput prompt;
        CardFrameFacts const & drawn = withPlaceholder.render(prompt);

        CardFrame withoutPlaceholder(420.f);
        PromptInput::Config config;
        config.placeholder = "";
        PromptInput suppressed(config);
        CardFrameFacts const & hidden = withoutPlaceholder.render(suppressed);

        // The placeholder is glyph geometry: an empty field without it draws nothing there.
        EXPECT_GT(drawn.vertices, hidden.vertices);
        EXPECT_GT(drawn.metrics.cardHeight, 0.f);
    }

    TEST_F(PromptCardFixture, RenderCard_FilledDraft_PlaceholderNotRedrawn)
    {
        PromptInput::Config config;
        std::string const draft = "Build a lattice lamp with a 120 mm shade";

        CardFrame withPlaceholder(420.f);
        PromptInput prompt;
        prompt.setDraft(draft);
        CardFrameFacts const & shown = withPlaceholder.render(prompt);

        CardFrame withoutPlaceholder(420.f);
        config.placeholder = "";
        PromptInput suppressed(config);
        suppressed.setDraft(draft);
        CardFrameFacts const & hidden = withoutPlaceholder.render(suppressed);

        EXPECT_EQ(shown.vertices, hidden.vertices);
        EXPECT_EQ(shown.metrics.cardHeight, hidden.metrics.cardHeight);
        EXPECT_GT(shown.fieldSize.y, 0.f);
    }

    TEST_F(PromptCardFixture, RenderCard_NarrowPanel_CombosWrapOntoSecondRow)
    {
        PromptInput prompt;

        CardFrame wide(520.f);
        CardFrameFacts const & wideFacts = wide.render(prompt);

        CardFrame narrow(300.f);
        CardFrameFacts const & narrowFacts = narrow.render(prompt);

        EXPECT_EQ(wideFacts.combos.size(), std::size_t{2});
        EXPECT_EQ(narrowFacts.combos.size(), std::size_t{2});
        for (auto const & combo : wideFacts.combos)
        {
            EXPECT_EQ(combo.row, 0);
            EXPECT_GT(combo.leadingWidth, 0.f);
            // The reserved width is what ImGui draws, so the split must leave room for the
            // leading controls instead of relying on ImGui squeezing the combos.
            EXPECT_NEAR(combo.width,
                        (combo.hostWidth - combo.leadingWidth - kItemSpacingX * 2.f) * 0.5f,
                        1.f);
        }
        EXPECT_EQ(wideFacts.combos[0].rectMin.y, wideFacts.combos[1].rectMin.y);
        EXPECT_GT(wideFacts.combos[1].rectMin.x, wideFacts.combos[0].rectMax.x);
        for (auto const & combo : narrowFacts.combos)
        {
            EXPECT_EQ(combo.row, 1);
            EXPECT_EQ(combo.leadingWidth, 0.f);
        }
        // Both combos must share the wrapped row instead of stacking again.
        EXPECT_EQ(narrowFacts.combos[0].rectMin.y, narrowFacts.combos[1].rectMin.y);
        EXPECT_LT(narrowFacts.combos[1].rectMax.x, narrowFacts.combos[0].rectMin.x + 1e6f);
        EXPECT_GT(narrowFacts.metrics.cardHeight, wideFacts.metrics.cardHeight);
        EXPECT_EQ(narrowFacts.metrics.toolbarRows, 2);
    }

    TEST_F(PromptCardFixture, RenderCard_LongerDraft_CardGrowsWithinMaxHeight)
    {
        PromptInput shortPrompt;
        CardFrame shortFrame(420.f);
        CardFrameFacts const & shortFacts = shortFrame.render(shortPrompt);

        PromptInput longPrompt;
        longPrompt.setDraft(std::string(2000u, 'a'));
        CardFrame longFrame(420.f);
        CardFrameFacts const & longFacts = longFrame.render(longPrompt);

        EXPECT_GT(longFacts.metrics.fieldHeight, shortFacts.metrics.fieldHeight);
        EXPECT_GT(longFacts.metrics.cardHeight, shortFacts.metrics.cardHeight);
        EXPECT_LE(longFacts.metrics.fieldHeight, longPrompt.config().maxHeight);
        EXPECT_GE(longFacts.fieldSize.x, 40.f);
    }
} // namespace gladius::ui::tests
