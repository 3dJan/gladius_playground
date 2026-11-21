#pragma once

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>
#include <vector>

namespace gladius::agent
{
    /// @brief Plain text content block
    struct TextContent
    {
        std::string text;
    };

    /// @brief Binary image content block (e.g. PNG viewport capture or reference image)
    struct ImageContent
    {
        std::vector<std::byte> data;
        std::string mimeType = "image/png";
    };

    /// @brief Model-requested tool invocation
    struct ToolCall
    {
        std::string id;
        std::string name;
        nlohmann::json arguments = nlohmann::json::object();
    };

    /// @brief Result of executing a ToolCall
    struct ToolResult
    {
        std::string callId;
        nlohmann::json value;
    };

    /// @brief Provider-neutral message content part
    using Content = std::variant<TextContent, ImageContent, ToolCall, ToolResult>;

    /// @brief Role of a message in a conversation
    enum class MessageRole
    {
        System,
        User,
        Assistant,
        Tool
    };

    /// @brief Single turn/message containing one or more Content parts
    struct Message
    {
        MessageRole role = MessageRole::User;
        std::vector<Content> parts;
    };

    /// @brief Provider-neutral conversation history
    class Conversation
    {
      public:
        void append(Message message);
        void appendUserText(std::string text);
        void appendUserImage(std::vector<std::byte> data, std::string mimeType = "image/png");
        void appendAssistant(std::string text, std::vector<ToolCall> toolCalls = {});
        void appendToolResult(std::string callId, nlohmann::json value);
        void clear();

        [[nodiscard]] std::vector<Message> const & messages() const noexcept
        {
            return m_messages;
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return m_messages.empty();
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return m_messages.size();
        }

      private:
        std::vector<Message> m_messages;
    };

    /// @brief Encode binary bytes to standard Base64 string
    [[nodiscard]] std::string encodeBase64(std::vector<std::byte> const & data);
} // namespace gladius::agent
