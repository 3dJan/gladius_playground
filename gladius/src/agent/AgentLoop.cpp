#include "AgentLoop.h"
#include "providers/ImageProvider.h"

namespace gladius::agent
{
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
                                              SnapshotProvider snapshotProvider)
    {
        AgentRunResult result;
        auto const startTime = std::chrono::steady_clock::now();

        while (result.modelTurns < limits.maxModelTurns)
        {
            if (cancellation.isCancelled())
            {
                result.cancelled = true;
                result.error = "Agent run cancelled";
                eventQueue.push(AgentFailedEvent{result.error});
                co_return result;
            }

            if (std::chrono::steady_clock::now() - startTime > limits.maxWallTime)
            {
                result.error = "Agent wall-time limit reached";
                eventQueue.push(AgentFailedEvent{result.error});
                co_return result;
            }

            DesignContextSnapshot const snapshot =
              snapshotProvider ? snapshotProvider() : DesignContextSnapshot{};
            CompletionRequest const req =
              contextBuilder.build(conversation, registry, policy.privacy(), snapshot);

            ++result.modelTurns;
            CompletionResponse const response = co_await provider.complete(
              req,
              [&](std::string const & delta) { eventQueue.push(TextDeltaEvent{delta}); },
              cancellation);

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
                result.completed = true;
                eventQueue.push(AgentCompletedEvent{result.finalMessage});
                co_return result;
            }

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

                nlohmann::json toolResult;
                auto const toolPtr = registry.getTool(call.name);
                if (toolPtr)
                {
                    tools::ToolDefinition const def = toolPtr->definition();
                    tools::PolicyDecision const pd = policy.evaluateTool(def, call.arguments);
                    if (!pd.allowed)
                    {
                        toolResult = {{"success", false}, {"error", pd.reason}};
                    }
                    else if (policy.requiresConfirmation(def, call.arguments))
                    {
                        eventQueue.push(UserDecisionRequiredEvent{
                            call.id, "Confirm tool execution: " + def.name, def, call.arguments});
                        bool const approved = confirmCallback ? confirmCallback(def, call.arguments) : false;
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

                if (isDesignComplete && isDesignComplete())
                {
                    result.completed = true;
                    eventQueue.push(AgentCompletedEvent{result.finalMessage});
                    co_return result;
                }
            }
        }

        result.error = "Maximum model turn limit reached";
        eventQueue.push(AgentFailedEvent{result.error});
        co_return result;
    }
} // namespace gladius::agent
