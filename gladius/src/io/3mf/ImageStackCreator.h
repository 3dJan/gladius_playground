#pragma once

#include <filesystem>
#include <functional>
#include <lib3mf_implicit.hpp>
#include <string>
#include <vector>

namespace gladius::io
{
    using Files = std::vector<std::filesystem::path>;

    /// T047: Progress callback signature - called with (current, total, filename)
    using ImportProgressCallback =
        std::function<void(size_t current, size_t total, std::string const & filename)>;

    /// T051: Result of ImageStack import operation
    struct ImportResult
    {
        Lib3MF::PImageStack imageStack;
        Lib3MF::PFunctionFromImage3D function;
        std::vector<std::string> paddedFiles;  ///< Files that were padded to match max dimensions
        unsigned int maxWidth = 0;
        unsigned int maxHeight = 0;

        bool hasPaddedFiles() const
        {
            return !paddedFiles.empty();
        }
    };

    class ImageStackCreator
    {
      public:
        Lib3MF::PFunctionFromImage3D
        importDirectoryAsFunctionFromImage3D(Lib3MF::PModel model,
                                             std::filesystem::path const & path);

        /// T048: Import with progress callback and padding support
        ImportResult importDirectoryWithPadding(Lib3MF::PModel model,
                                                std::filesystem::path const & path,
                                                ImportProgressCallback progressCb = nullptr);

        Lib3MF::PImageStack addImageStackFromDirectory(Lib3MF::PModel model,
                                                       std::filesystem::path const & path);
        Files getFiles(std::filesystem::path const & path) const;

      private:
        unsigned m_rows = 0;
        unsigned m_cols = 0;
        unsigned m_numSheets = 0;

        void determineImageStackSize(Files const & files);

        /// T049: Detect maximum dimensions across all images
        void determineMaxDimensions(Files const & files,
                                    unsigned int & maxWidth,
                                    unsigned int & maxHeight);
    };
}