#pragma once

#include "FileChooser.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <optional>
#include <vector>

namespace gladius::ui
{
    /// @brief Simple async file dialog using std::future.
    ///
    /// Runs the dialog in a background thread and allows polling for results.
    /// On Linux, uses zenity/kdialog subprocess. On Windows, uses COM dialogs.
    ///
    /// ## Usage:
    /// @code
    /// // Member variable in your class:
    /// AsyncFileDialog m_browseDialog;
    ///
    /// // In render loop:
    /// if (!m_browseDialog.isActive())
    /// {
    ///     if (ImGui::Button("Browse..."))
    ///         m_browseDialog.saveFile({"*.stl"}, defaultPath);
    /// }
    /// else
    /// {
    ///     ImGui::BeginDisabled();
    ///     ImGui::Button("Waiting...");
    ///     ImGui::EndDisabled();
    /// }
    ///
    /// if (auto result = m_browseDialog.checkResult())
    /// {
    ///     if (*result) // user selected a file
    ///         m_path = **result;
    /// }
    /// @endcode
    class AsyncFileDialog
    {
      public:
        /// @brief Start a save file dialog asynchronously.
        void saveFile(FilePatterns patterns, std::filesystem::path defaultPath = {});

        /// @brief Start an open file dialog asynchronously.
        void openFile(FilePatterns patterns, std::filesystem::path defaultPath = {});

        /// @brief Start a directory selection dialog asynchronously.
        void selectDirectory(std::filesystem::path defaultPath = {});

        /// @brief Check if a dialog operation is currently in progress.
        [[nodiscard]] bool isActive() const;

        /// @brief Poll for result (non-blocking).
        /// @return nullopt if still waiting, otherwise the dialog result
        ///         (which itself may be nullopt if user cancelled).
        [[nodiscard]] std::optional<QueriedFilename> checkResult();

      private:
        std::future<QueriedFilename> m_future;
        bool m_hasResult{false};
    };

} // namespace gladius::ui
