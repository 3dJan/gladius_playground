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

    std::vector<std::byte> decodeBase64(std::string const & encoded)
    {
        static int const kRev[256] = {
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62,
          -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0,
          1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
          23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
          39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

        std::vector<std::byte> out;
        out.reserve((encoded.size() / 4) * 3);
        uint32_t val = 0;
        int valb = -8;
        for (unsigned char c : encoded)
        {
            if (c == '=')
            {
                break;
            }
            int const d = kRev[c];
            if (d == -1)
            {
                continue;
            }
            val = (val << 6u) + static_cast<uint32_t>(d);
            valb += 6;
            if (valb >= 0)
            {
                out.push_back(static_cast<std::byte>((val >> valb) & 0xFFu));
                valb -= 8;
            }
        }
        return out;
    }

    nlohmann::json Conversation::toJson() const
    {
        nlohmann::json arr = nlohmann::json::array();
        for (auto const & msg : m_messages)
        {
            std::string roleStr = "user";
            switch (msg.role)
            {
            case MessageRole::System:
                roleStr = "system";
                break;
            case MessageRole::User:
                roleStr = "user";
                break;
            case MessageRole::Assistant:
                roleStr = "assistant";
                break;
            case MessageRole::Tool:
                roleStr = "tool";
                break;
            }

            nlohmann::json partsArr = nlohmann::json::array();
            for (auto const & part : msg.parts)
            {
                if (auto const * t = std::get_if<TextContent>(&part))
                {
                    partsArr.push_back({{"type", "text"}, {"text", t->text}});
                }
                else if (auto const * img = std::get_if<ImageContent>(&part))
                {
                    partsArr.push_back({{"type", "image"},
                                        {"mimeType", img->mimeType},
                                        {"dataBase64", encodeBase64(img->data)}});
                }
                else if (auto const * tc = std::get_if<ToolCall>(&part))
                {
                    partsArr.push_back({{"type", "tool_call"},
                                        {"id", tc->id},
                                        {"name", tc->name},
                                        {"arguments", tc->arguments}});
                }
                else if (auto const * tr = std::get_if<ToolResult>(&part))
                {
                    partsArr.push_back({{"type", "tool_result"},
                                        {"callId", tr->callId},
                                        {"value", tr->value}});
                }
            }
            arr.push_back({{"role", roleStr}, {"parts", std::move(partsArr)}});
        }
        return arr;
    }

    bool Conversation::fromJson(nlohmann::json const & json)
    {
        if (!json.is_array())
        {
            return false;
        }
        std::vector<Message> loaded;
        for (auto const & item : json)
        {
            if (!item.is_object())
            {
                continue;
            }
            Message msg;
            std::string const roleStr = item.value("role", "user");
            if (roleStr == "system")
            {
                msg.role = MessageRole::System;
            }
            else if (roleStr == "assistant")
            {
                msg.role = MessageRole::Assistant;
            }
            else if (roleStr == "tool")
            {
                msg.role = MessageRole::Tool;
            }
            else
            {
                msg.role = MessageRole::User;
            }

            if (item.contains("parts") && item["parts"].is_array())
            {
                for (auto const & p : item["parts"])
                {
                    std::string const typeStr = p.value("type", "");
                    if (typeStr == "text")
                    {
                        msg.parts.emplace_back(TextContent{p.value("text", "")});
                    }
                    else if (typeStr == "image")
                    {
                        msg.parts.emplace_back(ImageContent{
                          decodeBase64(p.value("dataBase64", "")),
                          p.value("mimeType", "image/png")});
                    }
                    else if (typeStr == "tool_call")
                    {
                        msg.parts.emplace_back(ToolCall{
                          p.value("id", ""),
                          p.value("name", ""),
                          p.value("arguments", nlohmann::json::object())});
                    }
                    else if (typeStr == "tool_result")
                    {
                        msg.parts.emplace_back(ToolResult{
                          p.value("callId", ""),
                          p.value("value", nlohmann::json::object())});
                    }
                }
            }
            if (!msg.parts.empty())
            {
                loaded.push_back(std::move(msg));
            }
        }
        m_messages = std::move(loaded);
        return true;
    }
} // namespace gladius::agent
