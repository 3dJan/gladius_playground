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

        [[nodiscard]] static float translucencyScore(FilamentOpticalProperties const& filament) noexcept;
    };
} // namespace gladius::io