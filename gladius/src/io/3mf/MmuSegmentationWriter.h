/**
 * @file MmuSegmentationWriter.h
 * @brief Encodes per-face extruder assignments in PrusaSlicer/OrcaSlicer MMU segmentation format
 *
 * The MMU segmentation format is a proprietary per-triangle annotation used by
 * PrusaSlicer and OrcaSlicer for multi-material/multi-color printing. Each triangle
 * gets a hex-encoded bitstream string that encodes its assigned extruder index.
 */

#pragma once

#include "io/3mf/ColorQuantizer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gladius::io
{

    /**
     * @class MmuSegmentationWriter
     * @brief Encodes per-face extruder assignments as Slic3r MMU segmentation hex strings
     */
    class MmuSegmentationWriter
    {
      public:
        /// Encode a 1-based extruder index to an MMU segmentation hex string.
        /// Returns empty string for extruder index 0 (NONE / unassigned).
        ///
        /// Encoding for unsplit (whole-face) triangles:
        /// - State 0: empty (no annotation)
        /// - State 1-2: single hex digit ("1" or "2"), bits = state<<2 | 0
        /// - State >= 3: prefix 0xC (bits 1100), then (state-3) in base-15 nibbles
        static std::string encodeExtruderState(int extruderIndex);

        /// Map palette indices (0-based) to 1-based extruder indices and encode
        /// each face's assignment as an MMU segmentation hex string.
        /// Returns a vector of hex strings, one per face.
        static std::vector<std::string> encodeFaceExtruders(QuantizedPalette const& palette);

        static constexpr int MAX_EXTRUDERS = 16;
    };

} // namespace gladius::io
