#include <gtest/gtest.h>

#include "io/3mf/SurfaceThicknessField.h"

namespace gladius::io::tests
{
    /// Helper to create a simple uniform LUT (all entries have the same thickness)
    std::vector<float> createUniformLut(int resolution, float thickness)
    {
        auto const size = static_cast<std::size_t>(resolution) * resolution * resolution;
        return std::vector<float>(size, thickness);
    }

    /// Helper to create a gradient LUT based on red channel
    std::vector<float> createRedGradientLut(int resolution, float minThickness, float maxThickness)
    {
        auto const size = static_cast<std::size_t>(resolution) * resolution * resolution;
        std::vector<float> lut(size);

        for (int r = 0; r < resolution; ++r)
        {
            float const t = static_cast<float>(r) / (resolution - 1);
            float const thickness = minThickness + t * (maxThickness - minThickness);
            for (int g = 0; g < resolution; ++g)
            {
                for (int b = 0; b < resolution; ++b)
                {
                    auto const idx = (static_cast<std::size_t>(r) * resolution + g) * resolution + b;
                    lut[idx] = thickness;
                }
            }
        }
        return lut;
    }

    /// Helper to create a BoundingBox from min/max points
    BoundingBox createBoundingBox(Eigen::Vector3f const& minPt, Eigen::Vector3f const& maxPt)
    {
        BoundingBox bounds;
        bounds.min = {minPt.x(), minPt.y(), minPt.z(), 0.0f};
        bounds.max = {maxPt.x(), maxPt.y(), maxPt.z(), 0.0f};
        return bounds;
    }

    // ============================================================================
    // T013: Build_WithValidInput_Succeeds
    // ============================================================================
    TEST(SurfaceThicknessField_Test, Build_WithValidInput_Succeeds)
    {
        std::vector<Eigen::Vector3f> vertices = {
            {0.0f, 0.0f, 0.0f},
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
        };

        std::vector<Eigen::Vector3f> colors = {
            {1.0f, 0.0f, 0.0f}, // Red
            {0.0f, 1.0f, 0.0f}, // Green
            {0.0f, 0.0f, 1.0f}, // Blue
        };

        int const lutRes = 4;
        auto const lut = createUniformLut(lutRes, 1.0f);

        BoundingBox const bounds = createBoundingBox({-0.5f, -0.5f, -0.5f}, {1.5f, 1.5f, 0.5f});

        SurfaceThicknessField field;
        EXPECT_NO_THROW(field.build(vertices, colors, lut, lutRes, bounds));
        EXPECT_TRUE(field.isBuilt());
        EXPECT_EQ(field.getResolution(), 128); // Default resolution
    }

    // ============================================================================
    // T014: Build_WithMismatchedArrays_Throws
    // ============================================================================
    TEST(SurfaceThicknessField_Test, Build_WithMismatchedArrays_Throws)
    {
        std::vector<Eigen::Vector3f> vertices(3);
        std::vector<Eigen::Vector3f> colors(5); // Mismatch!

        int const lutRes = 4;
        auto const lut = createUniformLut(lutRes, 1.0f);

        BoundingBox const bounds = createBoundingBox({-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f});

        SurfaceThicknessField field;
        EXPECT_THROW(field.build(vertices, colors, lut, lutRes, bounds), std::runtime_error);
    }

