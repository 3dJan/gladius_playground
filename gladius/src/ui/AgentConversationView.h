#pragma once

#include "../agent/Conversation.h"

#include <cstddef>
#include <imgui.h>
#include <string>
#include <vector>

namespace gladius::ui
{
    /**
     * @brief Renders the Design Agent transcript as turns: a role chip, a role line, and the text.
     * @details Pure presentation. It reads the conversation and draws; it never changes agent state
     *          and owns no expansion flags, so the host stays the single owner of UI state. The
     *          turn column is the phase title column of the rail above, which is what makes the
     *          transcript read as a continuation of the workflow instead of a second list.
     */
    class AgentConversationView
    {
      public:
        /// @brief What a turn is, once the caller has filtered it
        enum class TurnKind
        {
            Agent,
            User,
            System,
            Tool
        };

        /// @brief One transcript entry ready to draw
        struct Turn
        {
            TurnKind kind = TurnKind::Agent;
            /// @brief Heading text: role, or the operation for a tool trace
            std::string title;
            /// @brief Wrapped body text; empty for single-line tool traces
            std::string body;
            /// @brief True when the message carried an image the transcript cannot show
            bool hasImage = false;
        };

        /// @brief Geometry of one turn of the last render pass
        struct TurnLayout
        {
            ImVec2 min{};
            ImVec2 max{};
            float chipX = 0.f;
            float textX = 0.f;
            TurnKind kind = TurnKind::Agent;
            /// @brief True when the body was cut short by the collapsed budget
            bool truncated = false;
        };

        /**
         * @brief Turns for a conversation, in the order they should be drawn.
         * @param conversation Transcript to read; never modified
         * @param includeToolTrace Keep the system notes and tool traces instead of folding them away
         */
        [[nodiscard]] static std::vector<Turn> buildTurns(
          agent::Conversation const & conversation,
          bool includeToolTrace);

        /**
         * @brief Draws the turns into the current (scrollable) window.
         * @param turns Turns from buildTurns, oldest first
         * @param expanded In/out flag owned by the caller. Collapsed draws only the newest turns
         *                 with short bodies; clicking a row or the "show all" row turns it on.
         */
        void render(std::vector<Turn> const & turns, bool & expanded);

        /// @brief Placeholder line for an empty transcript, so the section is never invisible
        void renderEmptyHint();

        /// @brief Turns drawn by the last render pass, oldest first
        [[nodiscard]] std::vector<TurnLayout> const & lastTurns() const noexcept
        {
            return m_lastTurns;
        }

        /// @brief Left edge of the role chips of the last render pass
        [[nodiscard]] float lastChipX() const noexcept
        {
            return m_chipX;
        }

        /// @brief Left edge of the turn text of the last render pass
        [[nodiscard]] float lastTextX() const noexcept
        {
            return m_textX;
        }

      private:
        /// @brief Measures the chip, text and right columns from the current cursor and width
        void measureColumns();

        /// @brief Draws one turn; true when the user clicked it
        bool renderTurn(Turn const & turn,
                        bool expanded,
                        bool speakerChanged,
                        bool showRole,
                        float wrapWidth,
                        float ellipsisWidth);

        /// @brief Accent-coloured text row that switches between the short and the full transcript
        bool toggleRow(char const * icon, std::string const & label);

        std::vector<TurnLayout> m_lastTurns;
        float m_chipX = 0.f;
        float m_textX = 0.f;
        float m_rightX = 0.f;
    };
} // namespace gladius::ui
