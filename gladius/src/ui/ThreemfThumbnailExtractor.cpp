#include "ThreemfThumbnailExtractor.h"

#include <algorithm>
#include <fmt/format.h>
#include <lodepng.h>
#include "io/3mf/Lib3mfLoader.h"
#include "io/3mf/LibraryMetadata.h"

namespace gladius::ui
{
    ThreemfThumbnailExtractor::ThreemfThumbnailExtractor(events::SharedLogger logger)
        : m_logger(std::move(logger))
    {
        try
        {
            m_wrapper = gladius::io::loadLib3mfScoped();
        }
        catch (const std::exception & e)
        {
            if (m_logger)
            {
                m_logger->addEvent({e.what(), events::Severity::Error});
            }
        }
    }

    ThreemfThumbnailExtractor::~ThreemfThumbnailExtractor() = default;

    std::vector<unsigned char>
    ThreemfThumbnailExtractor::extractThumbnail(const std::filesystem::path & filePath)
    {
        std::vector<unsigned char> thumbnailData;

        if (!m_wrapper)
        {
            return thumbnailData;
        }

        try
        {
            auto model = m_wrapper->CreateModel();
            auto reader = model->QueryReader("3mf");

            reader->SetStrictModeActive(false);
            reader->ReadFromFile(filePath.string());

            if (model->HasPackageThumbnailAttachment())
            {
                auto thumbnail = model->GetPackageThumbnailAttachment();
                if (thumbnail)
                {
                    thumbnail->WriteToBuffer(thumbnailData);
                }
            }
        }
        catch (const std::exception & e)
        {
            if (m_logger)
            {
                m_logger->addEvent({fmt::format("Failed to extract thumbnail from {}: {}",
                                                filePath.string(),
                                                e.what()),
                                    events::Severity::Warning});
            }
        }

        return thumbnailData;
    }

    void ThreemfThumbnailExtractor::loadThumbnail(ThumbnailInfo & info)
    {
        if (info.thumbnailLoaded)
        {
            return;
        }

        info.thumbnailData = extractThumbnail(info.filePath);
        info.hasThumbnail = !info.thumbnailData.empty();
        info.thumbnailLoaded = true;

        if (info.hasThumbnail)
        {
            // Decode the PNG data to get width and height
            std::vector<unsigned char> image;
            unsigned int width, height;
            unsigned int error = lodepng::decode(image, width, height, info.thumbnailData);

            if (error == 0)
            {
                info.thumbnailWidth = width;
                info.thumbnailHeight = height;
                createThumbnailTexture(info);
            }
            else
            {
                info.hasThumbnail = false;
                if (m_logger)
                {
                    m_logger->addEvent({fmt::format("Failed to decode thumbnail for {}: {}",
                                                    info.fileName,
                                                    lodepng_error_text(error)),
                                        events::Severity::Warning});
                }
            }
        }
    }

    void ThreemfThumbnailExtractor::createThumbnailTexture(ThumbnailInfo & info)
    {
        if (info.thumbnailTextureId != 0 || !info.hasThumbnail || info.thumbnailData.empty())
        {
            return;
        }

        // Decode the PNG data
        std::vector<unsigned char> decodedImage;
        unsigned int width, height;
        unsigned int error = lodepng::decode(decodedImage, width, height, info.thumbnailData);

        if (error != 0)
        {
            return;
        }

        // Create OpenGL texture
        GLuint textureId;
        glGenTextures(1, &textureId);
        glBindTexture(GL_TEXTURE_2D, textureId);

        // Set texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Upload the image data to the texture
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA,
                     width,
                     height,
                     0,
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     decodedImage.data());

        // Unbind the texture
        glBindTexture(GL_TEXTURE_2D, 0);

