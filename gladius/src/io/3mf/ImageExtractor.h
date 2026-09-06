#pragma once

#include "ImageStack.h"
#if defined(GLADIUS_ENABLE_OPENVDB)
#include "io/vdb.h"
#endif

#include <filesystem>
#include <lodepng.h>
#include <minizip/unzip.h>

#include <string>
#include <vector>

namespace lodepng
{
    LodePNGInfo getPNGHeaderInfo(const std::vector<unsigned char> & png);
}

namespace gladius::io
{

    std::filesystem::path removeLeadingSlash(std::filesystem::path const & path);

    using FileList = std::vector<std::filesystem::path>;
    enum class FileLoaderType
    {
        Archive,
        Filesystem
    };

    class ImageExtractor
    {
      public:
        ImageExtractor() = default;
        ~ImageExtractor();

#if defined(GLADIUS_ENABLE_OPENVDB)
        openvdb::GridBase::Ptr loadAsVdbGrid(FileList const & filenames,
                                             FileLoaderType fileLoaderType) const;
#endif

        bool loadFromArchive(std::filesystem::path const & filename);

        void close();

        std::vector<unsigned char>
        loadFileFromArchive(std::filesystem::path const & filename) const;

        std::vector<unsigned char>
        loadFileFromFilesystem(std::filesystem::path const & filename) const;

        ImageStack loadImageStack(FileList const & filenames,
                                  FileLoaderType fileLoaderType = FileLoaderType::Archive);

        void printAllFiles() const;

        PixelFormat determinePixelFormat(std::filesystem::path const & filename) const;

        /// Determine pixel format from a filesystem file (not archive)
        PixelFormat determinePixelFormatFromFile(std::filesystem::path const & filename) const;

        LodePNGInfo const & getPNGInfo() const;

      private:
        unzFile m_archive = nullptr;
        LodePNGInfo m_pngInfo{};
    };
} // namespace gladius::io