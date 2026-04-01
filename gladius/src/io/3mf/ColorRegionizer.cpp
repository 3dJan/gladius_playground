/**
 * @file ColorRegionizer.cpp
 * @brief Implementation of discrete printable-region decomposition
 */

#include "ColorRegionizer.h"

#include <algorithm>
#include <map>

namespace gladius::io
{

    std::vector<PrintableRegion> ColorRegionizer::regionize(QuantizedPalette const& palette,
                                                             PrintableRegionKind kind)
    {
        if (palette.sourceToPaletteMap.empty())
        {
            return {};
        }

        // Group face indices by palette entry
        std::map<std::uint32_t, std::vector<std::uint32_t>> groups;
        for (std::uint32_t faceIdx = 0;
             faceIdx < static_cast<std::uint32_t>(palette.sourceToPaletteMap.size());
             ++faceIdx)
        {
            auto const paletteIdx = palette.sourceToPaletteMap[faceIdx];
            groups[paletteIdx].push_back(faceIdx);
        }

        // Build regions in deterministic order (sorted by palette index)
        std::vector<PrintableRegion> regions;
        regions.reserve(groups.size());

        std::uint32_t regionId = 0;
        for (auto& [paletteIdx, faceIndices] : groups)
        {
            PrintableRegion region;
            region.regionId = regionId++;
            region.paletteIndex = paletteIdx;
            region.triangleIndices = std::move(faceIndices);
            region.kind = kind;
            regions.push_back(std::move(region));
        }

        return regions;
    }

} // namespace gladius::io
