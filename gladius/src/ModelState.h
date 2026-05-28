#pragma once

#include <atomic>

namespace gladius
{
    enum class CompilationState
    {
        UpToDate,
        CompilationRequested,
        CompilationInProgress
    };

    class ModelState
    {
      public:
        void signalCompilationRequired()
        {
            m_compilationState.store(CompilationState::CompilationRequested,
                                     std::memory_order_release);
        }

        void signalCompilationFinished()
        {
            auto expected = CompilationState::CompilationInProgress;
            (void) m_compilationState.compare_exchange_strong(expected,
                                                              CompilationState::UpToDate,
                                                              std::memory_order_acq_rel,
                                                              std::memory_order_acquire);
        }

        void signalCompilationStarted()
        {
            m_compilationState.store(CompilationState::CompilationInProgress,
                                     std::memory_order_release);
        }

        [[nodiscard]] bool isCompilationRequired() const
        {
            return m_compilationState.load(std::memory_order_acquire) ==
                   CompilationState::CompilationRequested;
        }

        [[nodiscard]] bool isModelUpToDate() const
        {
            return m_compilationState.load(std::memory_order_acquire) ==
                   CompilationState::UpToDate;
        }

      private:
        std::atomic<CompilationState> m_compilationState{CompilationState::UpToDate};
    };
}
