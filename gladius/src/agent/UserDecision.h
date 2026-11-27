#pragma once

#include "../io/CancellationToken.h"

#include <coro/coro.hpp>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace gladius::agent
{
    /// @brief Upper bound on how often one agent run may interrupt the user
    inline constexpr std::size_t kMaxUserQuestions = 5;

    /**
     * @brief One question the model asks the user in decision mode.
     * @details A question is a *choice between alternatives*, not a confirmation: the answer is
     *          applied to the design graph (variant selection, interface values) and recorded as
     *          evidence on a `user` checklist entry.
     */
    struct UserQuestion
    {
        std::string id;
        std::string question;
        /// @brief Short labels for the offered alternatives; may be empty for an open question
        std::vector<std::string> options;
        /// @brief Index in `options` the agent would pick by itself
        int recommended = -1;
        /// @brief Why the user is being asked at all
        std::string reason;
        /// @brief Design-graph node the decision belongs to, so the UI can show it in context
        std::string nodeId;
    };

    /// @brief What the user (or the host, when working autonomously) decided
    struct UserAnswer
    {
        /// @brief Chosen option label, empty for a free-text answer
        std::string choice;
        /// @brief Free-form text the user typed instead of picking an option
        std::string customText;
        /// @brief True when no user was present and the recommendation was used
        bool assumed = false;
    };

    /**
     * @brief Outcome of asking the user, or the reason the question could not be answered.
     */
    struct UserDecisionResult
    {
        UserAnswer answer;
        /// @brief True when a human answered
        bool answered = false;
        /// @brief Populated when the run was cancelled before an answer arrived
        bool cancelled = false;
        /// @brief Machine-readable cause, for tool errors and metrics
        std::string error;
    };

    /**
     * @brief Resolves a `UserQuestion` and waits until an answer is available.
     * @details The native flavour parks the awaiting coroutine on a condition variable while the
     *          render thread keeps running. The WebAssembly flavour cannot block, so it suspends the
     *          coroutine and resumes it from the outside, exactly like `AgentRunControl`.
     * @note Single-shot per question: `resolve()` is effective once and wakes every waiter.
     */
    class UserDecisionAwaitable
    {
      public:
        explicit UserDecisionAwaitable(UserQuestion question)
            : m_question(std::move(question))
        {
        }

        /// @brief Question currently being awaited (empty before the first `await`)
        [[nodiscard]] UserQuestion const & question() const noexcept
        {
            return m_pending;
        }

        /// @brief Hand over an answer and wake the awaiting coroutine
        void resolve(UserAnswer answer)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_resolved)
                {
                    return;
                }
                m_answer = std::move(answer);
                m_resolved = true;
            }
            m_cv.notify_all();
        }

        /**
         * @brief Abandon the question, waking the waiter with a cancellation error.
         * @details Called when the run is stopped while a question is on screen.
         */
        void cancel(std::string error)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_resolved)
                {
                    return;
                }
                m_error = std::move(error);
                m_resolved = true;
            }
            m_cv.notify_all();
        }

#ifdef __EMSCRIPTEN__
        /**
         * @brief Claim the suspended coroutine once an answer is available.
         * @details WebAssembly cannot block, so the host resumes outside of rendering, exactly like
         *          `AgentRunControl`; resuming inside a render pass would re-enter tool execution.
         */
        std::coroutine_handle<> takeSuspended()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_resolved)
            {
                return nullptr;
            }
            return std::exchange(m_suspended, nullptr);
        }
