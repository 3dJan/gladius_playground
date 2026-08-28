#include "slicer/GridContourBuilder.h"

#include "ContourExtractor.h"
#include "MortonQuadtree.h"
#include "QuadtreeContourExtractor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gladius::slicer
{
    ContourGridDefinition makeAdaptiveContourGrid(float4 modelBounds)
    {
        constexpr float padding_mm = 2.0f;
        constexpr float targetCellSize_mm = 0.1f;
        constexpr int minResolution = 32;
        constexpr int maxResolution = 2048;

        ContourGridDefinition definition;
        definition.clippingArea = {modelBounds.x - padding_mm,
                                   modelBounds.y - padding_mm,
                                   modelBounds.z + padding_mm,
                                   modelBounds.w + padding_mm};

        float const domainWidth = definition.clippingArea.z - definition.clippingArea.x;
        float const domainHeight = definition.clippingArea.w - definition.clippingArea.y;
        if (!std::isfinite(domainWidth) || !std::isfinite(domainHeight) ||
            domainWidth <= 0.0f || domainHeight <= 0.0f)
        {
            throw std::invalid_argument("Adaptive contour extraction requires valid model bounds");
        }

                definition.width = static_cast<int>(std::clamp(
                    std::ceil(domainWidth / targetCellSize_mm) + 1.0f,
                    static_cast<float>(minResolution),
                    static_cast<float>(maxResolution)));
                definition.height = static_cast<int>(std::clamp(
                    std::ceil(domainHeight / targetCellSize_mm) + 1.0f,
                    static_cast<float>(minResolution),
                    static_cast<float>(maxResolution)));
        return definition;
    }

    float SdfGrid::sample(Eigen::Vector2f const & pos) const
    {
        auto const domainW = clippingArea.z - clippingArea.x;
        auto const domainH = clippingArea.w - clippingArea.y;
        if (domainW <= 0.0f || domainH <= 0.0f)
        {
            return 0.0f;
        }

        float const px = (pos.x() - clippingArea.x) / domainW * static_cast<float>(width - 1);
        float const py = (pos.y() - clippingArea.y) / domainH * static_cast<float>(height - 1);

        int const ix = std::clamp(static_cast<int>(px), 0, width - 1);
        int const iy = std::clamp(static_cast<int>(py), 0, height - 1);
        int const ix1 = std::min(ix + 1, width - 1);
        int const iy1 = std::min(iy + 1, height - 1);

        float const tx = px - static_cast<float>(ix);
        float const ty = py - static_cast<float>(iy);

        auto const idx = [&](int x, int y)
        {
            return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(x);
        };

        float const v00 = values[idx(ix, iy)];
        float const v10 = values[idx(ix1, iy)];
        float const v01 = values[idx(ix, iy1)];
        float const v11 = values[idx(ix1, iy1)];

        return v00 * (1.0f - tx) * (1.0f - ty) + v10 * tx * (1.0f - ty) +
               v01 * (1.0f - tx) * ty + v11 * tx * ty;
    }

    MarchingSquaresStates GridContourBuilder::buildMarchingSquareStates(
      ComputeContext & context, SdfGrid const & grid)
    {
        // The grid holds node samples; the marching squares state image has one cell
        // per node with the same dimensions as the OpenCL slicer output.
        MarchingSquaresStates states(context, static_cast<std::size_t>(grid.width),
                                     static_cast<std::size_t>(grid.height));
        states.allocateOnDevice();

        for (int y = 0; y < grid.height; ++y)
        {
            for (int x = 0; x < grid.width; ++x)
            {
                cl_char state = 0;

                auto const sampleAt = [&](int sx, int sy)
                {
                    int const cx = std::clamp(sx, 0, grid.width - 1);
                    int const cy = std::clamp(sy, 0, grid.height - 1);
                    return grid.values[static_cast<std::size_t>(cy) *
                                         static_cast<std::size_t>(grid.width) +
                                       static_cast<std::size_t>(cx)];
                };

                if (sampleAt(x - 1, y - 1) < 0.0f)
                {
                    state |= 1;
                }
                if (sampleAt(x, y - 1) < 0.0f)
                {
                    state |= 2;
                }
                if (sampleAt(x - 1, y) < 0.0f)
                {
                    state |= 4;
                }
                if (sampleAt(x, y) < 0.0f)
                {
                    state |= 8;
                }

                states.setValue(static_cast<std::size_t>(x), static_cast<std::size_t>(y), state);
            }
        }
        return states;
    }

    PolyLines GridContourBuilder::extractDenseContours(SdfGrid const & grid,
                                                       ComputeContext & context,
                                                       events::SharedLogger const & logger)
    {
        ContourExtractor extractor(logger);
        auto states = buildMarchingSquareStates(context, grid);
        extractor.addIsoLineFromMarchingSquare(states, grid.clippingArea);
        extractor.runPostProcessing();
        return extractor.getContour();
    }

    void GridContourBuilder::extractAdaptiveContours(SdfGrid const & grid,
                                                     float minFeatureSize_mm,
                                                     ContourExtractor & contourExtractor)
    {
        contourExtractor.clear();

        auto const expectedValueCount =
                    static_cast<std::size_t>(grid.width) * static_cast<std::size_t>(grid.height);
        if (grid.width < 2 || grid.height < 2 || grid.values.size() != expectedValueCount)
        {
            throw std::invalid_argument("Adaptive contour extraction requires a complete SDF grid");
        }

        float const xMin = grid.clippingArea.x;
        float const yMin = grid.clippingArea.y;
        float const xMax = grid.clippingArea.z;
        float const yMax = grid.clippingArea.w;
        float const domainW = xMax - xMin;
        float const domainH = yMax - yMin;
        if (domainW <= 0.0f || domainH <= 0.0f)
        {
            return;
        }

        auto const sdfFunc = [&](Eigen::Vector2f const & pos)
        {
            return grid.sample(pos);
        };

        float const nativeCellSize = domainW / static_cast<float>(grid.width - 1);
        float const targetCellSize = std::max(minFeatureSize_mm * 0.5f, nativeCellSize);
        float const domainSize = std::max(domainW, domainH);
        std::size_t maxDepth = 3U;
        while (maxDepth < 14U && (domainSize / static_cast<float>(1U << maxDepth)) > targetCellSize)
        {
            ++maxDepth;
        }

        BoundingBox2D const quadBounds{Eigen::Vector2f{xMin, yMin}, Eigen::Vector2f{xMax, yMax}};

        MortonQuadtreeConfig cfg;
        cfg.initialDepth = 3U;
        cfg.maxDepth = maxDepth;
        cfg.isoValue = 0.0f;
        cfg.minFeatureSize = targetCellSize;
        cfg.enableAdaptiveRefinement = false;
        cfg.maxNodes = 2000000U;
        cfg.refinementPasses = 1U;

        MortonQuadtree quadtree(quadBounds);
        quadtree.build(cfg);

        for (std::size_t depth = cfg.initialDepth; depth < maxDepth; ++depth)
        {
            QuadtreeContourExtractor::populateCornerValues(quadtree, sdfFunc, 0.0f);
            quadtree.refineAdaptively(cfg);
        }
        QuadtreeContourExtractor::populateCornerValues(quadtree, sdfFunc, 0.0f);

        for (int balancePass = 0; balancePass < 8; ++balancePass)
        {
            auto const created = quadtree.ensureBalancedSurface(cfg);
            if (created == 0U)
            {
                break;
            }
            QuadtreeContourExtractor::populateCornerValues(quadtree, sdfFunc, 0.0f);
        }

        float const snapTol = std::max(1e-4f, nativeCellSize * 0.1f);
        QuadtreeContourExtractor const extractor;
        auto const sparsePolyLines = extractor.extractPolyLines(quadtree, 0.0f, snapTol);

        auto & result = contourExtractor.getContour();
        result.reserve(sparsePolyLines.size());
        for (auto const & sparsePoly : sparsePolyLines)
        {
            if (sparsePoly.vertices.size() < 2)
            {
                continue;
            }
            PolyLine poly;
            poly.isClosed = sparsePoly.isClosed;
            poly.vertices.assign(sparsePoly.vertices.begin(), sparsePoly.vertices.end());
            result.push_back(std::move(poly));
        }
        contourExtractor.runPostProcessing();
    }

    PolyLines GridContourBuilder::extractAdaptiveContours(SdfGrid const & grid,
                                                          float minFeatureSize_mm,
                                                          events::SharedLogger const & logger)
    {
        ContourExtractor contourExtractor(logger);
        extractAdaptiveContours(grid, minFeatureSize_mm, contourExtractor);
        return contourExtractor.getContour();
    }
}