    // ============================================================================
    // T015: SampleAt_ReturnsInterpolatedValues
    // ============================================================================
    TEST(SurfaceThicknessField_Test, SampleAt_ReturnsInterpolatedValues)
    {
        // Create a simple 2-vertex case with different thicknesses based on color
        std::vector<Eigen::Vector3f> vertices = {
            {0.0f, 0.0f, 0.0f},
            {1.0f, 0.0f, 0.0f},
        };

        // Red = high thickness, black = low thickness
        std::vector<Eigen::Vector3f> colors = {
            {0.0f, 0.0f, 0.0f}, // Black -> 0.0 thickness
            {1.0f, 0.0f, 0.0f}, // Red -> 1.0 thickness
        };

        int const lutRes = 4;
        auto const lut = createRedGradientLut(lutRes, 0.0f, 1.0f);

        BoundingBox const bounds = createBoundingBox({-0.5f, -0.5f, -0.5f}, {1.5f, 0.5f, 0.5f});

        SurfaceThicknessFieldConfig config;
        config.gridResolution = 32; // Smaller for faster test

        SurfaceThicknessField field;
        field.build(vertices, colors, lut, lutRes, bounds, config);
        EXPECT_TRUE(field.isBuilt());

        // Sample at the first vertex position - should be close to 0.0 (black)
        float const thickness0 = field.sampleAt(vertices[0]);

        // Sample at the second vertex position - should be close to 1.0 (red)
        float const thickness1 = field.sampleAt(vertices[1]);

        // Verify gradient behavior
        EXPECT_LT(thickness0, thickness1);
    }

    // ============================================================================
    // T016: LookupThickness_TrilinearInterpolation
    // ============================================================================
    TEST(SurfaceThicknessField_Test, LookupThickness_TrilinearInterpolation)
    {
        // Use a gradient LUT where red channel maps to thickness
        int const lutRes = 4;
        auto const lut = createRedGradientLut(lutRes, 0.0f, 3.0f);

        // Create a single-vertex field to test LUT lookup
        std::vector<Eigen::Vector3f> vertices = {{0.5f, 0.5f, 0.5f}};

        // Test with 50% red - should give ~1.5 thickness (midpoint of 0-3)
        std::vector<Eigen::Vector3f> colors = {{0.5f, 0.0f, 0.0f}};

        BoundingBox const bounds = createBoundingBox({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});

        SurfaceThicknessFieldConfig config;
        config.gridResolution = 8;

        SurfaceThicknessField field;
        field.build(vertices, colors, lut, lutRes, bounds, config);
        EXPECT_TRUE(field.isBuilt());

        float const thickness = field.sampleAt(vertices[0]);
        // Should be approximately 1.5 (midpoint of 0-3 range)
        EXPECT_NEAR(thickness, 1.5f, 0.3f);
    }

    // ============================================================================
    // T017: PropagateInward_FillsUnassignedVoxels
    // ============================================================================
    TEST(SurfaceThicknessField_Test, PropagateInward_FillsUnassignedVoxels)
    {
        // Single vertex at center
        std::vector<Eigen::Vector3f> vertices = {{0.0f, 0.0f, 0.0f}};
        std::vector<Eigen::Vector3f> colors = {{1.0f, 0.0f, 0.0f}};

        int const lutRes = 4;
        auto const lut = createUniformLut(lutRes, 2.5f);

        BoundingBox const bounds = createBoundingBox({-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f});

        SurfaceThicknessFieldConfig config;
        config.gridResolution = 16;
        config.maxPropagationDistance = 16; // Fill entire grid from center

        SurfaceThicknessField field;
        field.build(vertices, colors, lut, lutRes, bounds, config);
        EXPECT_TRUE(field.isBuilt());

        // Sample at various points - should all have propagated value
        float const center = field.sampleAt({0.0f, 0.0f, 0.0f});
        float const nearby = field.sampleAt({0.2f, 0.2f, 0.2f});
        
        // Test propagation worked for nearby points
        EXPECT_NEAR(center, 2.5f, 0.1f);
        EXPECT_NEAR(nearby, 2.5f, 0.5f); // Slightly more tolerance for propagated values
    }