        // Store the texture ID
        info.thumbnailTextureId = textureId;
    }

    void ThreemfThumbnailExtractor::releaseThumbnail(ThumbnailInfo & info)
    {
        if (info.thumbnailTextureId != 0)
        {
            glDeleteTextures(1, &info.thumbnailTextureId);
            info.thumbnailTextureId = 0;
        }

        info.thumbnailData.clear();
        info.hasThumbnail = false;
        info.thumbnailLoaded = false;
    }

    ThreemfThumbnailExtractor::ThumbnailInfo
    ThreemfThumbnailExtractor::createThumbnailInfo(const std::filesystem::path & filePath,
                                                   std::time_t timestamp)
    {
        ThumbnailInfo info;
        info.filePath = filePath;
        info.fileName = filePath.stem().string();
        info.timestamp = timestamp;

        // Note: File size and metadata extraction are deferred to async loading
        // to avoid blocking the UI during startup. The async thumbnail loader
        // will populate fileInfo.fileSize and metadata when the thumbnail is loaded.

        return info;
    }

    ThumbnailLoadResult
    ThreemfThumbnailExtractor::extractThumbnailDataOnly(std::filesystem::path const & filePath)
    {
        ThumbnailLoadResult result;

        try
        {
            // Get file size (quick filesystem operation)
            if (std::filesystem::exists(filePath))
            {
                result.fileSize = std::filesystem::file_size(filePath);
            }

            // Create a new lib3mf wrapper for thread safety
            auto wrapper = gladius::io::loadLib3mfScoped();
            if (!wrapper)
            {
                result.errorMessage = "Failed to initialize lib3mf";
                return result;
            }

            auto model = wrapper->CreateModel();
            auto reader = model->QueryReader("3mf");

            reader->SetStrictModeActive(false);
            reader->ReadFromFile(filePath.string());

            // Extract metadata from the model
            try
            {
                auto metaDataGroup = model->GetMetaDataGroup();
                if (metaDataGroup)
                {
                    Lib3MF_uint32 const entryCount = metaDataGroup->GetMetaDataCount();
                    for (Lib3MF_uint32 i = 0; i < entryCount; i++)
                    {
                        try
                        {
                            auto metaData = metaDataGroup->GetMetaData(i);
                            if (metaData)
                            {
                                std::string key = metaData->GetName();
                                std::string value = metaData->GetValue();
                                std::string nameSpace = metaData->GetNameSpace();
                                if (!nameSpace.empty())
                                {
                                    key = nameSpace + ":" + key;
                                }
                                if (!value.empty())
                                {
                                    result.metadata.emplace_back(key, value);
                                }
                            }
                        }
                        catch (...)
                        {
                            // Skip this metadata entry if there's an error
                        }
                    }
                }
            }
            catch (std::exception const &)
            {
                // Metadata extraction is optional - continue without metadata
            }
            catch (...)
            {
                // Unknown error during metadata extraction - continue without metadata
            }

            if (model->HasPackageThumbnailAttachment())
            {
                auto thumbnail = model->GetPackageThumbnailAttachment();
                if (thumbnail)
                {
                    std::vector<unsigned char> pngData;
                    thumbnail->WriteToBuffer(pngData);

                    if (!pngData.empty())
                    {
                        // Decode PNG to RGBA pixels
                        result.decodedPixels =
                          decodePngToPixels(pngData, result.width, result.height);

                        if (!result.decodedPixels.empty())
                        {
                            result.success = true;
                        }
                        else
                        {
                            result.errorMessage = "Failed to decode PNG thumbnail";
                        }
                    }
                }
            }
            else
            {
                // No thumbnail in file - not an error, just no thumbnail
                result.success = false;
                result.errorMessage = "No thumbnail in 3MF file";
            }

            // Extract library metadata (function names + description)
            try
            {
                auto libMeta = io::readLibraryMetadata(model);
                if (libMeta)
                {
                    result.hasLibraryMetadata = true;
                    result.description = libMeta->libraryDescription;

                    auto const taggedIds = io::parseResourceIds(libMeta->libraryFunctions);
                    auto funcIter = model->GetResources();
                    while (funcIter->MoveNext())
                    {
                        auto res = funcIter->GetCurrent();
                        auto implicitFunc =
                          std::dynamic_pointer_cast<Lib3MF::CImplicitFunction>(res);
                        if (!implicitFunc)
                        {
                            continue;
                        }
                        auto const modelId = res->GetModelResourceID();
                        if (std::find(taggedIds.begin(), taggedIds.end(), modelId) !=
                            taggedIds.end())
                        {
                            result.libraryFunctionNames.push_back(
                              implicitFunc->GetDisplayName());
                        }
                    }
                }
            }
            catch (...)
            {
                // Library metadata extraction is optional - continue without it
            }
        }
        catch (const std::exception & e)
        {
            result.success = false;
            result.errorMessage = e.what();
        }

        return result;
    }

    std::vector<unsigned char>
    ThreemfThumbnailExtractor::decodePngToPixels(const std::vector<unsigned char> & pngData,
                                                  unsigned int & outWidth,
                                                  unsigned int & outHeight)
    {
        std::vector<unsigned char> decodedImage;
        outWidth = 0;
        outHeight = 0;

        if (pngData.empty())
        {
            return decodedImage;
        }

        unsigned int error = lodepng::decode(decodedImage, outWidth, outHeight, pngData);

        if (error != 0)
        {
            decodedImage.clear();
            outWidth = 0;
            outHeight = 0;
        }

        return decodedImage;
    }

    void ThreemfThumbnailExtractor::createTextureFromPixels(ThumbnailInfo & info)
    {
        if (info.thumbnailTextureId != 0 || info.decodedPixels.empty())
        {
            return;
        }

        // Create OpenGL texture
        GLuint textureId;
        glGenTextures(1, &textureId);
        glBindTexture(GL_TEXTURE_2D, textureId);

        // Set texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Upload the image data to the texture
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA,
                     info.thumbnailWidth,
                     info.thumbnailHeight,
                     0,
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     info.decodedPixels.data());

        // Unbind the texture
        glBindTexture(GL_TEXTURE_2D, 0);

        // Store the texture ID and update state
        info.thumbnailTextureId = textureId;
        info.textureCreated = true;
        info.hasThumbnail = true;
        info.loadState = ThumbnailLoadState::Ready;

        // Clear decoded pixels to free memory
        info.decodedPixels.clear();
        info.decodedPixels.shrink_to_fit();
    }
}
