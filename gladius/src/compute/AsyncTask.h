#pragma once

#include <cassert>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace gladius::compute
{
    /// Holds the single pending suspension of a task tree. A driver resumes it once its ready predicate holds.
    class AsyncScheduler
    {
      public:
        using ReadyPredicate = std::function<bool()>;

        void suspend(std::coroutine_handle<> handle, ReadyPredicate ready)
        {
            m_handle = handle;
            m_ready = std::move(ready);
        }

        [[nodiscard]] bool hasPending() const
        {
            return static_cast<bool>(m_handle);
        }

        [[nodiscard]] bool isReady() const
        {
            return m_handle && (!m_ready || m_ready());
        }

        void resumePending()
        {
            auto handle = std::exchange(m_handle, {});
            m_ready = nullptr;
            handle.resume();
        }

      private:
        std::coroutine_handle<> m_handle{};
        ReadyPredicate m_ready{};
    };

    /// Awaitable that suspends the enclosing task until `ready()` returns true. Resumed by the driver on a later step.
    class WaitUntil
    {
      public:
        explicit WaitUntil(std::function<bool()> ready)
            : m_ready(std::move(ready))
        {
        }

        [[nodiscard]] bool await_ready() const
        {
            return !m_ready || m_ready();
        }

        template <typename Promise>
        void await_suspend(std::coroutine_handle<Promise> handle)
        {
            handle.promise().scheduler().suspend(handle, m_ready);
        }

        void await_resume() const noexcept
        {
        }

      private:
        std::function<bool()> m_ready;
    };

    /// Awaitable that always yields control to the driver for one step.
    class AsyncYield
    {
      public:
        [[nodiscard]] bool await_ready() const noexcept
        {
            return false;
        }

        template <typename Promise>
        void await_suspend(std::coroutine_handle<Promise> handle)
        {
            handle.promise().scheduler().suspend(handle, nullptr);
        }

        void await_resume() const noexcept
        {
        }
    };

    namespace detail
    {
        template <typename T>
        struct AsyncTaskResult
        {
            std::optional<T> value;

            void return_value(T result)
            {
                value.emplace(std::move(result));
            }
        };

        template <>
        struct AsyncTaskResult<void>
        {
            void return_void() const noexcept
            {
            }
        };
    } // namespace detail

    /// Lazily started coroutine. Executed in steps by a driver (see step() and runBlocking()).
    /// Awaiting an AsyncTask runs the child on the same scheduler, so nested suspensions are driven from the root.
    template <typename T>
    class AsyncTask
    {
      public:
        struct FinalAwaiter
        {
            [[nodiscard]] bool await_ready() const noexcept
            {
                return false;
            }

            template <typename Promise>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) const noexcept
            {
                auto continuation = handle.promise().continuation();
                return continuation ? continuation : std::noop_coroutine();
            }

            void await_resume() const noexcept
            {
            }
        };

        class promise_type : public detail::AsyncTaskResult<T>
        {
          public:
            promise_type()
                : m_scheduler(std::make_shared<AsyncScheduler>())
            {
            }

            AsyncTask get_return_object()
            {
                return AsyncTask{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            [[nodiscard]] std::suspend_always initial_suspend() const noexcept
            {
                return {};
            }

            [[nodiscard]] FinalAwaiter final_suspend() const noexcept
            {
                return {};
            }

            void unhandled_exception() noexcept
            {
                m_exception = std::current_exception();
            }

            [[nodiscard]] AsyncScheduler& scheduler() const
            {
                return *m_scheduler;
            }

            void shareScheduler(std::shared_ptr<AsyncScheduler> scheduler, std::coroutine_handle<> parent)
            {
                m_scheduler = std::move(scheduler);
                m_continuation = parent;
            }

            [[nodiscard]] std::coroutine_handle<> continuation() const noexcept
            {
                return m_continuation;
            }

            [[nodiscard]] std::shared_ptr<AsyncScheduler> const& sharedScheduler() const noexcept
            {
                return m_scheduler;
            }

            std::exception_ptr m_exception{};

          private:
            std::shared_ptr<AsyncScheduler> m_scheduler;
            std::coroutine_handle<> m_continuation{};
        };

        using handle_type = std::coroutine_handle<promise_type>;

        explicit AsyncTask(handle_type handle) noexcept
            : m_handle(handle)
        {
        }

        AsyncTask(AsyncTask&& other) noexcept
            : m_handle(std::exchange(other.m_handle, {}))
            , m_started(std::exchange(other.m_started, false))
        {
        }

        AsyncTask& operator=(AsyncTask&& other) noexcept
        {
            if (this != &other)
            {
                destroy();
                m_handle = std::exchange(other.m_handle, {});
                m_started = std::exchange(other.m_started, false);
            }
            return *this;
        }

        AsyncTask(AsyncTask const&) = delete;
        AsyncTask& operator=(AsyncTask const&) = delete;

        ~AsyncTask()
        {
            destroy();
        }

        /// True when the coroutine has completed or holds no coroutine.
        [[nodiscard]] bool done() const noexcept
        {
            return !m_handle || m_handle.done();
        }

        /// Advances the task by one step. Resumes the pending suspension if it is ready, or starts the task.
        void step()
        {
            if (done())
            {
                return;
            }

            auto& scheduler = m_handle.promise().scheduler();
            if (scheduler.hasPending())
            {
                if (scheduler.isReady())
                {
                    scheduler.resumePending();
                }
                return;
            }

            assert(!m_started && "Task suspended without a pending resume condition");
            m_started = true;
            m_handle.resume();
        }

        /// Returns the result after completion and rethrows the stored exception, if any.
        T take()
        {
            assert(done() && "take() requires a completed task");
            auto& promise = m_handle.promise();
            if (promise.m_exception)
            {
                std::rethrow_exception(promise.m_exception);
            }
            if constexpr (!std::is_void_v<T>)
            {
                return std::move(*promise.value);
            }
        }

        // Awaiting a child task: the child runs on the parent's scheduler and resumes the parent when finished.
        [[nodiscard]] bool await_ready() const noexcept
        {
            return done();
        }

        template <typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> parent)
        {
            m_started = true;
            m_handle.promise().shareScheduler(parent.promise().sharedScheduler(), parent);
            return m_handle;
        }

        T await_resume()
        {
            return take();
        }

      private:
        void destroy() noexcept
        {
            if (m_handle)
            {
                m_handle.destroy();
                m_handle = {};
            }
        }

        handle_type m_handle{};
        bool m_started{false};
    };

    /// Drives a task to completion. `pump` runs between steps and must advance external work (e.g. GPU callbacks).
    template <typename T, typename Pump>
    T runBlocking(AsyncTask<T> task, Pump&& pump)
    {
        while (!task.done())
        {
            task.step();
            if (!task.done())
            {
                pump();
            }
        }
        return task.take();
    }
} // namespace gladius::compute
