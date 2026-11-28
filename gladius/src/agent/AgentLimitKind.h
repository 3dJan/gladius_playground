#pragma once

namespace gladius::agent
{
    /// @brief Execution limit that stopped a run, used in the "limit reached" notice
    enum class AgentLimitKind
    {
        ModelTurns,
        ToolCalls,
        WallTime,
        ViewportReviews
    };

    /// @brief Human-readable name of an execution limit
    inline char const * agentLimitLabel(AgentLimitKind kind) noexcept
    {
        switch (kind)
        {
        case AgentLimitKind::ModelTurns:
            return "model turns";
        case AgentLimitKind::ToolCalls:
            return "tool calls";
        case AgentLimitKind::WallTime:
            return "wall time";
        case AgentLimitKind::ViewportReviews:
            return "viewport reviews";
        }
        return "execution limit";
    }
} // namespace gladius::agent
