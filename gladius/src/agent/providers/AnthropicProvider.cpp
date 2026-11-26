#include "AnthropicProvider.h"

#include "CompletionStopwatch.h"
#include "ProviderError.h"

#include <map>

namespace gladius::agent
{
    namespace
    {
        /// @brief Fills `errorInfo` when a path produced only a raw provider string
        void classifyFailure(CompletionResponse & resp,
                             bool apiKeyEmpty,
                             std::string const & model)
        {
            if (!resp.errorInfo.isSet())
            {
                resp.errorInfo = classifyErrorText(resp.error, apiKeyEmpty, false, model);
            }
        }

        struct PendingAnthropicBlock
        {
            std::string type;
            std::string id;
            std::string name;
            std::string inputJson;
        };

        /// @brief Merge the Anthropic `usage` object of a payload into `usage`.
        /// Present keys overwrite; absent keys keep their previous value, so the `message_start`
        /// prompt totals survive a `message_delta` that only reports cumulative output tokens.
        void mergeAnthropicUsage(nlohmann::json const & payload, TokenUsage & usage)
        {
            if (!payload.contains("usage") || !payload["usage"].is_object())
            {
                return;
            }
            auto const & u = payload["usage"];
            auto const readInto = [&u](char const * key, std::int64_t & target)
            {
                if (u.contains(key) && u[key].is_number())
                {
                    target = u[key].get<std::int64_t>();
                }
            };
            readInto("input_tokens", usage.inputTokens);
            readInto("output_tokens", usage.outputTokens);
            readInto("cache_read_input_tokens", usage.cacheReadTokens);
            readInto("cache_creation_input_tokens", usage.cacheWriteTokens);
        }

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

        // The endpoint must be allowed to finish the visible reply, so a budget that leaves it no
        // room is dropped rather than clamped: a silent clamp would change behaviour invisibly.
        if (m_config.thinkingBudgetTokens > 0 && m_config.thinkingBudgetTokens < m_config.maxTokens)
        {
            payload["thinking"] = {{"type", "enabled"},
                                   {"budget_tokens", m_config.thinkingBudgetTokens}};
        }

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
            // The tool block is where the run-constant part of the prompt ends, so marking its last
            // schema caches system prompt plus schemas, the part that must never be re-prefilled.
            if (m_config.cacheBreakpoints != CacheBreakpointPlacement::None)
            {
                toolsArr.back()["cache_control"] = {{"type", "ephemeral"}};
            }
            payload["tools"] = std::move(toolsArr);
        }

