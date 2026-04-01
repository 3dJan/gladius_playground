/**
 * @file ColorQuantizer.h
 * @brief Deterministic adaptive color quantization for printable-region export
 *
 * Reduces a set of unique colors to a bounded palette suitable for discrete
 * printable-region assignment while preserving as much color variation as
 * practical. Quantization is deterministic: same input + same settings produce
 * identical output.
 */

#pragma once

#include "FaceColors.h"

#include <cstdint>
#include <vector>

namespace gladius::io
{

    /// Result of adaptive palette generation
    struct QuantizedPalette
    {
        std::vector<Color8> colors;                    ///< Palette colors
        std::vector<std::uint32_t> sourceToPaletteMap; ///< Per-face palette index
        float maxApproximationError = 0.0f;            ///< Worst-case L2 color distance
    };

    /**
     * @class ColorQuantizer
     * @brief Deterministic adaptive color quantization
     *
     * Given per-face colors and a maximum palette size, produces a reduced
     * palette and a mapping from each face to its assigned palette entry.
     * Uses median-cut quantization for deterministic output.
     */
    class ColorQuantizer
    {
      public:
        /// Quantize face colors into at most maxColors distinct palette entries
        static QuantizedPalette quantize(FaceColors const& faceColors, std::uint32_t maxColors);

        /// Quantize using multi-point oversampling for sharper material boundaries.
        /// Takes multiple sample sets per face (e.g. centroid + edge midpoints).
        /// Builds the palette from the first sample set, then assigns each face
        /// to the palette entry that wins a majority vote across all samples.
        static QuantizedPalette quantizeOversampled(
            std::vector<FaceColors> const& sampleSets,
            std::uint32_t maxColors);

        /// Count the number of unique opaque colors (ignoring alpha)
        static std::size_t countUniqueOpaqueColors(FaceColors const& faceColors);

        /// Check whether any face color has non-opaque alpha
        static bool hasTransparency(FaceColors const& faceColors);
    };

} // namespace gladius::io
