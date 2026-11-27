#pragma once

namespace gladius::design
{
    class DesignGraph;
}

namespace gladius::agent
{
    class Conversation;
}

namespace gladius::ui
{
    /**
     * @brief Demo content for reviewing the Design Agent panel without an AI provider.
     * @details The panel is only populated by a run, so a styling pass has nothing to look at in a
     *          browser build or a fresh session. Setting GLADIUS_AGENT_DEMO (native) or adding
     *          ?agentDemo=1 (web) fills the design graph and the transcript with a short example
     *          session. Nothing else reads it; it is presentation scaffolding, not a test double.
     */
    namespace agentDemo
    {
        /// @brief True when the demo was requested by environment or query parameter
        [[nodiscard]] bool requested();

        /// @brief Fills an empty graph and conversation with one short example session
        void populate(design::DesignGraph & graph, agent::Conversation & conversation);
    } // namespace agentDemo
} // namespace gladius::ui
