#pragma once

#include "FilamentOpticalProperties.h"
#include "FrontlitThicknessSolver.h"

#include <functional>
#include <limits>
#include <vector>

namespace gladius::io
{
    struct OrderedShellMaterials
    {
        FilamentStack stack;
        std::vector<std::size_t> orderedToOriginal;
        std::vector<std::size_t> originalToOrdered;
        std::size_t backgroundIndex{std::numeric_limits<std::size_t>::max()};
    };

    class ShellMaterialOrdering
    {
      public:
        [[nodiscard]] static OrderedShellMaterials reorderForShells(
            FilamentStack const& stack,
            std::size_t backgroundIndex,
            IlluminationMode mode);

        [[nodiscard]] static OrderedShellMaterials optimizeGlobalOrderForShells(
            FilamentStack const& stack,
            std::size_t backgroundIndex,
            IlluminationMode mode,
            std::function<float(FilamentStack const&, std::size_t)> const& scorer,
            std::size_t exhaustiveSearchLimit = 6U);

        /// Optimize global shell order for a target palette by solving each target color
        /// against every candidate permutation and selecting the order with the lowest
        /// mean achieved color error. Falls back to the translucency heuristic for large
        /// stacks or non-frontlit modes. Only the printable (non-background) layers are
        /// permuted; the background material stays pinned in its semantic role.
        [[nodiscard]] static OrderedShellMaterials optimizeGlobalOrderForPalette(
            FilamentStack const& stack,
            std::size_t backgroundIndex,
            IlluminationMode mode,
            ThicknessConstraints const& constraints,
            std::vector<Eigen::Vector3f> const& targetColors,
            std::size_t exhaustiveSearchLimit = 6U);

        [[nodiscard]] static float translucencyScore(FilamentOpticalProperties const& filament) noexcept;
    };
} // namespace gladius::io
