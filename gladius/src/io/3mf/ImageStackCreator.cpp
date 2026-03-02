#include "ImageStackCreator.h"
#include "ImageExtractor.h"
#include <fmt/format.h>
#include <lodepng.h>

namespace gladius::io
{
    Lib3MF::PImageStack
    ImageStackCreator::addImageStackFromDirectory(Lib3MF::PModel model,
                                                  std::filesystem::path const & path)
    {
        auto files = getFiles(path);
        if (files.empty())
        {
            return {};
        }

        determineImageStackSize(files);

        auto stack = model->AddImageStack(m_cols, m_rows, m_numSheets);

        // Use resource ID for unique internal paths to avoid collisions on reimport
        auto const stackResourceId = stack->GetModelResourceID();

        unsigned int i = 0u;

        for (auto & file : files)
        {
            auto newFileName =
              fmt::format("/volume/ImageStack_{}/layer_{}.png", stackResourceId, i);
            stack->CreateSheetFromFile(i, newFileName, file.string());
            ++i;
        }

        return stack;
    }

    ImportResult ImageStackCreator::importDirectoryWithPadding(Lib3MF::PModel model,
                                                               std::filesystem::path const & path,
                                                               ImportProgressCallback progressCb)
    {
        ImportResult result;

        auto files = getFiles(path);
        if (files.empty())
        {
            return result;
        }

        // T049: Detect maximum dimensions across all images
        determineMaxDimensions(files, result.maxWidth, result.maxHeight);

        m_cols = result.maxWidth;
        m_rows = result.maxHeight;
        m_numSheets = static_cast<unsigned int>(files.size());

        auto stack = model->AddImageStack(m_cols, m_rows, m_numSheets);

        // Use resource ID for unique internal paths to avoid collisions on reimport
        auto const stackResourceId = stack->GetModelResourceID();

        unsigned int i = 0u;
        size_t const total = files.size();

        for (auto & file : files)
        {
            // T048: Report progress
            if (progressCb)
            {
                progressCb(i + 1, total, file.filename().string());
            }

            // Check if this image needs padding
            std::vector<unsigned char> buffer;
            if (0 != lodepng::load_file(buffer, file.string()))
            {
                throw std::runtime_error("Error loading image file: " + file.string());
            }

            unsigned int width = 0;
            unsigned int height = 0;
            lodepng::State state;
            lodepng_inspect(&width, &height, &state, buffer.data(), buffer.size());

            // T050-T051: If image is smaller than max, pad it; track padded files
            if (width < result.maxWidth || height < result.maxHeight)
            {
                result.paddedFiles.push_back(file.filename().string());

                // Load full image data for padding
                std::vector<unsigned char> imageData;
                unsigned int actualWidth = 0;
                unsigned int actualHeight = 0;
                if (0 != lodepng::decode(imageData, actualWidth, actualHeight, file.string()))
                {
                    throw std::runtime_error("Error decoding image file: " + file.string());
                }

                // Create padded image (RGBA)
                std::vector<unsigned char> paddedData(
                    static_cast<size_t>(result.maxWidth) * result.maxHeight * 4, 0);

                // Copy original image into padded buffer
                for (unsigned int y = 0; y < actualHeight; ++y)
                {
                    for (unsigned int x = 0; x < actualWidth; ++x)
                    {
                        size_t srcIdx = (y * actualWidth + x) * 4;
                        size_t dstIdx = (y * result.maxWidth + x) * 4;
                        for (int c = 0; c < 4; ++c)
                        {
                            paddedData[dstIdx + c] = imageData[srcIdx + c];
                        }
                    }
                }

                // Encode padded image to temporary PNG in memory
                std::vector<unsigned char> pngData;
                lodepng::encode(pngData, paddedData, result.maxWidth, result.maxHeight);

                // Create sheet from buffer
                auto newFileName =
                    fmt::format("/volume/ImageStack_{}/layer_{}.png", stackResourceId, i);
                stack->CreateSheetFromBuffer(i, newFileName, pngData);
            }
            else
            {
                auto newFileName =
                    fmt::format("/volume/ImageStack_{}/layer_{}.png", stackResourceId, i);
                stack->CreateSheetFromFile(i, newFileName, file.string());
            }
            ++i;
        }

        result.imageStack = stack;

        // Create the FunctionFromImage3D
        if (stack)
        {
            result.function = model->AddFunctionFromImage3D(stack.get());
        }

        return result;
    }

    Lib3MF::PFunctionFromImage3D
    ImageStackCreator::importDirectoryAsFunctionFromImage3D(Lib3MF::PModel model,
                                                            std::filesystem::path const & path)
    {
        auto stack = addImageStackFromDirectory(model, path);
        if (stack)
        {
            auto function = model->AddFunctionFromImage3D(stack.get());
            return function;
        }

        return {};
    }

    Files ImageStackCreator::getFiles(std::filesystem::path const & path) const
    {
        if (!std::filesystem::is_directory(path))
        {
            return {};
        }

        // get all png files in the direcotry and order them alpahnumerically by name
        Files files;
        for (auto & p : std::filesystem::directory_iterator(path))
        {
            if (p.path().extension() == ".png")
            {
                files.push_back(p.path());
            }
        }

        std::sort(files.begin(),
                  files.end(),
                  [](std::filesystem::path const & a, std::filesystem::path const & b)
                  { return a.filename().string() < b.filename().string(); });

        return files;
    }

    void ImageStackCreator::determineImageStackSize(Files const & files)
    {
        if (files.empty())
        {
            return;
        }

        // get the size of the first image
        std::vector<unsigned char> buffer;
        if (0 != lodepng::load_file(buffer, files.front().string()))
        {
            throw std::runtime_error("Error loading image file: " + files.front().string());
        }

        lodepng::State state;
        lodepng_inspect(&m_cols, &m_rows, &state, &buffer[0], buffer.size());
        m_numSheets = static_cast<unsigned int>(files.size());
    }

    void ImageStackCreator::determineMaxDimensions(Files const & files,
                                                   unsigned int & maxWidth,
                                                   unsigned int & maxHeight)
    {
        maxWidth = 0;
        maxHeight = 0;

        for (auto const & file : files)
        {
            std::vector<unsigned char> buffer;
            if (0 != lodepng::load_file(buffer, file.string()))
            {
                throw std::runtime_error("Error loading image file: " + file.string());
            }

            unsigned int width = 0;
            unsigned int height = 0;
            lodepng::State state;
            lodepng_inspect(&width, &height, &state, buffer.data(), buffer.size());

            maxWidth = std::max(maxWidth, width);
            maxHeight = std::max(maxHeight, height);
        }
    }
}