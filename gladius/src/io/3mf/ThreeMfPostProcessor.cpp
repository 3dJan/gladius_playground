/**
 * @file ThreeMfPostProcessor.cpp
 * @brief Implementation of 3MF post-processing for slicer-specific attributes
 */

#include "ThreeMfPostProcessor.h"

#include <fmt/format.h>
#include <minizip/unzip.h>
#include <minizip/zip.h>

#include <stdexcept>

namespace gladius::io
{

    void ThreeMfPostProcessor::injectMmuSegmentation(
        std::filesystem::path const& filePath,
        std::vector<std::string> const& mmuAttributes,
        std::string const& slicerConfigXml)
    {
        // Read all entries from the existing ZIP
        auto entries = readZip(filePath);

        // Find and modify the model XML
        bool modelFound = false;
        for (auto& entry : entries)
        {
            if (entry.name == "3D/3dmodel.model")
            {
                std::string modelXml(entry.data.begin(), entry.data.end());
                std::string modified = injectTriangleAttributes(modelXml, mmuAttributes);
                entry.data.assign(modified.begin(), modified.end());
                modelFound = true;
                break;
            }
        }

        if (!modelFound)
        {
            throw std::runtime_error("3MF post-processing failed: no 3D/3dmodel.model found");
        }

        // Add the slicer config file
        if (!slicerConfigXml.empty())
        {
            ZipEntry configEntry;
            configEntry.name = "Metadata/Slic3r_PE_model.config";
            configEntry.data.assign(slicerConfigXml.begin(), slicerConfigXml.end());
            entries.push_back(std::move(configEntry));
        }

        // Rewrite the ZIP with modified contents
        writeZip(filePath, entries);
    }

    std::vector<ThreeMfPostProcessor::ZipEntry>
    ThreeMfPostProcessor::readZip(std::filesystem::path const& filePath)
    {
        std::vector<ZipEntry> entries;

        unzFile archive = unzOpen(filePath.string().c_str());
        if (!archive)
        {
            throw std::runtime_error(
                fmt::format("Failed to open ZIP for reading: {}", filePath.string()));
        }

        int status = unzGoToFirstFile(archive);
        while (status == UNZ_OK)
        {
            unz_file_info fileInfo;
            char filename[512];
            if (unzGetCurrentFileInfo(archive, &fileInfo, filename, sizeof(filename),
                                      nullptr, 0, nullptr, 0) != UNZ_OK)
            {
                unzClose(archive);
                throw std::runtime_error("Failed to get file info from ZIP");
            }

            ZipEntry entry;
            entry.name = filename;
            entry.data.resize(fileInfo.uncompressed_size);

            if (unzOpenCurrentFile(archive) != UNZ_OK)
            {
                unzClose(archive);
                throw std::runtime_error(
                    fmt::format("Failed to open entry in ZIP: {}", entry.name));
            }

            int bytesRead = unzReadCurrentFile(archive, entry.data.data(),
                                               static_cast<unsigned>(entry.data.size()));
            unzCloseCurrentFile(archive);

            if (bytesRead < 0 || static_cast<std::size_t>(bytesRead) != entry.data.size())
            {
                unzClose(archive);
                throw std::runtime_error(
                    fmt::format("Failed to read entry from ZIP: {}", entry.name));
            }

            entries.push_back(std::move(entry));
            status = unzGoToNextFile(archive);
            if (status != UNZ_OK && status != UNZ_END_OF_LIST_OF_FILE)
            {
                unzClose(archive);
                throw std::runtime_error("Failed to iterate ZIP entries");
            }
        }

        unzClose(archive);
        return entries;
    }

    void ThreeMfPostProcessor::writeZip(std::filesystem::path const& filePath,
                                         std::vector<ZipEntry> const& entries)
    {
        // Write to a temp file first, then rename to avoid data loss on failure
        auto const tempPath = filePath.string() + ".tmp";
        std::filesystem::remove(tempPath);

        zipFile archive = zipOpen(tempPath.c_str(), APPEND_STATUS_CREATE);
        if (!archive)
        {
            throw std::runtime_error(
                fmt::format("Failed to open ZIP for writing: {}", tempPath));
        }

        for (auto const& entry : entries)
        {
            zip_fileinfo fi = {};

            if (zipOpenNewFileInZip(archive, entry.name.c_str(), &fi,
                                    nullptr, 0, nullptr, 0, nullptr,
                                    Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK)
            {
                zipClose(archive, nullptr);
                throw std::runtime_error(
                    fmt::format("Failed to create entry in ZIP: {}", entry.name));
            }

            if (!entry.data.empty())
            {
                if (zipWriteInFileInZip(archive, entry.data.data(),
                                        static_cast<unsigned>(entry.data.size())) != ZIP_OK)
                {
                    zipCloseFileInZip(archive);
                    zipClose(archive, nullptr);
                    throw std::runtime_error(
                        fmt::format("Failed to write entry in ZIP: {}", entry.name));
                }
            }

            zipCloseFileInZip(archive);
        }

        zipClose(archive, nullptr);

        // Atomically replace the original file
        std::filesystem::rename(tempPath, filePath);
    }

    std::string ThreeMfPostProcessor::injectTriangleAttributes(
        std::string const& modelXml,
        std::vector<std::string> const& mmuAttributes)
    {
        // Add the slic3rpe namespace to the root element if not present
        std::string result = modelXml;

        // Find the <model element and inject namespace declaration
        constexpr char const* NS_DECL = "xmlns:slic3rpe=\"http://schemas.slic3r.org/3mf/2017/06\"";
        auto modelTagPos = result.find("<model");
        if (modelTagPos != std::string::npos && result.find("xmlns:slic3rpe") == std::string::npos)
        {
            auto closeBracket = result.find('>', modelTagPos);
            if (closeBracket != std::string::npos)
            {
                result.insert(closeBracket, std::string(" ") + NS_DECL);
            }
        }

        // Inject mmu_segmentation attribute on each <triangle element
        // We find each <triangle .../>  and insert the attribute before the closing />
        std::size_t searchPos = 0;
        std::size_t triangleIdx = 0;

        while (searchPos < result.size() && triangleIdx < mmuAttributes.size())
        {
            auto triPos = result.find("<triangle ", searchPos);
            if (triPos == std::string::npos)
            {
                break;
            }

            auto closePos = result.find("/>", triPos);
            if (closePos == std::string::npos)
            {
                break;
            }

            // Only inject if this triangle has an MMU attribute
            if (!mmuAttributes[triangleIdx].empty())
            {
                // Emit both paint_color (OrcaSlicer/BambuStudio) and
                // slic3rpe:mmu_segmentation (PrusaSlicer) for maximum compatibility
                std::string attr = fmt::format(
                    " paint_color=\"{}\" slic3rpe:mmu_segmentation=\"{}\"",
                    mmuAttributes[triangleIdx],
                    mmuAttributes[triangleIdx]);
                result.insert(closePos, attr);
                closePos += attr.size();
            }

            searchPos = closePos + 2; // skip past "/>"
            ++triangleIdx;
        }

        return result;
    }

} // namespace gladius::io