    // ============================================================================
    // T017a: ThicknessAccuracy_FlatSurface_Exceeds95Percent (validates SC-001)
    // ============================================================================
    TEST(SurfaceThicknessField_Test, ThicknessAccuracy_FlatSurface_Exceeds95Percent)
    {
        // Create a grid of vertices on a flat plane with known colors
        int const gridSize = 10;
        std::vector<Eigen::Vector3f> vertices;
        std::vector<Eigen::Vector3f> colors;
        std::vector<float> expectedThicknesses;

        int const lutRes = 8;
        auto const lut = createRedGradientLut(lutRes, 0.0f, 2.0f);

        // Create a flat grid of vertices with gradient colors
        for (int y = 0; y < gridSize; ++y)
        {
            for (int x = 0; x < gridSize; ++x)
            {
                float const fx = static_cast<float>(x) / (gridSize - 1);
                float const fy = static_cast<float>(y) / (gridSize - 1);

                vertices.push_back({fx, fy, 0.0f});
                colors.push_back({fx, 0.0f, 0.0f}); // Red gradient in X direction
                expectedThicknesses.push_back(fx * 2.0f); // Expected thickness: 0 to 2
            }
        }

        BoundingBox const bounds = createBoundingBox({-0.1f, -0.1f, -0.1f}, {1.1f, 1.1f, 0.1f});

        SurfaceThicknessFieldConfig config;
        config.gridResolution = 64;

        SurfaceThicknessField field;
        field.build(vertices, colors, lut, lutRes, bounds, config);
        EXPECT_TRUE(field.isBuilt());

        // Measure accuracy
        int matchCount = 0;
        float const tolerance = 0.2f; // 10% of max thickness range

        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            float const sampled = field.sampleAt(vertices[i]);
            if (std::abs(sampled - expectedThicknesses[i]) <= tolerance)
            {
                ++matchCount;
            }
        }

        float const accuracy = static_cast<float>(matchCount) / vertices.size();
        EXPECT_GE(accuracy, 0.95f) << "Accuracy: " << (accuracy * 100.0f) << "%, expected >= 95%";
    }

    // ============================================================================
    // Additional edge case tests
    // ============================================================================
    TEST(SurfaceThicknessField_Test, Build_WithEmptyVertices_DoesNotCrash)
    {
        std::vector<Eigen::Vector3f> vertices;
        std::vector<Eigen::Vector3f> colors;

        int const lutRes = 4;
        auto const lut = createUniformLut(lutRes, 1.0f);

        BoundingBox const bounds = createBoundingBox({-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f});

        SurfaceThicknessField field;
        EXPECT_NO_THROW(field.build(vertices, colors, lut, lutRes, bounds));
        EXPECT_FALSE(field.isBuilt()); // Empty input means not built
    }

    TEST(SurfaceThicknessField_Test, Build_WithInvalidLutSize_Throws)
    {
        std::vector<Eigen::Vector3f> vertices = {{0.0f, 0.0f, 0.0f}};
        std::vector<Eigen::Vector3f> colors = {{1.0f, 0.0f, 0.0f}};

        int const lutRes = 4;
        std::vector<float> lut(10); // Wrong size! Should be 64 (4³)

        BoundingBox const bounds = createBoundingBox({-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f});

        SurfaceThicknessField field;
        EXPECT_THROW(field.build(vertices, colors, lut, lutRes, bounds), std::runtime_error);
    }

    TEST(SurfaceThicknessField_Test, GetMemoryUsage_ReturnsReasonableValue)
    {
        std::vector<Eigen::Vector3f> vertices = {{0.0f, 0.0f, 0.0f}};
        std::vector<Eigen::Vector3f> colors = {{1.0f, 0.0f, 0.0f}};

        int const lutRes = 4;
        auto const lut = createUniformLut(lutRes, 1.0f);

        BoundingBox const bounds = createBoundingBox({-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f});

        SurfaceThicknessFieldConfig config;
        config.gridResolution = 32; // 32³ = 32768 voxels

        SurfaceThicknessField field;
        field.build(vertices, colors, lut, lutRes, bounds, config);

        std::size_t const expectedFloats = 32 * 32 * 32 * sizeof(float);  // ~128KB
        std::size_t const expectedBools = 32 * 32 * 32 * sizeof(bool);    // ~32KB

        EXPECT_GE(field.getMemoryUsage(), expectedFloats);
        EXPECT_LE(field.getMemoryUsage(), expectedFloats + expectedBools + 1024); // Some margin
    }

} // namespace gladius::io::tests
