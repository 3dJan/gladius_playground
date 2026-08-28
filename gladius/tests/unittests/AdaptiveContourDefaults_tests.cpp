#include "nodes/BuildParameter.h"
#include "slicer/GridContourBuilder.h"
#include "webgpu/WebGPUContourGenerator.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <stdexcept>

namespace gladius::tests
{
    namespace
    {
        [[nodiscard]] slicer::SdfGrid makeCircleGrid()
        {
            constexpr int resolution = 129;
            constexpr float min = -2.0f;
            constexpr float max = 2.0f;
            constexpr float radius = 1.0f;

            slicer::SdfGrid grid;
            grid.width = resolution;
            grid.height = resolution;
            grid.clippingArea = {min, min, max, max};
            grid.values.reserve(static_cast<std::size_t>(resolution * resolution));

            float const cellSize = (max - min) / static_cast<float>(resolution - 1);
            for (int y = 0; y < resolution; ++y)
            {
                for (int x = 0; x < resolution; ++x)
                {
                    float const px = min + static_cast<float>(x) * cellSize;
                    float const py = min + static_cast<float>(y) * cellSize;
                    grid.values.push_back(std::sqrt(px * px + py * py) - radius);
                }
            }
            return grid;
        }
    }

    TEST(AdaptiveContourDefaults, DefaultConstruction_EnablesAdaptiveExtraction)
    {
        nodes::SliceParameter const sliceParameter;
        webgpu::ContourGridRequest const gridRequest;

        EXPECT_TRUE(sliceParameter.useAdaptiveContour);
        EXPECT_TRUE(gridRequest.useAdaptiveContour);
    }

    TEST(AdaptiveContourGrid, ModelBounds_ProducesCanonicalPaddedLattice)
    {
        auto const definition =
          slicer::makeAdaptiveContourGrid({0.0f, 0.0f, 10.0f, 20.0f});

        EXPECT_FLOAT_EQ(definition.clippingArea.x, -2.0f);
        EXPECT_FLOAT_EQ(definition.clippingArea.y, -2.0f);
        EXPECT_FLOAT_EQ(definition.clippingArea.z, 12.0f);
        EXPECT_FLOAT_EQ(definition.clippingArea.w, 22.0f);
        EXPECT_EQ(definition.width, 141);
        EXPECT_EQ(definition.height, 241);
    }

    TEST(AdaptiveContourGrid, FractionalExtent_DoesNotExceedTargetCellSize)
    {
        auto const definition =
          slicer::makeAdaptiveContourGrid({0.0f, 0.0f, 10.01f, 20.01f});

        float const cellSizeX = (definition.clippingArea.z - definition.clippingArea.x) /
                                static_cast<float>(definition.width - 1);
        float const cellSizeY = (definition.clippingArea.w - definition.clippingArea.y) /
                                static_cast<float>(definition.height - 1);
        EXPECT_LE(cellSizeX, 0.1f);
        EXPECT_LE(cellSizeY, 0.1f);
    }

    TEST(ExtractAdaptiveContours, CircleGrid_ProducesManufacturingReadyContour)
    {
        auto const logger = std::make_shared<events::Logger>(events::OutputMode::Silent);

        auto const contours =
          slicer::GridContourBuilder::extractAdaptiveContours(makeCircleGrid(), 0.05f, logger);

        ASSERT_EQ(contours.size(), 1u);
        EXPECT_TRUE(contours.front().isClosed);
        EXPECT_EQ(contours.front().contourMode, ContourMode::Outer);
        EXPECT_FALSE(contours.front().hasIntersections);
        EXPECT_GT(contours.front().area, 0.0f);
    }

    TEST(ExtractAdaptiveContours, IdenticalGrid_ProducesBitExactResult)
    {
        auto const logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        auto const grid = makeCircleGrid();

        auto const first =
          slicer::GridContourBuilder::extractAdaptiveContours(grid, 0.05f, logger);
        auto const second =
          slicer::GridContourBuilder::extractAdaptiveContours(grid, 0.05f, logger);

        ASSERT_EQ(first.size(), second.size());
        for (std::size_t contourIndex = 0u; contourIndex < first.size(); ++contourIndex)
        {
            auto const & firstContour = first[contourIndex];
            auto const & secondContour = second[contourIndex];
            EXPECT_EQ(firstContour.isClosed, secondContour.isClosed);
            EXPECT_EQ(firstContour.contourMode, secondContour.contourMode);
            EXPECT_FLOAT_EQ(firstContour.area, secondContour.area);
            ASSERT_EQ(firstContour.vertices.size(), secondContour.vertices.size());
            for (std::size_t vertexIndex = 0u; vertexIndex < firstContour.vertices.size();
                 ++vertexIndex)
            {
                EXPECT_FLOAT_EQ(firstContour.vertices[vertexIndex].x(),
                                secondContour.vertices[vertexIndex].x());
                EXPECT_FLOAT_EQ(firstContour.vertices[vertexIndex].y(),
                                secondContour.vertices[vertexIndex].y());
            }
        }
    }

    TEST(ExtractAdaptiveContours, IncompleteGrid_ThrowsInvalidArgument)
    {
        auto const logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        slicer::SdfGrid grid;
        grid.width = 4;
        grid.height = 4;
        grid.clippingArea = {-1.0f, -1.0f, 1.0f, 1.0f};
        grid.values.resize(15u);

        EXPECT_THROW(
          {
              auto const contours =
                slicer::GridContourBuilder::extractAdaptiveContours(grid, 0.2f, logger);
              (void)contours;
          },
          std::invalid_argument);
    }
}