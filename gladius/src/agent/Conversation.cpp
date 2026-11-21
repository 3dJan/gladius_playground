#include "Conversation.h"

namespace gladius::agent
{
    void Conversation::append(Message message)
    {
        m_messages.push_back(std::move(message));
    }

    void Conversation::appendUserText(std::string text)
    {
        m_messages.push_back(Message{MessageRole::User, {TextContent{std::move(text)}}});
    }

    void Conversation::appendUserImage(std::vector<std::byte> data, std::string mimeType)
    {
        m_messages.push_back(
          Message{MessageRole::User, {ImageContent{std::move(data), std::move(mimeType)}}});
    }

    void Conversation::appendAssistant(std::string text, std::vector<ToolCall> toolCalls)
    {
        Message msg;
        msg.role = MessageRole::Assistant;
        if (!text.empty())
        {
            msg.parts.emplace_back(TextContent{std::move(text)});
        }
        for (auto & call : toolCalls)
        {
            msg.parts.emplace_back(std::move(call));
        }
        m_messages.push_back(std::move(msg));
    }

    void Conversation::appendToolResult(std::string callId, nlohmann::json value)
    {
        m_messages.push_back(
          Message{MessageRole::Tool, {ToolResult{std::move(callId), std::move(value)}}});
    }

    void Conversation::clear()
    {
        m_messages.clear();
    }

    std::string encodeBase64(std::vector<std::byte> const & data)
    {
        static constexpr char const kTable[] =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string out;
        out.reserve(((data.size() + 2) / 3) * 4);

        for (std::size_t i = 0; i < data.size(); i += 3)
        {
            uint32_t const b0 = static_cast<uint8_t>(data[i]);
            uint32_t const b1 = (i + 1 < data.size()) ? static_cast<uint8_t>(data[i + 1]) : 0u;
            uint32_t const b2 = (i + 2 < data.size()) ? static_cast<uint8_t>(data[i + 2]) : 0u;
            uint32_t const triple = (b0 << 16u) | (b1 << 8u) | b2;

            out.push_back(kTable[(triple >> 18u) & 0x3Fu]);
            out.push_back(kTable[(triple >> 12u) & 0x3Fu]);
            out.push_back((i + 1 < data.size()) ? kTable[(triple >> 6u) & 0x3Fu] : '=');
            out.push_back((i + 2 < data.size()) ? kTable[triple & 0x3Fu] : '=');
        }
        return out;
    }
} // namespace gladius::agent
