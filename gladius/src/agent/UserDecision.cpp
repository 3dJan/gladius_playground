#include "UserDecision.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace gladius::agent
{
    namespace
    {
        /// @brief Case-insensitive substring test used to match short script entries
        bool containsIgnoreCase(std::string const & haystack, std::string const & needle)
        {
            if (needle.empty())
            {
                return true;
            }
            auto const lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
            return std::search(haystack.begin(),
                               haystack.end(),
                               needle.begin(),
                               needle.end(),
                               [&](char a, char b) { return lower(a) == lower(b); }) != haystack.end();
        }
    } // namespace

    AskUserCallback makeAskUserCallback(UserDecisionHandoff & handoff, DecisionPolicy policy)
    {
        if (policy == DecisionPolicy::DisableQuestions)
        {
            return [](UserQuestion const &, io::CancellationToken const &) -> coro::task<UserDecisionResult>
            {
                UserDecisionResult result;
                result.error = "This host does not accept questions; decide yourself";
                co_return result;
            };
        }

        if (policy == DecisionPolicy::UseRecommendation)
        {
            return [](UserQuestion const & question,
                      io::CancellationToken const &) -> coro::task<UserDecisionResult>
            {
                UserDecisionResult result;
                if (question.recommended >= 0 &&
                    static_cast<std::size_t>(question.recommended) < question.options.size())
                {
                    result.answer.choice = question.options[static_cast<std::size_t>(question.recommended)];
                }
                result.answer.assumed = true;
                co_return result;
            };
        }

        return [&handoff](UserQuestion const & question,
                          io::CancellationToken const & cancellation) -> coro::task<UserDecisionResult>
        {
            UserDecisionAwaitable awaitable{question};
            handoff.begin(question);
            // `wait()` resets the awaitable, so attach only afterwards: an answer that arrives in
            // between is staged by the handoff and delivered on attach.
            auto waiter = awaitable.wait(cancellation);
            handoff.attach(&awaitable);
            UserDecisionResult result = co_await waiter;
            handoff.end();
            co_return result;
        };
    }

    AskUserCallback makeScriptedAskUserCallback(std::vector<std::string> script,
                                                std::vector<ScriptedDecision> & log,
                                                bool fallbackToRecommendation)
    {
        return [script = std::move(script), &log, fallbackToRecommendation](
                 UserQuestion const & question,
                 io::CancellationToken const &) -> coro::task<UserDecisionResult>
        {
            ScriptedDecision entry;
            entry.questionId = question.id;
            entry.question = question.question;

            UserDecisionResult result;
            std::string const next =
              log.size() < script.size() ? script[log.size()] : (fallbackToRecommendation ? "" : std::string{});
            if (!next.empty())
            {
                entry.script = next;
                char const * end = nullptr;
                auto const index = std::strtol(next.c_str(), const_cast<char **>(&end), 10);
                bool matched = false;
                if (end != next.c_str() && *end == '\0' && index >= 0 &&
                    static_cast<std::size_t>(index) < question.options.size())
                {
                    result.answer.choice = question.options[static_cast<std::size_t>(index)];
                    matched = true;
                }
                if (!matched)
                {
                    // A unique substring is enough; an ambiguous one refuses rather than guessing.
                    std::vector<std::size_t> hits;
                    for (std::size_t i = 0; i < question.options.size(); ++i)
                    {
                        if (containsIgnoreCase(question.options[i], next))
                        {
                            hits.push_back(i);
                        }
                    }
                    if (hits.size() == 1)
                    {
                        result.answer.choice = question.options[hits[0]];
                        matched = true;
                    }
                }
                if (!matched)
                {
                    entry.refused = true;
                    entry.choice = next;
                    result.error = "Scripted answer '" + next + "' matches no option of question " +
                                   question.id;
                    log.push_back(std::move(entry));
                    co_return result;
                }
            }
            else if (question.recommended >= 0 &&
                     static_cast<std::size_t>(question.recommended) < question.options.size())
            {
                result.answer.choice = question.options[static_cast<std::size_t>(question.recommended)];
            }
            result.answer.assumed = true;
            entry.choice = result.answer.choice;
            entry.assumed = true;
            log.push_back(std::move(entry));
            co_return result;
        };
    }
} // namespace gladius::agent
