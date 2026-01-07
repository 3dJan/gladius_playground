#pragma once

#include <atomic>
#include <string>

namespace gladius::ui
{
    /// @brief Represents the current phase of an export operation
    ///
    /// Used to track and communicate export progress through the UI.
    /// Transitions: Idle -> Exporting -> (Cancelling -> Idle) or (Idle)
    enum class ExportPhase
    {
        Idle,       ///< No export in progress
        Exporting,  ///< Export operation is running normally
        Cancelling  ///< Cancellation requested, waiting for export to abort
    };

    /// @brief Tracks the state of export operations to prevent model modifications during export
    ///
    /// This class provides a simple mechanism to lock out model modifications while
    /// a mesh export operation is in progress. UI components check isExportInProgress()
    /// before allowing any model modifications.
    class ExportState
    {
      public:
        /// @brief Mark that an export operation has begun
        /// @param description Optional description of the export (for UI feedback)
        void beginExport(std::string description = "Mesh export")
        {
            m_exportDescription = std::move(description);
            m_exportInProgress = true;
            m_phase.store(ExportPhase::Exporting, std::memory_order_release);
        }

        /// @brief Mark that the export operation has ended (completed or cancelled)
        void endExport()
        {
            m_exportInProgress = false;
            m_exportDescription.clear();
            m_phase.store(ExportPhase::Idle, std::memory_order_release);
        }

        /// @brief Request cancellation of the current export
        ///
        /// Transitions the phase to Cancelling if currently Exporting.
        /// This is a signal to the UI to show "Cancelling..." feedback.
        void requestCancellation()
        {
            ExportPhase expected = ExportPhase::Exporting;
            m_phase.compare_exchange_strong(expected, ExportPhase::Cancelling, std::memory_order_acq_rel);
        }

        /// @brief Get the current export phase (thread-safe)
        /// @return Current ExportPhase value
        [[nodiscard]] ExportPhase getPhase() const
        {
            return m_phase.load(std::memory_order_acquire);
        }

        /// @brief Check if cancellation has been requested
        /// @return true if phase is Cancelling
        [[nodiscard]] bool isCancelling() const
        {
            return getPhase() == ExportPhase::Cancelling;
        }

        /// @brief Check if an export operation is currently in progress
        /// @return true if export is in progress, false otherwise
        [[nodiscard]] bool isExportInProgress() const
        {
            return m_exportInProgress;
        }

        /// @brief Get the description of the current export operation
        /// @return Description string, empty if no export in progress
        [[nodiscard]] std::string const & getExportDescription() const
        {
            return m_exportDescription;
        }

      private:
        std::atomic<bool> m_exportInProgress{false};
        std::atomic<ExportPhase> m_phase{ExportPhase::Idle};
        std::string m_exportDescription;
    };

    /// @brief RAII guard for export state - ensures endExport is called on scope exit
    class ExportGuard
    {
      public:
        explicit ExportGuard(ExportState & state, std::string description = "Mesh export")
            : m_state(state)
        {
            m_state.beginExport(std::move(description));
        }

        ~ExportGuard()
        {
            m_state.endExport();
        }

        // Non-copyable, non-movable
        ExportGuard(ExportGuard const &) = delete;
        ExportGuard & operator=(ExportGuard const &) = delete;
        ExportGuard(ExportGuard &&) = delete;
        ExportGuard & operator=(ExportGuard &&) = delete;

      private:
        ExportState & m_state;
    };

} // namespace gladius::ui