#endif

        /// @brief Await the answer, returning as soon as cancellation is requested
        struct Awaitable
        {
            UserDecisionAwaitable * owner;
            io::CancellationToken const * cancellation;

            [[nodiscard]] bool await_ready() const
            {
                std::lock_guard<std::mutex> lock(owner->m_mutex);
                return owner->m_resolved || (cancellation != nullptr && cancellation->isCancelled());
            }

            bool await_suspend(std::coroutine_handle<> handle)
            {
#ifdef __EMSCRIPTEN__
                std::unique_lock<std::mutex> lock(owner->m_mutex);
                if (owner->m_resolved || (cancellation != nullptr && cancellation->isCancelled()))
                {
                    return false;
                }
                owner->m_suspended = handle;
                return true;
#else
                // Native hosts run the loop on their own thread, so parking here only blocks that
                // thread; the UI thread answers the question and wakes this one. Returning false
                // resumes this coroutine immediately once the wait finishes.
                std::ignore = handle;
                std::unique_lock<std::mutex> lock(owner->m_mutex);
                owner->m_cv.wait(lock, [&]() {
                    return owner->m_resolved || (cancellation != nullptr && cancellation->isCancelled());
                });
                return false;
#endif
            }

            [[nodiscard]] UserDecisionResult await_resume() const
            {
                std::lock_guard<std::mutex> lock(owner->m_mutex);
                UserDecisionResult result;
                if (owner->m_resolved)
                {
                    result.answer = owner->m_answer;
                    result.answered = !owner->m_answer.assumed;
                    result.error = owner->m_error;
                    result.cancelled = !owner->m_error.empty();
                }
                else if (cancellation != nullptr && cancellation->isCancelled())
                {
                    result.cancelled = true;
                    result.error = "Run cancelled while waiting for the user";
                }
                return result;
            }
        };

        [[nodiscard]] Awaitable wait(io::CancellationToken const & cancellation)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending = m_question;
            m_suspended = nullptr;
            m_answer = {};
            m_error.clear();
            m_resolved = false;
            return Awaitable{this, &cancellation};
        }

        /// @brief True once an answer (or a cancellation) has been recorded
        [[nodiscard]] bool isResolved() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_resolved;
        }

      private:
        UserQuestion m_question;
        UserQuestion m_pending;
        UserAnswer m_answer;
        std::string m_error;
        bool m_resolved = false;
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        /// @brief Parked coroutine on WebAssembly, where the await cannot block the thread
        std::coroutine_handle<> m_suspended = nullptr;
    };

    /// @brief How a host wants to handle a question: ask, pick the recommendation, or refuse
    enum class DecisionPolicy
    {
        /// @brief Wait for the user (single interactive host)
        AskUser,
        /// @brief Answer with the recommended option, marked as assumed (headless, batch, parallel)
        UseRecommendation,
        /// @brief Do not ask at all; `ask_user` reports that the agent must decide itself
        DisableQuestions
    };

    /// @brief Transport for one question: pushes it to a human and awaits the answer
    using AskUserCallback =
      std::function<coro::task<UserDecisionResult>(UserQuestion const &, io::CancellationToken const &)>;

    /**
     * @brief One awaiting question that an interactive host can resolve from its UI thread.
     * @details The panel registers the handover while `ask_user` is executing and resolves it when
     *          the user presses an option button (or when the run is stopped).
     */
    class UserDecisionHandoff
    {
      public:
        /// @brief Publish a question so the UI can show it and answer it later
        void begin(UserQuestion question)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_question = std::move(question);
            m_awaiting = true;
            m_pending = nullptr;
        }

        /**
         * @brief Attach the coroutine that is waiting for the answer.
         * @details An answer that arrived before the attach (fast test hosts) is delivered here.
         */
        void attach(UserDecisionAwaitable * awaitable)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending = awaitable;
            m_resumable = awaitable;
            if (m_stagedAnswer.has_value())
            {
                awaitable->resolve(std::move(*m_stagedAnswer));
                m_stagedAnswer.reset();
                m_awaiting = false;
            }
            else if (!m_stagedError.empty())
            {
                awaitable->cancel(std::move(m_stagedError));
                m_stagedError.clear();
                m_awaiting = false;
            }
        }

        /// @brief Detach a finished question and clear the UI state
        void end()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending = nullptr;
            m_resumable = nullptr;
            m_awaiting = false;
            m_question = {};
        }

        /// @brief Answer with an option label chosen by the user
        void answerOption(std::string choice)
        {
            UserAnswer answer;
            answer.choice = std::move(choice);
            resolveAnswer(std::move(answer));
        }

        /// @brief Answer with free text typed by the user
        void answerText(std::string text)
        {
            UserAnswer answer;
            answer.customText = std::move(text);
            resolveAnswer(std::move(answer));
        }

        /**
         * @brief Decline to answer, leaving the choice to the agent.
         * @details Marks the answer `assumed` so the checklist records it as a working assumption
         *          rather than a human decision.
         */
        void answerAssumed(std::string choice)
        {
            UserAnswer answer;
            answer.choice = std::move(choice);
            answer.assumed = true;
            resolveAnswer(std::move(answer));
        }

