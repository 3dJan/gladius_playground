#include "AgentLoop.h"
#include "providers/ImageProvider.h"
#include "trace/AgentTrace.h"

#include <algorithm>

namespace gladius::agent
{
    namespace
    {
        /// @brief Keeps trace error strings short so one failure cannot bloat every later line
        [[nodiscard]] std::string trimForTrace(std::string const & text)
        {
            constexpr std::size_t kMaxTraceErrorChars = 240;
            if (text.size() <= kMaxTraceErrorChars)
            {
                return text;
            }
            return text.substr(0, kMaxTraceErrorChars) + "...";
        }

        /**
         * @brief Assembles the `turn` trace record for one model completion.
         * @param turnStartMs mark of the moment the context build began, taken from the recorder
         */
        [[nodiscard]] nlohmann::json buildTurnRecord(trace::AgentTraceRecorder & recorder,
                                                    std::size_t turnIndex,
                                                    std::int64_t turnStartMs,
                                                    std::int64_t buildMs,
                                                    CompletionRequest const & request,
                                                    CompletionResponse const & response,
                                                    ContextBuildStats const & contextStats)
        {
            auto const usage = response.usage;
            auto const timing = response.timing;
            nlohmann::json record = {
              {"turnIndex", turnIndex},
              {"startedMarkMs", turnStartMs},
              {"durationMs", recorder.mark() - turnStartMs},
              {"buildMs", buildMs},
              {"completionMs", timing.totalMs},
              {"ttftMs", timing.ttftMs},
              {"firstTokenMs", timing.firstTokenMs},
              {"firstContentMs", timing.firstContentMs},
              {"streamEvents", timing.streamEvents},
              {"thinkingChars", timing.thinkingChars},
              {"textChars", timing.textChars},
              {"toolJsonChars", timing.toolJsonChars},
              {"stopReason", response.stopReason},
              {"success", response.success},
              {"toolCallsIssued", response.toolCalls.size()},
              {"systemPromptHash", trace::systemPromptHash(request.systemPrompt)},
              {"context",
               {{"imagesKept", contextStats.imagesKept},
                {"messagesSent", contextStats.messagesSent},
                {"messagesPruned", contextStats.messagesPruned}}},
              {"promptBytes", trace::breakdownRequest(request).toJson()},
              {"usage",
               {{"inputTokens", usage.inputTokens},
                {"outputTokens", usage.outputTokens},
                {"cacheReadTokens", usage.cacheReadTokens},
                {"cacheWriteTokens", usage.cacheWriteTokens},
                {"totalPromptTokens", usage.totalPromptTokens()}}}};
            if (!response.error.empty())
            {
                record["error"] = response.error;
            }
            if (recorder.promptDumpEnabled())
            {
                nlohmann::json messages = nlohmann::json::array();
                for (auto const & msg : request.messages)
                {
                    nlohmann::json parts = nlohmann::json::array();
                    for (auto const & part : msg.parts)
                    {
                        if (auto const * text = std::get_if<TextContent>(&part))
                        {
                            parts.push_back({{"type", "text"}, {"text", text->text}});
                        }
                        else if (auto const * image = std::get_if<ImageContent>(&part))
                        {
                            parts.push_back({{"type", "image"},
                                             {"mimeType", image->mimeType},
                                             {"bytes", image->data.size()}});
                        }
                        else if (auto const * call = std::get_if<ToolCall>(&part))
                        {
                            parts.push_back({{"type", "tool_call"},
                                             {"name", call->name},
                                             {"arguments", call->arguments}});
                        }
                        else if (auto const * result = std::get_if<ToolResult>(&part))
                        {
                            parts.push_back({{"type", "tool_result"}, {"value", result->value}});
                        }
                    }
                    messages.push_back({{"role", static_cast<int>(msg.role)},
                                        {"parts", std::move(parts)}});
                }
                record["request"] = {{"systemPrompt", request.systemPrompt},
                                     {"messages", std::move(messages)}};
            }
            return record;
        }
    } // namespace
    coro::task<AgentRunResult> AgentLoop::run(Conversation & conversation,
                                              IAiProvider & provider,
                                              tools::ToolRegistry const & registry,
                                              tools::ToolExecutor const & executor,
                                              ContextBuilder const & contextBuilder,
                                              PermissionPolicy const & policy,
                                              AgentEventQueue & eventQueue,
                                              AgentLimits const & limits,
                                              io::CancellationToken const & cancellation,
                                              CompletionPredicate isDesignComplete,
                                              ConfirmationCallback confirmCallback,
                                              SnapshotProvider snapshotProvider,
                                              trace::AgentTraceRecorder * traceRecorder)
    {
        AgentRunResult result;
        auto const startTime = std::chrono::steady_clock::now();
        std::size_t autonomousNudges = 0;
        constexpr std::size_t kMaxAutonomousNudges = 3;

        // Completion is scoped to this run: a design that was already complete when the run started
        // (e.g. finished by an earlier prompt) must not terminate a run for a follow-up request.
        bool const initiallyComplete = isDesignComplete ? isDesignComplete() : false;
        auto becameDesignComplete = [&]() {
            return isDesignComplete != nullptr && !initiallyComplete && isDesignComplete();
        };
        // Marks a turn that only happened because the previous turn's tool calls failed, which is how
        // avoidable cost is attributed to a harness problem instead of to the model.
        bool previousTurnHadFailure = false;

        while (limits.maxModelTurns <= 0
               || result.modelTurns < static_cast<std::size_t>(limits.maxModelTurns))
        {
            if (cancellation.isCancelled())
            {
                result.cancelled = true;
                result.error = "Agent run cancelled";
                eventQueue.push(AgentFailedEvent{result.error});
                co_return result;
            }

            if (limits.maxWallTime.count() > 0
                && std::chrono::steady_clock::now() - startTime > limits.maxWallTime)
            {
                result.error = "Agent wall-time limit reached";
                eventQueue.push(AgentFailedEvent{result.error});
                co_return result;
            }

            DesignContextSnapshot const snapshot =
              snapshotProvider ? snapshotProvider() : DesignContextSnapshot{};
            std::int64_t const buildStartMs = traceRecorder ? traceRecorder->mark() : 0;
            ContextBuildStats contextStats;
            CompletionRequest const req =
              contextBuilder.build(conversation, registry, policy.privacy(), snapshot, &contextStats);
            std::int64_t const buildMs = traceRecorder ? traceRecorder->mark() - buildStartMs : 0;

            ++result.modelTurns;
            std::int64_t const turnStartMs = traceRecorder ? traceRecorder->mark() - buildMs : 0;
            if (traceRecorder != nullptr && previousTurnHadFailure)
            {
                traceRecorder->recordNote("retry_after_failure", {{"turnIndex", result.modelTurns}});
            }
            previousTurnHadFailure = false;
            CompletionResponse const response = co_await provider.complete(
              req,
              [&](std::string const & delta) { eventQueue.push(TextDeltaEvent{delta}); },
              cancellation);
            result.usage.accumulate(response.usage);
            if (!response.providerName.empty())
            {
                result.providerName = response.providerName;
            }

            bool const truncated =
              response.stopReason == "max_tokens" || response.stopReason == "length";
            if (truncated)
            {
                ++result.truncatedTurns;
            }

            if (traceRecorder != nullptr)
            {
                traceRecorder->recordTurn(buildTurnRecord(*traceRecorder,
                                                          result.modelTurns,
                                                          turnStartMs,
                                                          buildMs,
                                                          req,
                                                          response,
                                                          contextStats));
            }

            if (cancellation.isCancelled())
            {
                result.cancelled = true;
                result.error = "Agent run cancelled";
                eventQueue.push(AgentFailedEvent{result.error});
                co_return result;
            }

            if (!response.success)
            {
                result.error = response.error.empty() ? "Provider completion failed" : response.error;
                eventQueue.push(AgentFailedEvent{result.error});
                co_return result;
            }

            conversation.appendAssistant(response.text, response.toolCalls);
            result.finalMessage = response.text;

            if (response.toolCalls.empty())
            {
                if (policy.autonomy() == AutonomyLevel::WorkAutonomously && isDesignComplete
                    && !becameDesignComplete() && autonomousNudges < kMaxAutonomousNudges)
                {
                    ++autonomousNudges;
                    if (traceRecorder != nullptr)
                    {
                        traceRecorder->recordNote("autonomous_nudge",
                                                  {{"turnIndex", result.modelTurns},
                                                   {"nudge", autonomousNudges}});
                    }
                    conversation.appendUserText(
                      "The user is away in autonomous mode. Do not wait for user input or ask "
                      "permission. Decide autonomously, execute the next tools, and call "
                      "finish_design when validated.");
                    continue;
                }
                result.completed = true;
                eventQueue.push(AgentCompletedEvent{result.finalMessage});
                co_return result;
            }
            autonomousNudges = 0;
            bool turnHadFailure = false;

            for (auto const & call : response.toolCalls)
            {
                if (result.toolCalls >= limits.maxToolCalls)
                {
                    result.error = "Maximum tool-call limit reached";
                    eventQueue.push(AgentFailedEvent{result.error});
                    co_return result;
                }

                ++result.toolCalls;
                eventQueue.push(ToolCallStartedEvent{call.id, call.name, call.arguments});

                std::int64_t const toolStartMs = traceRecorder ? traceRecorder->mark() : 0;
                nlohmann::json toolResult;
                char const * policyName = "unknown";
                bool denied = false;
                auto const toolPtr = registry.getTool(call.name);
                if (toolPtr)
                {
                    tools::ToolDefinition const def = toolPtr->definition();
                    policyName = tools::toString(def.policy);
                    tools::PolicyDecision const pd = policy.evaluateTool(def, call.arguments);
                    if (!pd.allowed)
                    {
                        denied = true;
                        toolResult = {{"success", false}, {"error", pd.reason}};
                    }
                    else if (policy.requiresConfirmation(def, call.arguments))
                    {
                        eventQueue.push(UserDecisionRequiredEvent{
                            call.id, "Confirm tool execution: " + def.name, def, call.arguments});
                        bool const approved =
                          confirmCallback ? co_await confirmCallback(def, call.arguments) : false;
                        if (!approved)
                        {
                            toolResult = {{"success", false}, {"error", "User declined tool execution"}};
                        }
                        else
                        {
                            toolResult = co_await executor.execute(call.name, call.arguments, cancellation);
                        }
                    }
                    else
                    {
                        toolResult = co_await executor.execute(call.name, call.arguments, cancellation);
                    }
                }
                else
                {
                    toolResult = co_await executor.execute(call.name, call.arguments, cancellation);
                }

                bool const ok = toolResult.is_object() ? toolResult.value("success", true) : true;
                if (traceRecorder != nullptr)
                {
                    nlohmann::json record = {
                      {"turnIndex", result.modelTurns},
                      {"startedMarkMs", toolStartMs},
                      {"durationMs", traceRecorder->mark() - toolStartMs},
                      {"callId", call.id},
                      {"name", call.name},
                      {"policy", policyName},
                      {"success", ok},
                      {"denied", denied},
                      {"argsBytes", call.arguments.dump().size()},
                      {"resultBytes", toolResult.dump().size()}};
                    if (!ok && toolResult.is_object() && toolResult.contains("error")
                        && toolResult["error"].is_string())
                    {
                        record["error"] = trimForTrace(toolResult["error"].get<std::string>());
                    }
                    traceRecorder->recordToolCall(std::move(record));
                }
                if (!ok)
                {
                    turnHadFailure = true;
                }
                eventQueue.push(ToolCallCompletedEvent{call.id, call.name, toolResult, ok});

                std::optional<ImageContent> attachedImage;
                if (ok && toolResult.is_object() && toolResult.contains("base64")
                    && toolResult["base64"].is_string())
                {
                    std::string const b64 = toolResult["base64"].get<std::string>();
                    std::string const mime = toolResult.value("mimeType", "image/png");
                    auto bytes = decodeBase64(b64);
                    if (!bytes.empty())
                    {
                        attachedImage = ImageContent{std::move(bytes), mime};
                        toolResult.erase("base64");
                        toolResult["imageAttached"] = true;
                    }
                }

                conversation.appendToolResult(call.id, toolResult);
                if (attachedImage.has_value())
                {
                    eventQueue.push(ImageGeneratedEvent{*attachedImage, call.name});
                    conversation.appendUserImage(std::move(attachedImage->data),
                                                 std::move(attachedImage->mimeType));
                }

                if (becameDesignComplete())
                {
                    result.completed = true;
                    eventQueue.push(AgentCompletedEvent{result.finalMessage});
                    co_return result;
                }
            }

            if (truncated)
            {
                if (traceRecorder != nullptr)
                {
                    traceRecorder->recordNote("truncation_nudge", {{"turnIndex", result.modelTurns}});
                }
                // A cut-off answer drops the tail of the last tool call's arguments, which surfaces
                // as a confusing "argument is required" failure. Tell the model to split its work up.
                conversation.appendUserText(
                  "Your previous answer hit the model output limit, so the last tool call may have "
                  "arrived incomplete. Keep single tool calls short: emit a program snippet one "
                  "helper function at a time (root function last) instead of the whole program, and "
                  "avoid repeating large snippets you already sent.");
            }
            previousTurnHadFailure = turnHadFailure;
        }

        result.error = "Maximum model turn limit reached";
        eventQueue.push(AgentFailedEvent{result.error});
        co_return result;
    }
} // namespace gladius::agent
