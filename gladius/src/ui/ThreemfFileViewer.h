#pragma once

#include "../Document.h"
#include "AsyncThumbnailLoader.h"
#include "ThreemfThumbnailExtractor.h"
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <string>

// Forward declare ImVec2 from imgui
struct ImVec2;

namespace gladius::ui
{
    /**
     * @class ThreemfFileViewer
     * @brief Widget that shows 3MF files in a given directory with their thumbnails
     *
     * This is a pure widget that can be embedded in any container. It does not create
     * its own window and should be placed inside another widget or window.
     * Thumbnails are loaded asynchronously using the same infrastructure as the welcome screen.
     */
    class ThreemfFileViewer
    {
      public:
        /**
         * @brief Constructor
         * @param logger The logger to use for events
         */
        explicit ThreemfFileViewer(events::SharedLogger logger);

        /**
         * @brief Destructor
         */
        ~ThreemfFileViewer();

        /**
         * @brief Set the directory to scan for 3MF files
         * @param directory The directory path
         */
        void setDirectory(const std::filesystem::path & directory);

        /**
         * @brief Get the current directory
         * @return The current directory path
         */
        [[nodiscard]] const std::filesystem::path & getDirectory() const
        {
            return m_directory;
        }

        /**
         * @brief Force a refresh of the directory contents
         */
        void refreshDirectory();

        /**
         * @brief Render the 3MF file viewer UI widget
         * @param doc The current document (for loading selected files)
         */
        void render(SharedDocument doc);

        /// @brief Callback signature: receives file path of the entry to delete.
        /// Return true if the entry was successfully handled.
        using DeleteCallback = std::function<bool(std::filesystem::path const &)>;

        /// @brief Set the callback invoked when the user deletes an entry via context menu.
        void setOnDeleteCallback(DeleteCallback callback)
        {
            m_onDelete = std::move(callback);
        }

        /// @brief Callback for restoring an entry (used in bin mode).
        using RestoreCallback = std::function<bool(std::filesystem::path const &)>;

        /// @brief Set the callback invoked when the user restores a bin entry.
        void setOnRestoreCallback(RestoreCallback callback)
        {
            m_onRestore = std::move(callback);
        }

        /// @brief Callback for permanent deletion (used in bin mode).
        using PermanentDeleteCallback = std::function<bool(std::filesystem::path const &)>;

        /// @brief Set the callback invoked when the user permanently deletes a bin entry.
        void setOnPermanentDeleteCallback(PermanentDeleteCallback callback)
        {
            m_onPermanentDelete = std::move(callback);
        }

        /// @brief Predicate to check if an entry is shipped (non-deletable).
        using IsShippedPredicate = std::function<bool(std::filesystem::path const &)>;

        /// @brief Set the predicate used to determine if an entry is shipped.
        void setIsShippedPredicate(IsShippedPredicate predicate)
        {
            m_isShipped = std::move(predicate);
        }

      private:
        /**
         * @brief Scan the directory for 3MF files and queue thumbnails for async loading
         */
        void scanDirectory();

        /**
         * @brief Render a single thumbnail item in the grid
         */
        void renderThumbnailItem(ThreemfThumbnailExtractor::ThumbnailInfo & info,
                                 SharedDocument doc,
                                 float cellWidth,
                                 float cellHeight,
                                 const ImVec2 & itemPos);

        /**
         * @brief Render the file name below a thumbnail, truncating if necessary
         */
        void renderFileName(const std::string & fileName,
                            float cellWidth,
                            const ImVec2 & itemPos);

        std::filesystem::path m_directory; ///< Directory to scan

        /// List of thumbnail infos for 3MF files.
        /// Uses std::list for pointer stability - AsyncThumbnailLoader stores pointers
        /// to ThumbnailInfo objects that must remain valid during async loading.
        std::list<ThreemfThumbnailExtractor::ThumbnailInfo> m_files;

        bool m_needsRefresh = true;     ///< Whether the directory needs to be rescanned
        events::SharedLogger m_logger;  ///< Logger for events
        float m_thumbnailSize = 150.0f; ///< Size of thumbnails in the UI
        int m_columns = 3;             ///< Number of columns in the grid

        std::unique_ptr<ThreemfThumbnailExtractor> m_thumbnailExtractor; ///< Thumbnail extractor
        std::unique_ptr<AsyncThumbnailLoader> m_asyncLoader;             ///< Async loader

        DeleteCallback m_onDelete;                     ///< Delete callback
        RestoreCallback m_onRestore;                   ///< Restore callback (bin mode)
        PermanentDeleteCallback m_onPermanentDelete;   ///< Permanent delete callback (bin mode)
        IsShippedPredicate m_isShipped;                ///< Shipped entry predicate
    };

} // namespace gladius::ui