#ifdef __EMSCRIPTEN__
        /**
         * @brief Claim the parked `ask_user` coroutine once an answer has arrived.
         * @details WebAssembly cannot block, so the host resumes outside of the render pass;
         *          resuming while tools are being rendered would re-enter tool execution.
         */
        std::coroutine_handle<> takeResumable()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_resumable != nullptr ? m_resumable->takeSuspended() : nullptr;
        }
#endif

        /// @brief Drop the question because the run is being stopped
        void abandon(std::string error)
        {
            UserDecisionAwaitable * pending = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_awaiting = false;
                pending = m_pending;
                if (pending == nullptr)
                {
                    m_stagedError = std::move(error);
                    return;
                }
            }
            pending->cancel(std::move(error));
        }

        /// @brief True while a question is on screen waiting for the user
        [[nodiscard]] bool isAwaiting() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_awaiting;
        }

        /// @brief The pending question, or `std::nullopt` when nothing awaits an answer
        [[nodiscard]] std::optional<UserQuestion> peek() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_awaiting)
            {
                return std::nullopt;
            }
            return m_question;
        }

        /// @brief The question currently awaiting an answer
        [[nodiscard]] UserQuestion question() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_question;
        }

      private:
        void resolveAnswer(UserAnswer answer)
        {
            UserDecisionAwaitable * pending = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_awaiting)
                {
                    return;
                }
                m_awaiting = false;
                pending = m_pending;
                m_pending = nullptr;
                if (pending == nullptr)
                {
                    // Answered before the question reached the awaitable; deliver on attach.
                    m_stagedAnswer = std::move(answer);
                    return;
                }
            }
            pending->resolve(std::move(answer));
        }

        mutable std::mutex m_mutex;
        UserQuestion m_question;
        bool m_awaiting = false;
        UserDecisionAwaitable * m_pending = nullptr;
        /// @brief Question whose coroutine is parked, resumed by the host outside rendering
        UserDecisionAwaitable * m_resumable = nullptr;
        std::optional<UserAnswer> m_stagedAnswer;
        std::string m_stagedError;
    };

    /**
     * @brief Builds the `ask_user` transport for a given decision policy.
     * @param handoff Interactive surface to publish questions on (ignored for non-interactive modes)
     * @param policy How answers are produced
     */
    AskUserCallback makeAskUserCallback(UserDecisionHandoff & handoff, DecisionPolicy policy);

    /// @brief One decision as it was resolved by a non-interactive host
    struct ScriptedDecision
    {
        std::string questionId;
        std::string question;
        /// @brief Script entry that produced the answer, empty when the recommendation was used
        std::string script;
        std::string choice;
        bool assumed = false;
        /// @brief True when neither a scripted entry nor a recommendation could resolve it
        bool refused = false;
    };

    /**
     * @brief Builds the callback used by non-interactive hosts (tests, benchmarks, MCP).
     * @details Decisions have to be deterministic there: entries are consumed in question order
     *          and every outcome is appended to `log`, so a run stays replayable.
     * @param script Answer per question: an option index, or option text (a unique case-insensitive
     *               substring of exactly one option is enough; an ambiguous entry refuses).
     * @param log Every resolved decision of this callback, oldest first.
     * @param fallbackToRecommendation Answer with the recommendation once `script` is exhausted;
     *              false refuses the question instead, which surfaces as a tool failure.
     */
    AskUserCallback makeScriptedAskUserCallback(std::vector<std::string> script,
                                                std::vector<ScriptedDecision> & log,
                                                bool fallbackToRecommendation = true);
} // namespace gladius::agent
