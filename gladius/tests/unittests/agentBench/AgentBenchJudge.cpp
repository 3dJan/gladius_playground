#include "AgentBenchJudge.h"

#include "agent/Conversation.h"

#include <cctype>
#include <string>
#include <utility>

namespace gladius::agent::bench
{
    namespace
    {
        constexpr char const * const kJudgeSystemPrompt =
          "You are a strict, independent QA judge for CAD models produced by a design agent.\n"
          "You are shown several rendered views of the produced model together with the task and "
          "the acceptance rubric. Judge only what is visible in the images.\n"
          "Faces that point away from the scene light are rendered dark; a dark area is not by "
          "itself evidence of a missing feature.\n"
          "Answer with a single JSON object and nothing else, using exactly this shape:\n"
          "{\"passes\": true|false, \"confidence\": 0.0-1.0, \"observed\": \"<what you see>\", "
          "\"issues\": [\"<each reason the model fails, empty if none>\"]}\n"
          "Set passes to false whenever a rubric requirement is not clearly visible.";

        /// @brief Find the closing brace of the first balanced JSON object in `text`
        [[nodiscard]] std::size_t findObjectEnd(std::string const & text, std::size_t openPos)
        {
            int depth = 0;
            bool inString = false;
            bool escaped = false;
            for (std::size_t i = openPos; i < text.size(); ++i)
            {
                char const c = text[i];
                if (inString)
                {
                    if (escaped)
                    {
                        escaped = false;
                    }
                    else if (c == '\\')
                    {
                        escaped = true;
                    }
                    else if (c == '"')
                    {
                        inString = false;
                    }
                    continue;
                }
                if (c == '"')
                {
                    inString = true;
                }
                else if (c == '{')
                {
                    ++depth;
                }
                else if (c == '}' && --depth == 0)
                {
                    return i;
                }
            }
            return std::string::npos;
        }

        [[nodiscard]] double readConfidence(nlohmann::json const & obj)
        {
            if (!obj.contains("confidence"))
            {
                return 0.0;
            }
            auto const & value = obj["confidence"];
            if (value.is_number())
            {
                return value.get<double>();
            }
            if (value.is_string())
            {
                try
                {
                    return std::stod(value.get<std::string>());
                }
                catch (std::exception const &)
                {
                    return 0.0;
                }
            }
            return 0.0;
        }
    } // namespace

    nlohmann::json JudgeVerdict::toJson() const
    {
        return {{"available", available},
                {"passes", passes},
                {"confidence", confidence},
                {"observed", observed},
                {"issues", issues},
                {"error", error},
                {"usage",
                 {{"inputTokens", usage.inputTokens},
                  {"outputTokens", usage.outputTokens},
                  {"cacheReadTokens", usage.cacheReadTokens},
                  {"cacheWriteTokens", usage.cacheWriteTokens}}}};
    }

    nlohmann::json AgentBenchJudge::extractJson(std::string const & text)
    {
        auto const openPos = text.find('{');
        if (openPos == std::string::npos)
        {
            return nlohmann::json();
        }
        auto const closePos = findObjectEnd(text, openPos);
        if (closePos == std::string::npos)
        {
            return nlohmann::json();
        }
        auto parsed = nlohmann::json::parse(text.substr(openPos, closePos - openPos + 1),
                                           nullptr,
                                           false);
        return parsed.is_discarded() ? nlohmann::json() : parsed;
    }

    JudgeVerdict AgentBenchJudge::judge(AgentBenchTask const & task,
                                        std::vector<std::vector<std::byte>> const & views)
    {
        JudgeVerdict verdict;
        if (!m_provider || views.empty())
        {
            verdict.error = views.empty() ? "No rendered views to judge" : "No judge provider";
            return verdict;
        }

        Conversation conversation;
        std::vector<Content> parts;
        parts.emplace_back(TextContent{
          "Task: " + task.prompt + "\n\nAcceptance rubric: " + task.judgeRubric
          + "\n\nThe following images are rendered views of the produced model."});
        for (auto const & view : views)
        {
            parts.emplace_back(ImageContent{view, "image/png"});
        }
        conversation.append(Message{MessageRole::User, std::move(parts)});

        CompletionRequest request;
        request.systemPrompt = kJudgeSystemPrompt;
        request.messages = conversation.messages();

        std::int64_t const judgeStartMs = m_trace ? m_trace->mark() : 0;
        CompletionResponse const response = coro::sync_wait(
          m_provider->complete(request, nullptr, io::CancellationToken{}));
        if (m_trace)
        {
            m_trace->recordPhase("judge",
                                 judgeStartMs,
                                 {{"views", views.size()},
                                  {"success", response.success},
                                  {"promptTokens", response.usage.totalPromptTokens()},
                                  {"completionTokens", response.usage.outputTokens}});
        }
        verdict.usage = response.usage;
        m_usage.accumulate(response.usage);

        if (!response.success)
        {
            verdict.error = response.error.empty() ? "Judge call failed" : response.error;
            return verdict;
        }

        auto const parsed = extractJson(response.text);
        if (!parsed.is_object() || !parsed.contains("passes"))
        {
            verdict.error = "Judge answer had no usable verdict JSON";
            verdict.observed = response.text;
            return verdict;
        }

        verdict.available = true;
        verdict.passes = parsed["passes"].is_boolean()
                           ? parsed["passes"].get<bool>()
                           : parsed["passes"] != 0;
        verdict.confidence = readConfidence(parsed);
        verdict.observed = parsed.value("observed", std::string{});
        if (parsed.contains("issues") && parsed["issues"].is_array())
        {
            for (auto const & issue : parsed["issues"])
            {
                verdict.issues.push_back(issue.is_string() ? issue.get<std::string>()
                                                           : issue.dump());
            }
        }
        return verdict;
    }
} // namespace gladius::agent::bench
