#pragma once

#include "Conversation.h"
#include "../tools/ToolDefinition.h"

#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>
#include <vector>

namespace gladius::agent
{
    struct TextDeltaEvent
    {
        std::string text;
    };

    struct ToolCallStartedEvent
    {
        std::string callId;
        std::string name;
        nlohmann::json arguments;
    };

    struct ToolCallCompletedEvent
    {
        std::string callId;
        std::string name;
        nlohmann::json result;
        bool success = true;
    };

    struct ImageGeneratedEvent
    {
        ImageContent image;
        std::string caption;
    };

    struct VariantCreatedEvent
    {
        std::string variantId;
        std::string name;
    };

    struct ValidationCompletedEvent
    {
        bool valid = false;
        nlohmann::json summary;
    };

    struct UserDecisionRequiredEvent
    {
        std::string requestId;
        std::string prompt;
        tools::ToolDefinition tool;
        nlohmann::json arguments;
    };

    struct AgentCompletedEvent
    {
        std::string summary;
    };

    struct AgentFailedEvent
    {
        std::string error;
    };

    using AgentEvent = std::variant<TextDeltaEvent,
                                    ToolCallStartedEvent,
                                    ToolCallCompletedEvent,
                                    ImageGeneratedEvent,
                                    VariantCreatedEvent,
                                    ValidationCompletedEvent,
                                    UserDecisionRequiredEvent,
                                    AgentCompletedEvent,
                                    AgentFailedEvent>;

    /// @brief Thread-safe queue passing agent worker events to the UI/render thread
    class AgentEventQueue
    {
      public:
        void push(AgentEvent event)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_events.push_back(std::move(event));
        }

        [[nodiscard]] std::vector<AgentEvent> drain()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::vector<AgentEvent> out;
            out.swap(m_events);
            return out;
        }

        [[nodiscard]] bool empty() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_events.empty();
        }

      private:
        mutable std::mutex m_mutex;
        std::vector<AgentEvent> m_events;
    };
} // namespace gladius::agent
