/**
 * @file ColorRegionizer.h
 * @brief Discrete printable-region decomposition for standards-based 3MF fallback
 *
 * Converts per-face palette assignments into contiguous printable regions
 * suitable for standard 3MF component or object partitioning.
 */

#pragma once

#include "ColorQuantizer.h"
#include "FaceColors.h"

#include <cstdint>
#include <vector>

namespace gladius::io
{

    /// Kind of standard 3MF entity used to represent a printable region
    enum class PrintableRegionKind
    {
        Component,
        Object,
        BuildItem
    };

    /// A discrete printable region: faces sharing the same palette color
    struct PrintableRegion
    {
        std::uint32_t regionId = 0;
        std::uint32_t paletteIndex = 0;
        std::vector<std::uint32_t> triangleIndices;
        PrintableRegionKind kind = PrintableRegionKind::Component;
    };

    /**
     * @class ColorRegionizer
     * @brief Decomposes a quantized-palette-mapped mesh into printable regions
     *
     * Groups triangles by their assigned palette entry and produces a
     * set of printable regions that can be emitted as standard 3MF
     * components, objects, or build items.
     */
    class ColorRegionizer
    {
      public:
        /// Create printable regions from a quantized palette mapping
        static std::vector<PrintableRegion> regionize(QuantizedPalette const& palette,
                                                       PrintableRegionKind kind);
    };

} // namespace gladius::io
