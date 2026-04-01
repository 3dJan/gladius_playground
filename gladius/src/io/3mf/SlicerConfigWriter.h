/**
 * @file SlicerConfigWriter.h
 * @brief Generates PrusaSlicer/OrcaSlicer model config XML for 3MF packages
 *
 * PrusaSlicer and OrcaSlicer store per-object and per-volume metadata
 * (including extruder assignments) in a "Metadata/Slic3r_PE_model.config"
 * file inside the 3MF ZIP archive. This writer generates that XML.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gladius::io
{

    /// Per-volume metadata for the slicer config
    struct SlicerVolumeInfo
    {
        std::string name;
        std::size_t firstTriangleId = 0;
        std::size_t lastTriangleId = 0;
        int defaultExtruder = 1; ///< Default extruder for the volume (1-based)
    };

    /**
     * @class SlicerConfigWriter
     * @brief Generates Slic3r_PE_model.config XML content
     */
    class SlicerConfigWriter
    {
      public:
        /// Generate the model config XML for one object with one volume.
        /// @param objectId The 3MF object ID (matches the mesh object's unique resource ID)
        /// @param volume Volume metadata
        /// @return Complete XML string for Metadata/Slic3r_PE_model.config
        static std::string generate(int objectId, SlicerVolumeInfo const& volume);

        /// The path inside the 3MF ZIP where the config file lives
        static constexpr char const* CONFIG_PATH = "Metadata/Slic3r_PE_model.config";
    };

} // namespace gladius::io
