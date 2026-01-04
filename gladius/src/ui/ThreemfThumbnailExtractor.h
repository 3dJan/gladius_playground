#pragma once

#include "../EventLogger.h"
#include "../gpgpu.h" // This includes GL properly with Windows.h first
#include <algorithm>  // Added for std::find_if
#include <cstdint>
#include <filesystem>
#include <lib3mf_implicit.hpp>
#include <memory>
#include <string>
#include <vector>

namespace gladius::ui
{
    /**
     * @brief Enumeration for thumbnail loading states (async loading support)
     */
    enum class ThumbnailLoadState
    {
        NotStarted,     ///< Load not yet initiated
        Loading,        ///< Background load in progress
        DecodedPending, ///< Decoded pixels ready, waiting for texture creation on main thread
        Ready,          ///< Texture created, ready to display
        Failed          ///< Load failed (show placeholder)
    };

    /**
     * @brief Result of a background thumbnail load operation
     */
    struct ThumbnailLoadResult
    {
        bool success = false;                       ///< Whether extraction succeeded
        std::vector<unsigned char> decodedPixels;   ///< Decoded RGBA pixel data
        unsigned int width = 0;                     ///< Image width
        unsigned int height = 0;                    ///< Image height
        std::string errorMessage;                   ///< Error description if failed
    };

    /**
     * @brief Class for extracting and handling thumbnails from 3MF files
     */
    class ThreemfThumbnailExtractor
    {
      public:
        /**
         * @brief Structure to hold 3MF file metadata
         */
        struct ThreemfFileInfo
        {
            struct MetadataItem
            {
                std::string key;   ///< Metadata key
                std::string value; ///< Metadata value
            };

            std::vector<MetadataItem> metadata; ///< Dynamic key-value pairs for metadata
            uintmax_t fileSize = 0;             ///< File size in bytes

            /**
             * @brief Add a metadata item if it has a value
             *
             * @param key The metadata key
             * @param value The metadata value
             */
            void addMetadata(const std::string & key, const std::string & value)
            {
                if (!value.empty())
                {
                    metadata.push_back({key, value});
                }
            }

            /**
             * @brief Get a metadata value by key
             *
             * @param key The metadata key to look for
             * @return std::string The value or empty string if not found
             */
            std::string getMetadata(const std::string & key) const
            {
                auto it =
                  std::find_if(metadata.begin(),
                               metadata.end(),
                               [&key](const MetadataItem & item) { return item.key == key; });

                if (it != metadata.end())
                {
                    return it->value;
                }

                return {};
            }
        };

        /**
         * @brief Structure to hold thumbnail information
         */
        struct ThumbnailInfo
        {
            std::filesystem::path filePath;           ///< Path to the 3MF file
            std::string fileName;                     ///< Name of the file (without extension)
            std::vector<unsigned char> thumbnailData; ///< Raw PNG data
            std::vector<unsigned char> decodedPixels; ///< Decoded RGBA pixels (for async loading)
            bool hasThumbnail = false;                ///< Whether the file has a thumbnail
            bool thumbnailLoaded = false;             ///< Whether the thumbnail has been loaded
            bool textureCreated = false;              ///< Whether the GL texture has been created
            GLuint thumbnailTextureId = 0;            ///< OpenGL texture ID
            unsigned int thumbnailWidth = 0;          ///< Width of the thumbnail
            unsigned int thumbnailHeight = 0;         ///< Height of the thumbnail
            std::time_t timestamp = 0;                ///< Last modified timestamp
            ThreemfFileInfo fileInfo;                 ///< Additional file metadata
            ThumbnailLoadState loadState = ThumbnailLoadState::NotStarted; ///< Current loading state
        };

        /**
         * @brief Construct a new Threemf Thumbnail Extractor
         *
         * @param logger Event logger for error reporting
         */
        explicit ThreemfThumbnailExtractor(events::SharedLogger logger);

        /**
         * @brief Destroy the Threemf Thumbnail Extractor
         */
        ~ThreemfThumbnailExtractor();

        /**
         * @brief Extract thumbnail data from a 3MF file
         *
         * @param filePath Path to the 3MF file
         * @return std::vector<unsigned char> Raw PNG data
         */
        std::vector<unsigned char> extractThumbnail(const std::filesystem::path & filePath);

        /**
         * @brief Load thumbnail data for a file
         *
         * @param info Thumbnail info to be updated
         */
        void loadThumbnail(ThumbnailInfo & info);

        /**
         * @brief Create an OpenGL texture from thumbnail data
         *
         * @param info Thumbnail info containing the PNG data
         */
        void createThumbnailTexture(ThumbnailInfo & info);

        /**
         * @brief Release resources associated with a thumbnail
         *
         * @param info Thumbnail info to clean up
         */
        void releaseThumbnail(ThumbnailInfo & info);

        /**
         * @brief Create a thumbnail info object from a file path
         *
         * @param filePath Path to the 3MF file
         * @param timestamp Optional timestamp (defaults to file's last modified time)
         * @return ThumbnailInfo New thumbnail info object
         */
        ThumbnailInfo createThumbnailInfo(const std::filesystem::path & filePath,
                                          std::time_t timestamp = 0);

        /**
         * @brief Extract thumbnail PNG data without creating a texture (thread-safe)
         *
         * This method creates its own lib3mf wrapper instance, making it safe to call
         * from a background thread. Use this for async thumbnail loading.
         *
         * @param filePath Path to the 3MF file
         * @return ThumbnailLoadResult Result containing decoded pixels or error
         */
        static ThumbnailLoadResult extractThumbnailDataOnly(const std::filesystem::path & filePath);

        /**
         * @brief Decode PNG data to RGBA pixels (thread-safe, static)
         *
         * @param pngData Raw PNG data
         * @param outWidth Output: image width
         * @param outHeight Output: image height
         * @return std::vector<unsigned char> Decoded RGBA pixel data (empty on error)
         */
        static std::vector<unsigned char> decodePngToPixels(const std::vector<unsigned char> & pngData,
                                                            unsigned int & outWidth,
                                                            unsigned int & outHeight);

        /**
         * @brief Create OpenGL texture from pre-decoded RGBA pixels (main thread only)
         *
         * @param info Thumbnail info with decodedPixels populated
         */
        void createTextureFromPixels(ThumbnailInfo & info);

      private:
        events::SharedLogger m_logger; ///< Logger for error reporting
        Lib3MF::PWrapper m_wrapper;    ///< 3MF library wrapper
    };
}
