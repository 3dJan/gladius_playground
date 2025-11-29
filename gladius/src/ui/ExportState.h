#pragma once

#include <atomic>
#include <string>

namespace gladius::ui
{
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
        }

        /// @brief Mark that the export operation has ended (completed or cancelled)
        void endExport()
        {
            m_exportInProgress = false;
            m_exportDescription.clear();
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
