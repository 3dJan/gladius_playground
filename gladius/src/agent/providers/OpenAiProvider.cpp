#include "OpenAiProvider.h"

#include "CompletionStopwatch.h"
#include "ProviderError.h"

#include <map>

namespace gladius::agent
{
    namespace
    {
        struct PendingToolCall
        {
            std::string id;
            std::string name;
            std::string argumentsJson;
        };

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

        std::string resolveOpenAiUrl(std::string endpoint)
        {
            if (endpoint.empty())
            {
                return "https://api.openai.com/v1/chat/completions";
            }
            while (!endpoint.empty() && endpoint.back() == '/')
            {
                endpoint.pop_back();
            }
            if (endpoint.size() >= 17 && endpoint.substr(endpoint.size() - 17) == "/chat/completions")
            {
                return endpoint;
            }
            return endpoint + "/chat/completions";
        }

        /// @brief Merge an OpenAI-style `usage` object into `usage`, ignoring absent fields.
        void mergeOpenAiUsage(nlohmann::json const & body, TokenUsage & usage)
        {
            if (!body.contains("usage") || !body["usage"].is_object())
            {
                return;
            }
            auto const & u = body["usage"];
            auto const readInto = [](nlohmann::json const & src,
                                     char const * key,
                                     std::int64_t & target)
            {
                if (src.contains(key) && src[key].is_number())
                {
                    target = src[key].get<std::int64_t>();
                }
            };
            readInto(u, "prompt_tokens", usage.inputTokens);
            readInto(u, "completion_tokens", usage.outputTokens);
            if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object())
            {
                readInto(u["prompt_tokens_details"], "cached_tokens", usage.cacheReadTokens);
            }
        }
    } // namespace

    OpenAiProvider::OpenAiProvider(ProviderConfig config,
                                   std::shared_ptr<IHttpTransport> transport)
        : m_config(std::move(config))
        , m_transport(transport ? std::move(transport) : std::make_shared<CurlHttpTransport>())
    {
    }

    nlohmann::json OpenAiProvider::buildPayload(CompletionRequest const & request) const
    {
        nlohmann::json messages = nlohmann::json::array();

        if (!request.systemPrompt.empty())
        {
            messages.push_back({{"role", "system"}, {"content", request.systemPrompt}});
        }

        for (auto const & msg : request.messages)
        {
            if (msg.role == MessageRole::Tool)
            {
                for (auto const & part : msg.parts)
                {
                    if (auto const * res = std::get_if<ToolResult>(&part))
                    {
                        messages.push_back({
                            {"role", "tool"},
                            {"tool_call_id", res->callId},
                            {"content", res->value.is_string() ? res->value.get<std::string>() : res->value.dump()}
                        });
                    }
                }
                continue;
            }

            if (msg.role == MessageRole::Assistant)
            {
                std::string text;
                nlohmann::json toolCalls = nlohmann::json::array();
                for (auto const & part : msg.parts)
                {
                    if (auto const * t = std::get_if<TextContent>(&part))
                    {
                        text += t->text;
                    }
                    else if (auto const * tc = std::get_if<ToolCall>(&part))
                    {
                        toolCalls.push_back({
                            {"id", tc->id},
                            {"type", "function"},
                            {"function", {{"name", tc->name}, {"arguments", tc->arguments.dump()}}}
                        });
                    }
                }
                nlohmann::json entry = {{"role", "assistant"}};
                entry["content"] = text.empty() && !toolCalls.empty() ? nlohmann::json(nullptr) : nlohmann::json(text);
                if (!toolCalls.empty())
                {
                    entry["tool_calls"] = std::move(toolCalls);
                }
                messages.push_back(std::move(entry));
                continue;
            }

            std::string const roleStr = (msg.role == MessageRole::System) ? "system" : "user";
            bool hasImages = false;
            for (auto const & part : msg.parts)
            {
                if (std::holds_alternative<ImageContent>(part))
                {
                    hasImages = true;
                    break;
                }
            }

            if (!hasImages)
            {
                std::string text;
                for (auto const & part : msg.parts)
                {
                    if (auto const * t = std::get_if<TextContent>(&part))
                    {
                        text += t->text;
                    }
                }
                messages.push_back({{"role", roleStr}, {"content", text}});
            }
            else
            {
                nlohmann::json contentArr = nlohmann::json::array();
                for (auto const & part : msg.parts)
                {
                    if (auto const * t = std::get_if<TextContent>(&part))
                    {
                        contentArr.push_back({{"type", "text"}, {"text", t->text}});
                    }
                    else if (auto const * img = std::get_if<ImageContent>(&part))
                    {
                        std::string const url = "data:" + img->mimeType + ";base64," + encodeBase64(img->data);
                        contentArr.push_back({
                            {"type", "image_url"},
                            {"image_url", {{"url", url}}}
                        });
                    }
                }
                messages.push_back({{"role", roleStr}, {"content", std::move(contentArr)}});
            }
        }

        nlohmann::json payload = {
            {"model", m_config.model},
            {"messages", std::move(messages)},
            {"max_tokens", m_config.maxTokens},
            {"temperature", m_config.temperature},
            {"stream", m_config.stream}
        };

        if (m_config.stream)
        {
            // Without this most OpenAI-compatible servers omit `usage` from streamed responses.
            payload["stream_options"] = {{"include_usage", true}};
        }

        if (!request.tools.empty())
        {
            nlohmann::json toolsArr = nlohmann::json::array();
            for (auto const & tool : request.tools)
            {
                toolsArr.push_back({
                    {"type", "function"},
                    {"function", {
                        {"name", tool.name},
                        {"description", tool.description},
                        {"parameters", tool.inputSchema.is_object() ? tool.inputSchema : nlohmann::json{{"type", "object"}}}
                    }}
                });
            }
            payload["tools"] = std::move(toolsArr);
        }

        return payload;
    }

    CompletionResponse OpenAiProvider::parseResponseJson(nlohmann::json const & body)
    {
        CompletionResponse resp;
        if (body.contains("error") && body["error"].is_object())
        {
            resp.success = false;
            resp.error = body["error"].value("message", body["error"].dump());
            resp.errorInfo = classifyErrorText(resp.error, false);
            return resp;
        }
        if (!body.contains("choices") || !body["choices"].is_array() || body["choices"].empty())
        {
            resp.success = false;
            resp.error = "Missing choices in OpenAI response";
            resp.errorInfo = classifyErrorText(resp.error, false);
            return resp;
        }

        auto const & choice = body["choices"][0];
        resp.stopReason = choice.value("finish_reason", "");
        if (choice.contains("message") && choice["message"].is_object())
        {
            auto const & msg = choice["message"];
            if (msg.contains("content") && msg["content"].is_string())
            {
                resp.text = msg["content"].get<std::string>();
                resp.timing.textChars += resp.text.size();
            }
            if (msg.contains("tool_calls") && msg["tool_calls"].is_array())
            {
                for (auto const & tc : msg["tool_calls"])
                {
                    ToolCall call;
                    call.id = tc.value("id", "");
                    if (tc.contains("function") && tc["function"].is_object())
                    {
                        call.name = tc["function"].value("name", "");
                        std::string const argsStr = tc["function"].value("arguments", "{}");
                        resp.timing.toolJsonChars += argsStr.size();
                        auto parsed = nlohmann::json::parse(argsStr, nullptr, false);
                        call.arguments = parsed.is_discarded() ? nlohmann::json::object() : parsed;
                    }
                    resp.toolCalls.push_back(std::move(call));
                }
            }
        }
        mergeOpenAiUsage(body, resp.usage);
        return resp;
    }

    coro::task<CompletionResponse>
    OpenAiProvider::complete(CompletionRequest const & request,
                             TextDeltaCallback onDelta,
                             io::CancellationToken const & cancellation)
    {
        HttpRequest httpReq;
        httpReq.url = resolveOpenAiUrl(m_config.endpoint);
        httpReq.timeoutSeconds = m_config.timeoutSeconds;
        httpReq.headers.emplace_back("Content-Type: application/json");
        if (!m_config.apiKey.empty())
        {
            httpReq.headers.emplace_back("Authorization: Bearer " + m_config.apiKey);
        }
        if (m_config.endpoint.find("openrouter.ai") != std::string::npos)
        {
            httpReq.headers.emplace_back("HTTP-Referer: https://github.com/3MFConsortium/gladius");
            httpReq.headers.emplace_back("X-Title: Gladius Design Agent");
        }
        for (auto const & h : m_config.extraHeaders)
        {
            if (!h.empty())
            {
                httpReq.headers.emplace_back(h);
            }
        }
        httpReq.body = buildPayload(request).dump();
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
                co_return CompletionResponse{{}, {}, "", false, "Invalid JSON from OpenAI endpoint"};
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
        std::map<int, PendingToolCall> pendingCalls;

        watch.restart();
        HttpResponse const httpRes = co_await m_transport->postSseAsync(
          httpReq,
          [&](std::string const & /*eventName*/, std::string const & data)
          {
              if (data == "[DONE]")
              {
                  return;
              }
              watch.onEvent(resp.timing);
              auto chunk = nlohmann::json::parse(data, nullptr, false);
              if (chunk.is_discarded())
              {
                  return;
              }
              if (chunk.contains("error") && chunk["error"].is_object())
              {
                  resp.success = false;
                  resp.error = chunk["error"].value("message", chunk["error"].dump());
                  resp.errorInfo = classifyErrorText(
                    resp.error, m_config.apiKey.empty(), false, m_config.model);
                  return;
              }
              // The final usage chunk carries an empty `choices` array, so read it before the early return.
              mergeOpenAiUsage(chunk, resp.usage);
              if (!chunk.contains("choices") || !chunk["choices"].is_array() || chunk["choices"].empty())
              {
                  return;
              }
              auto const & choice = chunk["choices"][0];
              if (choice.contains("finish_reason") && choice["finish_reason"].is_string())
              {
                  resp.stopReason = choice["finish_reason"].get<std::string>();
              }
              if (!choice.contains("delta") || !choice["delta"].is_object())
              {
                  return;
              }
              auto const & delta = choice["delta"];
              if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string())
              {
                  // Local reasoning servers expose the chain of thought on this field.
                  resp.timing.thinkingChars += delta["reasoning_content"].get<std::string>().size();
                  watch.onToken(resp.timing);
              }
              if (delta.contains("content") && delta["content"].is_string())
              {
                  std::string const piece = delta["content"].get<std::string>();
                  resp.text += piece;
                  resp.timing.textChars += piece.size();
                  watch.onContent(resp.timing);
                  if (onDelta && !piece.empty())
                  {
                      onDelta(piece);
                  }
              }
              if (delta.contains("tool_calls") && delta["tool_calls"].is_array())
              {
                  for (auto const & tcDelta : delta["tool_calls"])
                  {
                      int const idx = tcDelta.value("index", 0);
                      auto & entry = pendingCalls[idx];
                      if (tcDelta.contains("id") && tcDelta["id"].is_string())
                      {
                          entry.id = tcDelta["id"].get<std::string>();
                      }
                      if (tcDelta.contains("function") && tcDelta["function"].is_object())
                      {
                          auto const & fn = tcDelta["function"];
                          if (fn.contains("name") && fn["name"].is_string())
                          {
                              entry.name += fn["name"].get<std::string>();
                          }
                          if (fn.contains("arguments") && fn["arguments"].is_string())
                          {
                              std::string const piece = fn["arguments"].get<std::string>();
                              entry.argumentsJson += piece;
                              resp.timing.toolJsonChars += piece.size();
                              watch.onContent(resp.timing);
                          }
                      }
                  }
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

        for (auto & [idx, pending] : pendingCalls)
        {
            ToolCall call;
            call.id = std::move(pending.id);
            call.name = std::move(pending.name);
            if (!pending.argumentsJson.empty())
            {
                auto parsedArgs = nlohmann::json::parse(pending.argumentsJson, nullptr, false);
                call.arguments = parsedArgs.is_discarded() ? nlohmann::json::object() : parsedArgs;
            }
            resp.toolCalls.push_back(std::move(call));
        }

        co_return resp;
    }
} // namespace gladius::agent
