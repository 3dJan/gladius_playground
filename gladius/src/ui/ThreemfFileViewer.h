#pragma once

#include "../Document.h"
#include "AsyncThumbnailLoader.h"
#include "ThreemfThumbnailExtractor.h"
#include <filesystem>
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
    };

} // namespace gladius::ui