        // A provider only reads back the prompt up to a breakpoint. With just the tool block marked,
        // everything the conversation grew by is re-prefilled every turn, so marking the last content
        // block lets the next turn pay for its new tail only.
        if (m_config.cacheBreakpoints == CacheBreakpointPlacement::CacheAll
            && payload["messages"].is_array() && !payload["messages"].empty())
        {
            nlohmann::json & lastContent = payload["messages"].back()["content"];
            if (lastContent.is_array() && !lastContent.empty())
            {
                lastContent.back()["cache_control"] = {{"type", "ephemeral"}};
            }
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
            resp.errorInfo = classifyErrorText(resp.error, false);
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
                    resp.timing.textChars += block.value("text", std::string{}).size();
                }
                else if (type == "thinking")
                {
                    resp.timing.thinkingChars += block.value("thinking", std::string{}).size();
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
                    resp.timing.toolJsonChars += call.arguments.dump().size();
                    resp.toolCalls.push_back(std::move(call));
                }
            }
        }
        mergeAnthropicUsage(body, resp.usage);
        return resp;
    }

    coro::task<CompletionResponse>
    AnthropicProvider::complete(CompletionRequest const & request,
                                TextDeltaCallback onDelta,
                                io::CancellationToken const & cancellation)
    {
        HttpRequest httpReq;
        httpReq.url = resolveAnthropicUrl(m_config.endpoint);
        httpReq.timeoutSeconds = m_config.timeoutSeconds;
        httpReq.headers.emplace_back("Content-Type: application/json");
        httpReq.headers.emplace_back("anthropic-version: 2023-06-01");
        if (!m_config.apiKey.empty())
        {
            httpReq.headers.emplace_back("x-api-key: " + m_config.apiKey);
        }
        nlohmann::json const payload = buildPayload(request);
        m_requestDump.write(payload);
        httpReq.body = payload.dump();
        CompletionStopwatch watch;

        if (!m_config.stream)
        {
            watch.restart();
            HttpResponse const httpRes = co_await m_transport->postJsonAsync(httpReq, cancellation);
            bool const keyMissing = m_config.apiKey.empty();
            if (httpRes.cancelled || !httpRes.error.empty())
            {
                CompletionResponse cancelledResp{{}, {}, "", false, httpRes.error};
                cancelledResp.errorInfo
                  = classifyErrorText(httpRes.error, keyMissing, httpRes.cancelled, m_config.model);
                co_return cancelledResp;
            }
            if (httpRes.statusCode < 200 || httpRes.statusCode >= 300)
            {
                CompletionResponse httpResp{
                  {}, {}, "", false,
                  "HTTP " + std::to_string(httpRes.statusCode) + ": "
                    + trimProviderBody(httpRes.body)};
                httpResp.errorInfo = classifyHttpError(
                  httpRes.statusCode, httpRes.body, keyMissing, m_config.model);
                co_return httpResp;
            }
            auto parsed = nlohmann::json::parse(httpRes.body, nullptr, false);
            if (parsed.is_discarded())
            {
                co_return CompletionResponse{{}, {}, "", false, "Invalid JSON from Anthropic endpoint"};
            }
            CompletionResponse resp = parseResponseJson(parsed);
            classifyFailure(resp, keyMissing, m_config.model);
            // A non-streaming reply has no per-event phases: everything arrived at once.
            watch.onFinish(resp.timing);
            resp.timing.ttftMs = resp.timing.totalMs;
            resp.timing.firstTokenMs = resp.timing.totalMs;
            resp.timing.firstContentMs = resp.timing.totalMs;
            if (resp.success && onDelta && !resp.text.empty())
            {
                onDelta(resp.text);
            }
            co_return resp;
        }

        CompletionResponse resp;
        std::map<int, PendingAnthropicBlock> blocks;

        watch.restart();
        HttpResponse const httpRes = co_await m_transport->postSseAsync(
          httpReq,
          [&](std::string const & eventName, std::string const & data)
          {
              watch.onEvent(resp.timing);
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
                  resp.errorInfo = classifyErrorText(
                    resp.error, m_config.apiKey.empty(), false, m_config.model);
                  return;
              }
              // Prompt-side totals arrive in `message_start`; cumulative completion totals in `message_delta`.
              mergeAnthropicUsage(type == "message_start" && payload.contains("message")
                                    ? payload["message"]
                                    : payload,
                                  resp.usage);
              if (type == "content_block_start" && payload.contains("content_block"))
              {
                  int const idx = payload.value("index", 0);
                  auto const & cb = payload["content_block"];
                  PendingAnthropicBlock block;
                  block.type = cb.value("type", "");
                  block.id = cb.value("id", "");
                  block.name = cb.value("name", "");
                  if (block.type == "text" || block.type == "tool_use")
                  {
                      watch.onContent(resp.timing);
                  }
                  // Any block, reasoning included, means the server is done prefilling and generating.
                  watch.onToken(resp.timing);
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
                      resp.timing.textChars += piece.size();
                      watch.onContent(resp.timing);
                      if (onDelta && !piece.empty())
                      {
                          onDelta(piece);
                      }
                  }
                  else if (deltaType == "thinking_delta")
                  {
                      resp.timing.thinkingChars += delta.value("thinking", std::string{}).size();
                      watch.onToken(resp.timing);
                  }
                  else if (deltaType == "input_json_delta")
                  {
                      std::string const piece = delta.value("partial_json", "");
                      blocks[idx].inputJson += piece;
                      resp.timing.toolJsonChars += piece.size();
                      watch.onContent(resp.timing);
                  }
              }
              else if (type == "message_delta" && payload.contains("delta"))
              {
                  resp.stopReason = payload["delta"].value("stop_reason", resp.stopReason);
              }
          },
          cancellation);
        watch.onFinish(resp.timing);

        if (httpRes.cancelled || !httpRes.error.empty())
        {
            resp.success = false;
            resp.error = httpRes.error;
            resp.errorInfo
              = classifyErrorText(httpRes.error, m_config.apiKey.empty(), httpRes.cancelled,
                                  m_config.model);
            co_return resp;
        }
        if (httpRes.statusCode != 0 && (httpRes.statusCode < 200 || httpRes.statusCode >= 300))
        {
            resp.success = false;
            resp.error = "HTTP " + std::to_string(httpRes.statusCode) + ": "
                  + trimProviderBody(httpRes.body);
            resp.errorInfo = classifyHttpError(
              httpRes.statusCode, httpRes.body, m_config.apiKey.empty(), m_config.model);
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
