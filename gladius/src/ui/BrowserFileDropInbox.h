#pragma once

#include <filesystem>
#include <optional>
#include <set>

namespace gladius::ui
{
    /**
     * @brief Discovers browser-dropped 3MF files staged in the virtual filesystem.
     *
     * The browser shell writes a completed file into the inbox with an atomic rename.
     * This class keeps discovery and claim state independent from the UI lifecycle so
     * files dropped while WebGPU is initializing can wait safely.
     */
    class BrowserFileDropInbox
    {
      public:
        explicit BrowserFileDropInbox(std::filesystem::path directory);

        /**
         * @brief Ensure that the inbox directory exists and is usable.
         * @return true when the directory exists and is a directory.
         */
        [[nodiscard]] bool ensureDirectory() const;

        /**
         * @brief Claim the next completed 3MF file in deterministic order.
         * @return An unclaimed file path, or std::nullopt when none is available.
         */
        [[nodiscard]] std::optional<std::filesystem::path> claimNext();

        /**
         * @brief Check whether a path belongs to this browser-session inbox.
         * @param filePath Path to inspect.
         * @return true when filePath is below the inbox directory.
         */
        [[nodiscard]] bool isManagedPath(std::filesystem::path const & filePath) const noexcept;

        /**
         * @brief Return the inbox directory.
         */
        [[nodiscard]] std::filesystem::path const & directory() const noexcept
        {
            return m_directory;
        }

      private:
        [[nodiscard]] bool isCompleted3mfFile(std::filesystem::directory_entry const & entry) const;

        std::filesystem::path m_directory;
        std::set<std::filesystem::path> m_claimedPaths;
    };
}
