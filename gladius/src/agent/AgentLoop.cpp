#include "AgentLoop.h"
#include "providers/ImageProvider.h"
#include "trace/AgentTrace.h"

#include <algorithm>
#include <cctype>

namespace gladius::agent
{
    namespace
    {
        /**
         * @brief True when a text-only reply reads as a question to the user.
         * @details A reply with no tool call is only the end of a run because the model wants
         *          something from the user. Statements that merely close out the turn are not
         *          questions, and the panel should not claim the agent is waiting for those.
         */
        [[nodiscard]] bool isRequestForUserInput(std::string const & text)
        {
            if (text.find('?') != std::string::npos)
            {
                return true;
            }
            std::string lower;
            lower.reserve(text.size());
            for (char c : text)
            {
                lower.push_back(
                  static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            static constexpr char const * kWaitPhrases[] = {"please confirm",
                                                            "please tell me",
                                                            "let me know",
                                                            "would you like",
                                                            "do you want",
                                                            "should i",
                                                            "which option",
                                                            "waiting for your"};
            for (auto const * phrase : kWaitPhrases)
            {
                if (lower.find(phrase) != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }

        /// @brief Marks conversation text the user injected while the run was already going
        constexpr char const * kSteeringMarker =
          "[USER STEERING MESSAGE - the user changed course mid-run. Re-plan against this before "
          "the next tool call.] ";

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

        /// @brief True when the model ran out of output tokens on this turn
        [[nodiscard]] bool isTruncatedStop(std::string const & stopReason)
        {
            return stopReason == "max_tokens" || stopReason == "length";
        }

        /**
         * @brief True when the provider answered successfully but produced nothing usable.
         * @details Hidden reasoning is not something the loop can act on, and it is never echoed
         *          back to the model, so a reply that carries only thinking is as unusable as an
         *          empty one. Local servers produce exactly this when the output budget is spent on
         *          reasoning before the first tool call is written; treating such a turn as "the
         *          agent is done" silently ends the run without any geometry.
         */
        [[nodiscard]] bool isEmptyCompletion(CompletionResponse const & response)
        {
            return response.toolCalls.empty() && response.text.empty();
        }

        /// @brief Guidance appended after a turn the model cut off at its output limit
        /// @details A cut-off answer drops the tail of the last tool call's arguments, which surfaces
        ///          as a confusing "argument is required" failure. Tell the model to split its work.
        constexpr char const * kTruncationGuidance =
          "Your previous answer hit the model output limit, so the last tool call may have arrived "
          "incomplete. Keep single tool calls short: emit a program snippet one helper function at a "
          "time (root function last) instead of the whole program, and avoid repeating large "
          "snippets you already sent.";

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
                                              trace::AgentTraceRecorder * traceRecorder,
                                              AgentRunControl * runControl,
                                              SecondaryLimitProbe secondaryLimitProbe,
                                              SteeringQueue * steering)
    {
        AgentRunResult result;
        // Failures reach the UI in both forms: the raw text for diagnostics and a classified
        // message the panel can put in the status line.
        auto pushFailure = [&](std::string const & error, bool cancelled = false) {
            eventQueue.push(
              AgentFailedEvent{error, classifyErrorText(error, false, cancelled)});
        };
        // Steering messages that never reached the model must come back to the host, otherwise a
        // message typed during the last turn of a run would be silently dropped.
        auto drainLeftoverSteers = [&]() {
            if (steering != nullptr)
            {
                result.leftoverSteers = steering->drain();
            }
        };
        // Segment budgets: a limit that fires parks the run instead of failing it, and "Continue
        // starts a fresh segment while keeping the cumulative statistics in `result`.
        int turnCap = limits.maxModelTurns;
        std::size_t toolCap = limits.maxToolCalls;
        std::size_t turnSegmentBase = 0;
        std::size_t toolSegmentBase = 0;
        auto segmentStartTime = std::chrono::steady_clock::now();
        std::chrono::milliseconds wallCap = limits.maxWallTime;
        auto turnBudgetAvailable = [&]() {
            return turnCap <= 0
                   || result.modelTurns - turnSegmentBase < static_cast<std::size_t>(turnCap);
        };
        // Applies the user's choice after a parked limit: unlimited from now on, or a new segment.
        auto applyContinue = [&]() {
            if (runControl != nullptr && runControl->consumeContinueUnlimited())
            {
                turnCap = 0;
                toolCap = 0;
                wallCap = std::chrono::milliseconds::zero();
            }
            turnSegmentBase = result.modelTurns;
            toolSegmentBase = result.toolCalls;
            segmentStartTime = std::chrono::steady_clock::now();
        };
        // A limit with an interactive run parks for a continue decision; without one, e.g. in the
        // orchestrator or bench, it fails the run exactly as before. Returns true to end the run.
        auto hitLimit = [&](AgentLimitKind kind,
                            std::string error,
                            std::size_t used) -> coro::task<bool> {
            result.error = std::move(error);
            if (runControl == nullptr)
            {
                pushFailure(result.error);
                co_return true;
            }
            eventQueue.push(AgentLimitReachedEvent{kind, result.error, used});
            runControl->requestPause();
            co_await runControl->waitForResume(cancellation);
            applyContinue();
            if (cancellation.isCancelled())
            {
                result.cancelled = true;
                result.error = "Agent run cancelled";
                pushFailure(result.error, true);
                co_return true;
            }
            // Continuing is not a failure: the limit text only stays in the event.
            result.error.clear();
            co_return false;
        };
        std::size_t autonomousNudges = 0;
        constexpr std::size_t kMaxAutonomousNudges = 3;
        // An empty completion is a provider fault, not an agent decision, so it gets a couple of
        // retries before the run is reported as failed instead of three nudges and a fake success.
        std::size_t emptyCompletionRetries = 0;
        constexpr std::size_t kMaxEmptyCompletionRetries = 2;

        // Prompt-cache latch: `isIncremental` and `autonomy` are the only snapshot fields that reach
        // the system prompt, and both are derived from state that changes during a run (conversation
        // size, the autonomy selector). Re-evaluating them mid-run rewrites the system block and
        // re-prefills the whole conversation, so the first turn's values hold for the rest of the run.
        bool havePromptLatch = false;
        bool latchIncremental = false;
        AutonomyLevel latchAutonomy = AutonomyLevel::AskAtMajorDecisions;

        // Completion is scoped to this run: a design that was already complete when the run started
        // (e.g. finished by an earlier prompt) must not terminate a run for a follow-up request.
        bool const initiallyComplete = isDesignComplete ? isDesignComplete() : false;
        auto becameDesignComplete = [&]() {
            return isDesignComplete != nullptr && !initiallyComplete && isDesignComplete();
        };
        // Marks a turn that only happened because the previous turn's tool calls failed, which is how
        // avoidable cost is attributed to a harness problem instead of to the model.
        bool previousTurnHadFailure = false;

        while (true)
        {
            if (!turnBudgetAvailable())
            {
                if (co_await hitLimit(AgentLimitKind::ModelTurns,
                                      "Maximum model turn limit reached",
                                      result.modelTurns - turnSegmentBase))
                {
                    drainLeftoverSteers();
                    co_return result;
                }
            }

            // Pause is cooperative: it takes effect between turns and between tool calls, never
            // in the middle of a tool that is already mutating the document.
            if (runControl != nullptr)
            {
                co_await runControl->waitForResume(cancellation);
            }

            if (cancellation.isCancelled())
            {
                result.cancelled = true;
                result.error = "Agent run cancelled";
                pushFailure(result.error, true);
                drainLeftoverSteers();
                co_return result;
            }

            if (wallCap.count() > 0
                && std::chrono::steady_clock::now() - segmentStartTime > wallCap)
            {
                auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - segmentStartTime);
                if (co_await hitLimit(AgentLimitKind::WallTime, "Agent wall-time limit reached",
                                      static_cast<std::size_t>(elapsed.count())))
                {
                    drainLeftoverSteers();
                    co_return result;
                }
            }

            if (secondaryLimitProbe)
            {
                if (auto const hit = secondaryLimitProbe(); hit.has_value())
                {
                    if (co_await hitLimit(hit->first,
                                          std::string("Maximum ") + agentLimitLabel(hit->first)
                                            + " limit reached",
                                          hit->second))
                    {
                        drainLeftoverSteers();
                        co_return result;
                    }
                }
            }

            // Steering enters the conversation here: at a turn boundary, after the previous turn's
            // tool_use/tool_result pairs are complete and before the request is built. Injecting a
            // message anywhere else would either split a tool exchange, which the endpoints reject,
            // or rewrite bytes the provider has already cached.
            if (steering != nullptr)
            {
                auto steers = steering->drain();
                if (!steers.empty())
                {
                    std::size_t textBytes = 0;
                    std::size_t images = 0;
                    for (auto & steer : steers)
                    {
                        textBytes += steer.text.size();
                        images += steer.images.size();
                        conversation.appendUserText(kSteeringMarker + std::move(steer.text));
                        for (auto & image : steer.images)
                        {
                            conversation.appendUserImage(std::move(image.data),
                                                         std::move(image.mimeType));
                        }
                    }
                    result.steersApplied += steers.size();
                    if (traceRecorder != nullptr)
                    {
                        traceRecorder->recordNote("steering_applied",
                                                  {{"turnIndex", result.modelTurns + 1},
                                                   {"count", steers.size()},
                                                   {"textBytes", textBytes},
                                                   {"images", images}});
                    }
                }
            }

            DesignContextSnapshot snapshot =
              snapshotProvider ? snapshotProvider() : DesignContextSnapshot{};
            if (havePromptLatch)
            {
                snapshot.isIncremental = latchIncremental;
                snapshot.autonomy = latchAutonomy;
            }
            else
            {
                latchIncremental = snapshot.isIncremental;
                latchAutonomy = snapshot.autonomy;
                havePromptLatch = true;
            }
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
                pushFailure(result.error, true);
                drainLeftoverSteers();
                co_return result;
            }

            if (!response.success)
            {
                result.error = response.error.empty() ? "Provider completion failed" : response.error;
                eventQueue.push(AgentFailedEvent{
                  result.error,
                  response.errorInfo.isSet()
                    ? response.errorInfo
                    : classifyErrorText(result.error, false, false, response.providerName)});
                drainLeftoverSteers();
                co_return result;
            }

            // Nothing to act on: retry a bounded number of times, then report a failure. Ending the
            // run as "completed" here would leave the user with an empty document and no message.
            if (isEmptyCompletion(response))
            {
                ++result.emptyTurns;
                bool const canRetry = emptyCompletionRetries < kMaxEmptyCompletionRetries
                                      && !becameDesignComplete()
                                      && turnBudgetAvailable();
                if (canRetry)
                {
                    ++emptyCompletionRetries;
                    if (traceRecorder != nullptr)
                    {
                        traceRecorder->recordNote("empty_completion_retry",
                                                  {{"turnIndex", result.modelTurns},
                                                   {"retry", emptyCompletionRetries},
                                                   {"stopReason", response.stopReason}});
                    }
                    conversation.appendUserText(
                      "Your last reply was empty and contained no tool call. Continue now: issue the "
                      "next tool calls as a single tool_use response, or call finish_design once the "
                      "design is validated.");
                    continue;
                }
                result.error = isTruncatedStop(response.stopReason)
                                 ? "Model returned an empty answer at its output limit"
                                 : "Model returned an empty answer";
                eventQueue.push(AgentFailedEvent{
                  result.error,
                  describeEmptyCompletion(response.stopReason, response.providerName)});
                drainLeftoverSteers();
                co_return result;
            }
            emptyCompletionRetries = 0;

            conversation.appendAssistant(response.text, response.toolCalls);
            result.finalMessage = response.text;

            if (response.toolCalls.empty())
            {
                // In autonomous mode a question is never a stopping point: either the model asked
                // something or the design is still unfinished, and both mean "decide and continue".
                bool const asksSomething = isRequestForUserInput(result.finalMessage);
                bool const designPending = isDesignComplete != nullptr && !initiallyComplete;
                if (policy.autonomy() == AutonomyLevel::WorkAutonomously
                    && !becameDesignComplete() && autonomousNudges < kMaxAutonomousNudges
                    && (asksSomething || designPending))
                {
                    ++autonomousNudges;
                    if (traceRecorder != nullptr)
                    {
                        traceRecorder->recordNote("autonomous_nudge",
                                                  {{"turnIndex", result.modelTurns},
                                                   {"nudge", autonomousNudges}});
                    }
                    conversation.appendUserText(std::string("The user is away in autonomous mode. "
                                                            "Do not wait for user input or ask "
                                                            "permission. Decide autonomously, execute "
                                                            "the next tools, and call finish_design "
                                                            "when validated.")
                                                + (truncated ? std::string(kTruncationGuidance)
                                                             : std::string()));
                    continue;
                }
                result.completed = true;
                // A text-only answer that asks something is the agent waiting on the user: the run
                // is over and nothing moves until they answer, so the UI has to show the question.
                bool const awaitingUserReply =
                  asksSomething && policy.autonomy() != AutonomyLevel::WorkAutonomously
                  && !becameDesignComplete();
                if (awaitingUserReply && traceRecorder != nullptr)
                {
                    traceRecorder->recordNote("awaiting_user_reply",
                                              {{"turnIndex", result.modelTurns}});
                }
                eventQueue.push(AgentCompletedEvent{result.finalMessage, awaitingUserReply});
                drainLeftoverSteers();
                co_return result;
            }
            autonomousNudges = 0;
            bool turnHadFailure = false;

            for (auto const & call : response.toolCalls)
            {
                if (runControl != nullptr)
                {
                    co_await runControl->waitForResume(cancellation);
                }
                if (cancellation.isCancelled())
                {
                    result.cancelled = true;
                    result.error = "Agent run cancelled";
                    pushFailure(result.error, true);
                    drainLeftoverSteers();
                    co_return result;
                }

                if (toolCap != 0 && result.toolCalls - toolSegmentBase >= toolCap)
                {
                    if (co_await hitLimit(AgentLimitKind::ToolCalls,
                                          "Maximum tool-call limit reached",
                                          result.toolCalls - toolSegmentBase))
                    {
                        drainLeftoverSteers();
                        co_return result;
                    }
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
                    drainLeftoverSteers();
                    co_return result;
                }
            }

            if (truncated)
            {
                if (traceRecorder != nullptr)
                {
                    traceRecorder->recordNote("truncation_nudge", {{"turnIndex", result.modelTurns}});
                }
                conversation.appendUserText(kTruncationGuidance);
            }
            previousTurnHadFailure = turnHadFailure;
        }
    }
} // namespace gladius::agent
