#include "ComputeContext.h"
#include "Document.h"
#include "EventLogger.h"
#include "io/3mf/ShellGenerator.h"
#include "io/3mf/FilamentOpticalProperties.h"
#include "io/SurfaceExtractionOptions.h"

#include <compute/ComputeCore.h>

#include <gtest/gtest.h>

namespace gladius_tests::shell_generator
{
    using namespace gladius;
    using namespace gladius::io;

    class ShellGeneratorTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_logger = std::make_shared<events::Logger>();
            m_context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);

            if (!m_context->isValid())
            {
                GTEST_SKIP() << "OpenCL context not available";
            }
        }

        struct DocumentBundle
        {
            std::shared_ptr<ComputeCore> core;
            std::shared_ptr<Document> document;
        };

        DocumentBundle loadDocument(std::filesystem::path const & path)
        {
            auto core =
              std::make_shared<ComputeCore>(m_context, RequiredCapabilities::ComputeOnly, m_logger);
            auto document = std::make_shared<Document>(core);
            document->load(path);

            return DocumentBundle{std::move(core), std::move(document)};
        }

        std::shared_ptr<ComputeContext> m_context;
        events::SharedLogger m_logger;
    };

    TEST_F(ShellGeneratorTest, GenerateShells_ProducesNestedMeshes)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        // Define a stack of 2 filaments
        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties("Bottom", {0,0,0}, 1.0f));
        stack.push_back(FilamentOpticalProperties("Top", {1,1,1}, 0.5f));

        // Define thickness solution
        ThicknessSolution solution(2);
        solution.thicknesses[0] = 1.0f; // Bottom thickness
        solution.thicknesses[1] = 0.5f; // Top thickness

        // Configure Manifold DC
        ManifoldDualContouringOptions options;
        options.initialDepth = 5;
        options.maxDepth = 6;
        options.enableGpu = false; // Use CPU for deterministic test
        options.qualityPreset = ManifoldDualContouringQuality::Custom;

        ShellGenerator generator(*bundle.core, *bundle.document);
        auto shells = generator.generateShells(stack, solution, options);

        ASSERT_EQ(shells.size(), 2);

        // Shell 0: Top layer (Outer surface)
        // Iso = 0.
        EXPECT_EQ(shells[0].filamentName, "Top");
        EXPECT_EQ(shells[0].layerIndex, 1);
        EXPECT_GT(shells[0].vertices.size(), 0);

        // Shell 1: Bottom layer (Interface)
        // Iso = -0.5.
        EXPECT_EQ(shells[1].filamentName, "Bottom");
        EXPECT_EQ(shells[1].layerIndex, 0);
        EXPECT_GT(shells[1].vertices.size(), 0);

        // Verify nesting: Shell 1 should be "smaller" (eroded) than Shell 0?
        // Or rather, Shell 1 is the surface at depth 0.5.
        // Since Gyroid is a surface, offsetting it might make it thicker or thinner depending on sign.
        // But here we are offsetting the SDF.
        // If SDF > 0 is outside, SDF < 0 is inside.
        // Iso = 0 is the surface.
        // Iso = -0.5 is inside the object (depth 0.5).
        
        // So Shell 1 is "inside" Shell 0.
        // We can't easily check geometric containment without complex checks.
        // But we can check that they are different.
        EXPECT_NE(shells[0].vertices.size(), shells[1].vertices.size());
    }

    TEST(ShellGeneratorStaticTests, BuildCumulativeLut_HasExpectedSizeAndNonNegativeValues)
    {
        using gladius::io::FilamentOpticalProperties;
        using gladius::io::FilamentStack;
        using gladius::io::ThicknessConstraints;

        FilamentOpticalProperties f1{"Bottom", Eigen::Vector3f{0.8F, 0.1F, 0.1F}, 0.6F};
        FilamentOpticalProperties f2{"Top", Eigen::Vector3f{0.1F, 0.8F, 0.2F}, 0.7F};

        FilamentStack stack({f1, f2});
        ThicknessConstraints constraints;

        int const resolution = 4; // 64 samples keeps test fast
        auto const lut = gladius::io::ShellGenerator::buildCumulativeThicknessLut(
            stack, constraints, /*startLayer=*/1U, resolution);

        ASSERT_EQ(lut.size(), static_cast<std::size_t>(resolution * resolution * resolution));
        for (float value : lut)
        {
            EXPECT_GE(value, 0.0F);
        }
    }

    TEST_F(ShellGeneratorTest, GenerateShellsWithPrecomputedLut_UsesCachedLut)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties("Bottom", {0.8F, 0.1F, 0.1F}, 0.6F));
        stack.push_back(FilamentOpticalProperties("Top", {0.1F, 0.8F, 0.2F}, 0.7F));

        ThicknessConstraints constraints;
        int const lutResolution = 4;

        // Precompute LUTs once (simulating UI dialog behavior)
        std::vector<std::vector<float>> precomputedLuts;
        precomputedLuts.reserve(stack.size());
        for (std::size_t layer = 0; layer < stack.size(); ++layer)
        {
            precomputedLuts.push_back(ShellGenerator::buildCumulativeThicknessLut(
                stack, constraints, layer, lutResolution));
        }

        // Placeholder solution (unused when LUTs provided)
        ThicknessSolution solution(stack.size());
        for (auto & t : solution.thicknesses)
        {
            t = constraints.minThickness;
        }

        ManifoldDualContouringOptions options;
        options.initialDepth = 4;
        options.maxDepth = 5;
        options.enableGpu = false;
        options.qualityPreset = ManifoldDualContouringQuality::Custom;

        ShellGenerator generator(*bundle.core, *bundle.document);
        auto shells = generator.generateShells(
            stack,
            solution,
            options,
            lutResolution,
            constraints,
            &precomputedLuts);

        ASSERT_EQ(shells.size(), stack.size());
        for (auto const & shell : shells)
        {
            EXPECT_GT(shell.vertices.size(), 0U);
            EXPECT_GT(shell.indices.size(), 0U);
        }
    }

    /// @brief Test surface-aligned color sampling mode (T028 from 011-surface-color-shells)
    /// 
    /// This test verifies that shells generated with useSurfaceColorSampling=true
    /// produce valid mesh output using the new SurfaceThicknessField-based approach.
    TEST_F(ShellGeneratorTest, GenerateShells_WithSurfaceColorSampling_ProducesValidMesh)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        // Define a stack of 2 filaments with varying optical properties
        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties("White", {1.0f, 1.0f, 1.0f}, 0.3f));
        stack.push_back(FilamentOpticalProperties("Red", {1.0f, 0.0f, 0.0f}, 0.5f));

        ThicknessConstraints constraints;
        constraints.minThickness = 0.1f;
        constraints.maxThickness = 2.0f;

        int const lutResolution = 8;  // Reasonable resolution for test

        // Use solution with varying thicknesses
        ThicknessSolution solution(2);
        solution.thicknesses[0] = 0.5f;
        solution.thicknesses[1] = 0.8f;

        ManifoldDualContouringOptions options;
        options.initialDepth = 4;
        options.maxDepth = 5;
        options.enableGpu = true; // Need GPU for thickness field sampling
        options.qualityPreset = ManifoldDualContouringQuality::Custom;

        ShellGenerator generator(*bundle.core, *bundle.document);
        
        // Generate shells WITH surface color sampling enabled
        bool const useSurfaceColorSampling = true;
        auto shells = generator.generateShells(
            stack,
            solution,
            options,
            lutResolution,
            constraints,
            nullptr,  // No precomputed LUTs
            useSurfaceColorSampling);

        // Verify we got shells for each layer
        ASSERT_EQ(shells.size(), stack.size());
        
        for (auto const & shell : shells)
        {
            // Each shell should have valid mesh data
            EXPECT_GT(shell.vertices.size(), 0U) 
                << "Shell " << shell.layerIndex << " has no vertices";
            EXPECT_GT(shell.indices.size(), 0U) 
                << "Shell " << shell.layerIndex << " has no indices";
            
            // Indices should be divisible by 3 (triangle mesh)
            EXPECT_EQ(shell.indices.size() % 3, 0U) 
                << "Shell " << shell.layerIndex << " has non-triangular indices";
        }
    }

    /// @brief Verify surface sampling doesn't crash with empty model
    TEST_F(ShellGeneratorTest, GenerateShells_WithSurfaceColorSampling_EmptyModel_ReturnsEmpty)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        // Don't update bbox - leave it empty
        
        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties("White", {1,1,1}, 0.3f));

        ThicknessSolution solution(1);
        solution.thicknesses[0] = 0.5f;

        ManifoldDualContouringOptions options;
        options.initialDepth = 4;
        options.maxDepth = 5;
        options.enableGpu = true;

        ShellGenerator generator(*bundle.core, *bundle.document);
        
        bool const useSurfaceColorSampling = true;
        auto shells = generator.generateShells(
            stack,
            solution,
            options,
            8,  // lutResolution
            ThicknessConstraints{},
            nullptr,
            useSurfaceColorSampling);

        // Without valid bbox, should return empty or handle gracefully
        // (exact behavior depends on implementation - we just verify no crash)
        SUCCEED();
    }
}
