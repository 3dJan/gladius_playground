#include "AnthropicProvider.h"

#include <map>

namespace gladius::agent
{
    namespace
    {
        struct PendingAnthropicBlock
        {
            std::string type;
            std::string id;
            std::string name;
            std::string inputJson;
        };

        std::string resolveAnthropicUrl(std::string endpoint)
        {
            if (endpoint.empty())
            {
                return "https://api.anthropic.com/v1/messages";
            }
            while (!endpoint.empty() && endpoint.back() == '/')
            {
                endpoint.pop_back();
            }
            if (endpoint.size() >= 9 && endpoint.substr(endpoint.size() - 9) == "/messages")
            {
                return endpoint;
            }
            return endpoint + "/messages";
        }
    } // namespace

    AnthropicProvider::AnthropicProvider(ProviderConfig config,
                                         std::shared_ptr<IHttpTransport> transport)
        : m_config(std::move(config))
        , m_transport(transport ? std::move(transport) : std::make_shared<CurlHttpTransport>())
    {
    }

    nlohmann::json AnthropicProvider::buildPayload(CompletionRequest const & request) const
    {
        nlohmann::json messages = nlohmann::json::array();

        for (auto const & msg : request.messages)
        {
            if (msg.role == MessageRole::System)
            {
                continue;
            }

            std::string const roleStr =
              (msg.role == MessageRole::Assistant) ? "assistant" : "user";
            nlohmann::json contentArr = nlohmann::json::array();

            for (auto const & part : msg.parts)
            {
                if (auto const * t = std::get_if<TextContent>(&part))
                {
                    contentArr.push_back({{"type", "text"}, {"text", t->text}});
                }
                else if (auto const * img = std::get_if<ImageContent>(&part))
                {
                    contentArr.push_back({
                        {"type", "image"},
                        {"source", {
                            {"type", "base64"},
                            {"media_type", img->mimeType},
                            {"data", encodeBase64(img->data)}
                        }}
                    });
                }
                else if (auto const * tc = std::get_if<ToolCall>(&part))
                {
                    contentArr.push_back({
                        {"type", "tool_use"},
                        {"id", tc->id},
                        {"name", tc->name},
                        {"input", tc->arguments.is_object() ? tc->arguments : nlohmann::json::object()}
                    });
                }
                else if (auto const * tr = std::get_if<ToolResult>(&part))
                {
                    contentArr.push_back({
                        {"type", "tool_result"},
                        {"tool_use_id", tr->callId},
                        {"content", tr->value.is_string() ? tr->value.get<std::string>() : tr->value.dump()}
                    });
                }
            }

            messages.push_back({{"role", roleStr}, {"content", std::move(contentArr)}});
        }

        nlohmann::json payload = {
            {"model", m_config.model},
            {"max_tokens", m_config.maxTokens},
            {"temperature", m_config.temperature},
            {"stream", m_config.stream},
            {"messages", std::move(messages)}
        };

        if (!request.systemPrompt.empty())
        {
            payload["system"] = request.systemPrompt;
        }

        if (!request.tools.empty())
        {
            nlohmann::json toolsArr = nlohmann::json::array();
            for (auto const & tool : request.tools)
            {
                toolsArr.push_back({
                    {"name", tool.name},
                    {"description", tool.description},
                    {"input_schema", tool.inputSchema.is_object() ? tool.inputSchema : nlohmann::json{{"type", "object"}}}
                });
            }
            payload["tools"] = std::move(toolsArr);
        }

        return payload;
    }

    CompletionResponse AnthropicProvider::parseResponseJson(nlohmann::json const & body)
    {
        CompletionResponse resp;
        if (body.contains("error") && body["error"].is_object())
        {
            resp.success = false;
            resp.error = body["error"].value("message", body["error"].dump());
            return resp;
        }

        resp.stopReason = body.value("stop_reason", "");
        if (body.contains("content") && body["content"].is_array())
        {
            for (auto const & block : body["content"])
            {
                std::string const type = block.value("type", "");
                if (type == "text")
                {
                    resp.text += block.value("text", "");
                }
                else if (type == "tool_use")
                {
                    ToolCall call;
                    call.id = block.value("id", "");
                    call.name = block.value("name", "");
                    if (block.contains("input") && block["input"].is_object())
                    {
                        call.arguments = block["input"];
                    }
                    resp.toolCalls.push_back(std::move(call));
                }
            }
        }
        return resp;
    }

