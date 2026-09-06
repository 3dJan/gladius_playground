#pragma once

#include "../Document.h"
#include "ThreemfFileViewer.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gladius::webgpu
{
#if defined(GLADIUS_UI_BACKEND_WEBGPU)
  class WebGPUComputeContext;
#endif
}

namespace gladius::ui
{
    /**
     * @class LibraryBrowser
     * @brief Shows a tabbed library browser with 3MF files from multiple directories
     *
     * The LibraryBrowser shows each subfolder of the selected root directory as a tab,
     * with each tab showing the 3MF files in that directory using a ThreemfFileViewer.
     */
    class LibraryBrowser
    {
      public:
        LibraryBrowser() = default;
        /**
         * @brief Constructor
         * @param logger The logger to use for events
         */
        explicit LibraryBrowser(events::SharedLogger logger);

        void setLogger(events::SharedLogger logger);

      #if defined(GLADIUS_UI_BACKEND_WEBGPU)
        /// @brief Sets the Dawn context used by all library thumbnail viewers.
        void setWebGPUContext(std::shared_ptr<webgpu::WebGPUComputeContext> context);
      #endif

        /**
         * @brief Destructor
         */
        ~LibraryBrowser();

        /**
         * @brief Set the root directory containing subfolders to display
         * @param directory The root directory path
         */
        void setRootDirectory(const std::filesystem::path & directory);

        /**
         * @brief Show or hide the browser
         * @param visible Whether the browser should be visible
         */
        void setVisibility(bool visible);

        /**
         * @brief Check if the browser is visible
         * @return Whether the browser is visible
         */
        [[nodiscard]] bool isVisible() const
        {
            return m_visible;
        }

        /**
         * @brief Render the library browser UI
         * @param doc The current document (for loading selected files)
         */
        void render(SharedDocument doc);

        /**
         * @brief Force a refresh of all directories
         */
        void refreshDirectories();

      private:
        /**
         * @brief Scan for subfolders in the root directory
         */
        void scanSubfolders();

        /**
         * @brief Create ThreemfFileViewer instances for each subfolder
         */
        void createFileBrowsers();

        /// @brief Render the bin tab contents.
        void renderBinTab(SharedDocument doc);

        /// @brief Scan the bin directory for category subfolders.
        void scanBinFolder();

        /// @brief Create ThreemfFileViewer instances for bin subfolders.
        void createBinBrowsers();

        std::filesystem::path m_rootDirectory;           ///< Root directory to scan for subfolders
        std::vector<std::filesystem::path> m_subfolders; ///< Found subfolders
        /// Whether the browser is visible.
        /// Default true: callers decide whether to call render() at all,
        /// so an always-hidden default would require extra opt-in calls at every site.
        bool m_visible = true;
        bool m_needsRefresh = true;    ///< Whether the directories need to be rescanned
        events::SharedLogger m_logger; ///< Logger for events
      #if defined(GLADIUS_UI_BACKEND_WEBGPU)
        std::shared_ptr<webgpu::WebGPUComputeContext> m_webgpuContext;
      #endif
        std::unordered_map<std::string, std::unique_ptr<ThreemfFileViewer>>
          m_fileBrowsers; ///< File browsers for each subfolder

        std::vector<std::filesystem::path> m_binSubfolders;  ///< Bin category subfolders
        bool m_binNeedsRefresh = true;                       ///< Whether bin dirs need rescan
        std::unordered_map<std::string, std::unique_ptr<ThreemfFileViewer>>
          m_binBrowsers; ///< File browsers for each bin subfolder
    };
}
