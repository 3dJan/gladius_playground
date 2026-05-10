/**
 * @file ThreeMfPostProcessor.h
 * @brief Post-processes 3MF ZIP files to inject slicer-specific attributes
 *
 * Since lib3mf doesn't support custom attributes on triangle elements,
 * this class post-processes the 3MF ZIP to inject PrusaSlicer/OrcaSlicer
 * specific attributes (e.g. slic3rpe:mmu_segmentation) onto each triangle.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gladius::io
{

    /**
     * @class ThreeMfPostProcessor
     * @brief Injects slicer-specific data into an existing 3MF file
     */
    class ThreeMfPostProcessor
    {
      public:
        /// Inject MMU segmentation attributes onto triangle elements and
        /// add a Slic3r model config file to the 3MF ZIP.
        ///
        /// @param filePath Path to the existing 3MF file (modified in-place)
        /// @param mmuAttributes Per-triangle MMU segmentation hex strings
        /// @param slicerConfigXml Content of Metadata/Slic3r_PE_model.config
        static void injectMmuSegmentation(std::filesystem::path const& filePath,
                                          std::vector<std::string> const& mmuAttributes,
                                          std::string const& slicerConfigXml);

        /// Modify the model XML to inject mmu_segmentation attributes on triangles.
        /// Emits both paint_color (OrcaSlicer) and slic3rpe:mmu_segmentation (PrusaSlicer).
        static std::string injectTriangleAttributes(std::string const& modelXml,
                                                    std::vector<std::string> const& mmuAttributes);

      private:
        /// Read all entries from a ZIP file
        struct ZipEntry
        {
            std::string name;
            std::vector<std::uint8_t> data;
        };

        static std::vector<ZipEntry> readZip(std::filesystem::path const& filePath);
        static void writeZip(std::filesystem::path const& filePath,
                             std::vector<ZipEntry> const& entries);
    };

} // namespace gladius::io