    coro::task<CompletionResponse>
    AnthropicProvider::complete(CompletionRequest const & request,
                                TextDeltaCallback onDelta,
                                io::CancellationToken const & cancellation)
    {
        HttpRequest httpReq;
        httpReq.url = resolveAnthropicUrl(m_config.endpoint);
        httpReq.headers.emplace_back("Content-Type: application/json");
        httpReq.headers.emplace_back("anthropic-version: 2023-06-01");
        if (!m_config.apiKey.empty())
        {
            httpReq.headers.emplace_back("x-api-key: " + m_config.apiKey);
        }
        httpReq.body = buildPayload(request).dump();

        if (!m_config.stream)
        {
            HttpResponse const httpRes = m_transport->postJson(httpReq, cancellation);
            if (httpRes.cancelled || !httpRes.error.empty())
            {
                co_return CompletionResponse{{}, {}, "", false, httpRes.error};
            }
            if (httpRes.statusCode < 200 || httpRes.statusCode >= 300)
            {
                co_return CompletionResponse{
                    {}, {}, "", false,
                    "HTTP " + std::to_string(httpRes.statusCode) + ": " + httpRes.body};
            }
            auto parsed = nlohmann::json::parse(httpRes.body, nullptr, false);
            if (parsed.is_discarded())
            {
                co_return CompletionResponse{{}, {}, "", false, "Invalid JSON from Anthropic endpoint"};
            }
            CompletionResponse resp = parseResponseJson(parsed);
            if (resp.success && onDelta && !resp.text.empty())
            {
                onDelta(resp.text);
            }
            co_return resp;
        }

        CompletionResponse resp;
        std::map<int, PendingAnthropicBlock> blocks;

        HttpResponse const httpRes = m_transport->postSse(
          httpReq,
          [&](std::string const & eventName, std::string const & data)
          {
              auto payload = nlohmann::json::parse(data, nullptr, false);
              if (payload.is_discarded())
              {
                  return;
              }
              std::string const type = !eventName.empty() ? eventName : payload.value("type", "");
              if (type == "error" && payload.contains("error"))
              {
                  resp.success = false;
                  resp.error = payload["error"].value("message", payload["error"].dump());
                  return;
              }
              if (type == "content_block_start" && payload.contains("content_block"))
              {
                  int const idx = payload.value("index", 0);
                  auto const & cb = payload["content_block"];
                  PendingAnthropicBlock block;
                  block.type = cb.value("type", "");
                  block.id = cb.value("id", "");
                  block.name = cb.value("name", "");
                  blocks[idx] = std::move(block);
              }
              else if (type == "content_block_delta" && payload.contains("delta"))
              {
                  int const idx = payload.value("index", 0);
                  auto const & delta = payload["delta"];
                  std::string const deltaType = delta.value("type", "");
                  if (deltaType == "text_delta")
                  {
                      std::string const piece = delta.value("text", "");
                      resp.text += piece;
                      if (onDelta && !piece.empty())
                      {
                          onDelta(piece);
                      }
                  }
                  else if (deltaType == "input_json_delta")
                  {
                      blocks[idx].inputJson += delta.value("partial_json", "");
                  }
              }
              else if (type == "message_delta" && payload.contains("delta"))
              {
                  resp.stopReason = payload["delta"].value("stop_reason", resp.stopReason);
              }
          },
          cancellation);

        if (httpRes.cancelled || !httpRes.error.empty())
        {
            resp.success = false;
            resp.error = httpRes.error;
            co_return resp;
        }
        if (httpRes.statusCode != 0 && (httpRes.statusCode < 200 || httpRes.statusCode >= 300))
        {
            resp.success = false;
            resp.error = "HTTP " + std::to_string(httpRes.statusCode) + ": " + httpRes.body;
            co_return resp;
        }

        for (auto & [idx, block] : blocks)
        {
            if (block.type == "tool_use")
            {
                ToolCall call;
                call.id = std::move(block.id);
                call.name = std::move(block.name);
                if (!block.inputJson.empty())
                {
                    auto parsedArgs = nlohmann::json::parse(block.inputJson, nullptr, false);
                    call.arguments = parsedArgs.is_discarded() ? nlohmann::json::object() : parsedArgs;
                }
                resp.toolCalls.push_back(std::move(call));
            }
        }

        co_return resp;
    }
} // namespace gladius::agent
