#include "ShellMaterialOrdering.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

namespace gladius::io
{
    float ShellMaterialOrdering::translucencyScore(FilamentOpticalProperties const& filament) noexcept
    {
        float sum = 0.0F;
        int count = 0;
        for (int channel = 0; channel < 3; ++channel)
        {
            float const td = filament.transmissionDistance[channel];
            if (td > 0.0F)
            {
                sum += td;
                ++count;
            }
        }

        if (count > 0)
        {
            return sum / static_cast<float>(count);
        }

        if (filament.opacity <= 0.0F)
        {
            return std::numeric_limits<float>::infinity();
        }

        if (filament.opacity >= 1.0F)
        {
            return 0.0F;
        }

        float const safeReferenceThickness = std::max(filament.referenceThickness, 1e-6F);
        float const oneMinusOpacity = std::max(1.0F - filament.opacity, 1e-6F);
        float const alpha = -std::log(oneMinusOpacity) / safeReferenceThickness;
        if (alpha <= 0.0F)
        {
            return std::numeric_limits<float>::infinity();
        }

        return 1.0F / alpha;
    }

    OrderedShellMaterials ShellMaterialOrdering::reorderForShells(
        FilamentStack const& stack,
        std::size_t backgroundIndex,
        IlluminationMode mode)
    {
        OrderedShellMaterials ordered;
        std::size_t const count = stack.size();
        ordered.originalToOrdered.resize(count, 0U);

        if (count == 0U)
        {
            return ordered;
        }

        std::vector<std::size_t> indices(count);
        std::iota(indices.begin(), indices.end(), 0U);

        bool const hasBackground = backgroundIndex < count;
        if (hasBackground)
        {
            std::stable_sort(indices.begin(), indices.end(), [backgroundIndex](std::size_t lhs, std::size_t rhs) {
                if (lhs == backgroundIndex)
                {
                    return true;
                }
                if (rhs == backgroundIndex)
                {
                    return false;
                }
                return false;
            });
        }

        auto reorderBegin = indices.begin() + (hasBackground ? 1 : 0);
        if (mode == IlluminationMode::Frontlit)
        {
            std::stable_sort(reorderBegin, indices.end(), [&stack](std::size_t lhs, std::size_t rhs) {
                return translucencyScore(stack[lhs]) < translucencyScore(stack[rhs]);
            });
        }

        ordered.stack.filaments.reserve(count);
        ordered.orderedToOriginal.reserve(count);
        for (std::size_t orderedIndex = 0; orderedIndex < count; ++orderedIndex)
        {
            std::size_t const originalIndex = indices[orderedIndex];
            ordered.stack.push_back(stack[originalIndex]);
            ordered.orderedToOriginal.push_back(originalIndex);
            ordered.originalToOrdered[originalIndex] = orderedIndex;
        }

        if (hasBackground)
        {
            ordered.backgroundIndex = 0U;
        }

        return ordered;
    }

    OrderedShellMaterials ShellMaterialOrdering::optimizeGlobalOrderForShells(
        FilamentStack const& stack,
        std::size_t backgroundIndex,
        IlluminationMode mode,
        std::function<float(FilamentStack const&, std::size_t)> const& scorer,
        std::size_t exhaustiveSearchLimit)
    {
        OrderedShellMaterials const heuristic = reorderForShells(stack, backgroundIndex, mode);

        if (!scorer || stack.size() <= 1U || mode != IlluminationMode::Frontlit)
        {
            return heuristic;
        }

        std::size_t const count = stack.size();
        bool const hasBackground = backgroundIndex < count;
        std::size_t const permutableCount = count - (hasBackground ? 1U : 0U);

        if (permutableCount <= 1U || count > exhaustiveSearchLimit)
        {
            return heuristic;
        }

        std::vector<std::size_t> indices(count);
        std::iota(indices.begin(), indices.end(), 0U);

        std::vector<std::size_t> fixedPrefix;
        std::vector<std::size_t> permutableIndices;
        fixedPrefix.reserve(hasBackground ? 1U : 0U);
        permutableIndices.reserve(permutableCount);

        for (std::size_t index : indices)
        {
            if (hasBackground && index == backgroundIndex)
            {
                fixedPrefix.push_back(index);
            }
            else
            {
                permutableIndices.push_back(index);
            }
        }

        std::sort(permutableIndices.begin(), permutableIndices.end());

        auto makeOrdered = [&](std::vector<std::size_t> const& orderedIndices) {
            OrderedShellMaterials ordered;
            ordered.originalToOrdered.resize(count, 0U);
            ordered.stack.filaments.reserve(count);
            ordered.orderedToOriginal.reserve(count);

            for (std::size_t orderedIndex = 0; orderedIndex < orderedIndices.size(); ++orderedIndex)
            {
                std::size_t const originalIndex = orderedIndices[orderedIndex];
                ordered.stack.push_back(stack[originalIndex]);
                ordered.orderedToOriginal.push_back(originalIndex);
                ordered.originalToOrdered[originalIndex] = orderedIndex;
            }

            if (hasBackground)
            {
                ordered.backgroundIndex = 0U;
            }

            return ordered;
        };

        float bestScore = scorer(heuristic.stack, heuristic.backgroundIndex);
        OrderedShellMaterials bestOrdered = heuristic;

        do
        {
            std::vector<std::size_t> orderedIndices = fixedPrefix;
            orderedIndices.insert(orderedIndices.end(), permutableIndices.begin(), permutableIndices.end());

            OrderedShellMaterials candidate = makeOrdered(orderedIndices);
            float const score = scorer(candidate.stack, candidate.backgroundIndex);
            if (score < bestScore)
            {
                bestScore = score;
                bestOrdered = std::move(candidate);
            }
        }
        while (std::next_permutation(permutableIndices.begin(), permutableIndices.end()));

        return bestOrdered;
    }

    OrderedShellMaterials ShellMaterialOrdering::optimizeGlobalOrderForPalette(
        FilamentStack const& stack,
        std::size_t backgroundIndex,
        IlluminationMode mode,
        ThicknessConstraints const& constraints,
        std::vector<Eigen::Vector3f> const& targetColors,
        std::size_t exhaustiveSearchLimit)
    {
        if (targetColors.empty())
        {
            return reorderForShells(stack, backgroundIndex, mode);
        }

        auto paletteScorer = [&constraints, &targetColors, mode](FilamentStack const& candidateStack,
                                                                 std::size_t candidateBackgroundIndex) {
            FrontlitThicknessSolver solver{candidateStack, constraints, mode, candidateBackgroundIndex};

            float totalError = 0.0F;
            for (Eigen::Vector3f const& target : targetColors)
            {
                ThicknessSolution const solution = solver.solve(target);
                totalError += solution.colorError;
            }

            return totalError / static_cast<float>(targetColors.size());
        };

        return optimizeGlobalOrderForShells(
            stack,
            backgroundIndex,
            mode,
            paletteScorer,
            exhaustiveSearchLimit);
    }
} // namespace gladius::io