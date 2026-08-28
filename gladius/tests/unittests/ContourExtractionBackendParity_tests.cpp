/// @file ContourExtractionBackendParity_tests.cpp
/// @brief Compares contour extraction between the OpenCL and WebGPU backends.
///
/// Both backends sample the same analytic model on an identical NxM grid at a fixed
/// z height. The WebGPU side evaluates via WebGPUSdfEvaluator; the OpenCL side uses
/// the ManifoldDualContouringProgram batch evaluator. The resulting SDF grids are
/// compared pointwise (stage A) and fed into the shared GridContourBuilder host code
/// (dense marching squares and adaptive quadtree) whose polylines are compared by
/// topology and geometry budgets (stage B).

#include "ComputeContext.h"
#include "EventLogger.h"
#include "compute/ComputeCore.h"
#include "compute/ManifoldDualContouringProgram.h"
#include "slicer/GridContourBuilder.h"
#include "webgpu/WebGPUContourGenerator.h"
#include "webgpu/WebGPUSdfShaderComposer.h"

#include <gtest/gtest.h>

#include <Eigen/Core>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#if defined(GLADIUS_ENABLE_OPENCL) && defined(GLADIUS_ENABLE_WEBGPU)

namespace gladius::tests
{
    namespace
    {
        constexpr char const * c_sphereKernel = R"CLC(
    float4 model(float3 pos, PAYLOAD_ARGS)
    {
        float const distance = length(pos - (float3)(1.0f, 1.0f, 1.0f)) - 0.6f;
        return (float4)(0.0f, 0.0f, 0.0f, distance);
    }
    )CLC";

        constexpr std::string_view c_sphereWgsl = R"WGSL(
fn evaluateModel(pos: vec3<f32>) -> vec4<f32> {
    let center = vec3<f32>(1.0, 1.0, 1.0);
    let d = distance(pos, center) - 0.6;
    return vec4<f32>(0.0, 0.0, 0.0, d);
}
)WGSL";

        constexpr char const * c_multiLoopKernel = R"CLC(
    float4 model(float3 pos, PAYLOAD_ARGS)
    {
        float const ring = fabs(length(pos.xy - (float2)(1.0f, 1.0f)) - 0.6f) - 0.15f;
        float const island = length(pos.xy - (float2)(2.2f, 1.0f)) - 0.2f;
        return (float4)(0.0f, 0.0f, 0.0f, fmin(ring, island));
    }
    )CLC";

        constexpr std::string_view c_multiLoopWgsl = R"WGSL(
fn evaluateModel(pos: vec3<f32>) -> vec4<f32> {
    let ring = abs(distance(pos.xy, vec2<f32>(1.0, 1.0)) - 0.6) - 0.15;
    let island = distance(pos.xy, vec2<f32>(2.2, 1.0)) - 0.2;
    return vec4<f32>(0.0, 0.0, 0.0, min(ring, island));
}
)WGSL";

        struct ContourStats
        {
            std::size_t closedCount{};
            std::size_t openCount{};
            float totalLength{};
            float enclosedArea{};
        };

        [[nodiscard]] ContourStats computeStats(PolyLines const & polylines)
        {
            ContourStats stats;
            for (auto const & poly : polylines)
            {
                if (poly.vertices.size() < 2)
                {
                    continue;
                }
                if (poly.isClosed)
                {
                    ++stats.closedCount;
                }
                else
                {
                    ++stats.openCount;
                }
                for (std::size_t i = 0u + 1u; i < poly.vertices.size(); ++i)
                {
                    stats.totalLength += (poly.vertices[i] - poly.vertices[i - 1u]).norm();
                }
                if (poly.isClosed)
                {
                    // Shoelace formula.
                    float area = 0.0f;
                    auto const n = poly.vertices.size();
                    for (std::size_t i = 0u; i < n; ++i)
                    {
                        auto const & a = poly.vertices[i];
                        auto const & b = poly.vertices[(i + 1u) % n];
                        area += a.x() * b.y() - b.x() * a.y();
                    }
                    stats.enclosedArea += std::abs(area) * 0.5f;
                }
            }
            return stats;
        }

        [[nodiscard]] slicer::SdfGrid makeGrid(std::vector<float> values,
                                               int width,
                                               int height,
                                               float4 clippingArea)
        {
            slicer::SdfGrid grid;
            grid.width = width;
            grid.height = height;
            grid.clippingArea = clippingArea;
            grid.values = std::move(values);
            return grid;
        }

        [[nodiscard]] float findBestClosedLoopAlignment(PolyLine const & reference,
                                                        PolyLine const & candidate)
        {
            if (reference.vertices.size() != candidate.vertices.size() ||
                reference.vertices.empty())
            {
                return std::numeric_limits<float>::infinity();
            }

            auto const vertexCount = reference.vertices.size();
            float bestMaximumDistance = std::numeric_limits<float>::infinity();
            for (std::size_t offset = 0u; offset < vertexCount; ++offset)
            {
                for (bool const reverse : {false, true})
                {
                    float maximumDistance = 0.0f;
                    for (std::size_t index = 0u; index < vertexCount; ++index)
                    {
                        auto const candidateIndex = reverse
                                                      ? (offset + vertexCount - index) % vertexCount
                                                      : (offset + index) % vertexCount;
                        maximumDistance =
                          std::max(maximumDistance,
                                   (reference.vertices[index] -
                                    candidate.vertices[candidateIndex])
                                     .norm());
                    }
                    bestMaximumDistance = std::min(bestMaximumDistance, maximumDistance);
                }
            }
            return bestMaximumDistance;
        }
    }

    class ContourExtractionBackendParityTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            if (std::getenv("GLADIUS_RUN_GPU_TESTS") == nullptr ||
                std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
            {
                GTEST_SKIP() << "GPU tests disabled; set GLADIUS_RUN_GPU_TESTS=1 and "
                                "GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
            }

            m_context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
            if (!m_context->isValid())
            {
                GTEST_SKIP() << "OpenCL context unavailable";
            }
            m_logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        }

        /// Sample the analytic sphere on the grid using the OpenCL batch evaluator.
        [[nodiscard]] std::optional<slicer::SdfGrid> renderGridOnOpenCl(
                    float4 clippingArea,
                    int width,
                    int height,
                    float zHeight_mm,
                    char const * modelKernel = c_sphereKernel) const
        {
            auto core =
              std::make_shared<ComputeCore>(m_context, RequiredCapabilities::ComputeOnly, m_logger);
            auto primitives = core->getPrimitives();

            gladius::compute::ManifoldDualContouringProgram sampler(m_context,
                                                                    core->getResourceContext());
            sampler.setLogger(m_logger);
            sampler.setModelKernel(modelKernel);

            auto & settings = core->getResourceContext()->getRenderingSettings();
            settings.meshInflationDistance = 0.0f;

            primitives->clear();
            primitives->write();

            auto const cellSizeX = (clippingArea.z - clippingArea.x) / static_cast<float>(width - 1);
            auto const cellSizeY = (clippingArea.w - clippingArea.y) / static_cast<float>(height - 1);

            std::vector<Eigen::Vector3f> points;
            points.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    float const px = clippingArea.x + cellSizeX * static_cast<float>(x);
                    float const py = clippingArea.y + cellSizeY * static_cast<float>(y);
                    points.emplace_back(px, py, zHeight_mm);
                }
            }

            auto values = sampler.evaluateSdfBatch(points, *primitives, 0.0f);
            if (values.size() != points.size())
            {
                return std::nullopt;
            }
            return makeGrid(std::move(values), width, height, clippingArea);
        }

        [[nodiscard]] std::optional<slicer::SdfGrid> renderGridOnWebGpu(
                    float4 clippingArea,
                    int width,
                    int height,
                    float zHeight_mm,
                    bool * skipped,
                    std::string_view evaluatorWgsl = c_sphereWgsl) const
        {
            *skipped = false;
            std::unique_ptr<webgpu::WebGPUContourGenerator> generator;
            try
            {
                generator = std::make_unique<webgpu::WebGPUContourGenerator>();
            }
            catch (std::exception const &)
            {
                *skipped = true;
                return std::nullopt;
            }
            if (!generator->isAvailable())
            {
                *skipped = true;
                return std::nullopt;
            }

            webgpu::ContourGridRequest gridRequest;
            gridRequest.zHeight_mm = zHeight_mm;
            gridRequest.clippingArea = clippingArea;
            gridRequest.width = width;
            gridRequest.height = height;

            compute::SdfEvaluationRequest baseRequest;
            baseRequest.isoValue = 0.0f;
            baseRequest.shaderSource =
                            webgpu::WebGPUSdfShaderComposer::compose(evaluatorWgsl);

            try
            {
                return generator->renderSdfGrid(baseRequest, gridRequest);
            }
            catch (std::exception const &)
            {
                *skipped = true;
                return std::nullopt;
            }
        }

        void compareGrids(slicer::SdfGrid const & openClGrid,
                          slicer::SdfGrid const & webGpuGrid) const
        {
            ASSERT_EQ(openClGrid.values.size(), webGpuGrid.values.size());

            constexpr float absoluteTolerance = 2.0e-4f;
            constexpr float relativeTolerance = 1.0e-4f;
            std::size_t signMismatches = 0u;
            float maxError = 0.0f;
            for (std::size_t index = 0u; index < openClGrid.values.size(); ++index)
            {
                ASSERT_TRUE(std::isfinite(openClGrid.values[index]));
                ASSERT_TRUE(std::isfinite(webGpuGrid.values[index]));

                if ((openClGrid.values[index] < 0.0f) != (webGpuGrid.values[index] < 0.0f))
                {
                    ++signMismatches;
                    continue;
                }
                auto const error = std::abs(openClGrid.values[index] - webGpuGrid.values[index]);
                maxError = std::max(maxError, error);
                auto const tolerance =
                  absoluteTolerance +
                  relativeTolerance * std::abs(openClGrid.values[index]);
                EXPECT_LE(error, tolerance) << "Sample mismatch at index " << index;
            }
            EXPECT_EQ(signMismatches, 0u)
              << "Sign mismatches between backends: " << signMismatches;
        }

        void compareContours(PolyLines const & openClContours,
                             PolyLines const & webGpuContours) const
        {
            auto const openClStats = computeStats(openClContours);
            auto const webGpuStats = computeStats(webGpuContours);

            EXPECT_GT(openClStats.closedCount, 0u) << "OpenCL produced no closed contours";
            EXPECT_EQ(openClStats.closedCount, webGpuStats.closedCount)
              << "Closed contour count mismatch: OpenCL=" << openClStats.closedCount
              << ", WebGPU=" << webGpuStats.closedCount;
                        EXPECT_EQ(openClStats.openCount, webGpuStats.openCount)
                            << "Open contour count mismatch: OpenCL=" << openClStats.openCount
                            << ", WebGPU=" << webGpuStats.openCount;

            // Geometry budget: relative agreement of length and enclosed area.
            ASSERT_GT(openClStats.totalLength, 0.0f);
            EXPECT_LT(std::abs(openClStats.totalLength - webGpuStats.totalLength) /
                        openClStats.totalLength,
                      0.05f)
              << "Total length mismatch: OpenCL=" << openClStats.totalLength
              << ", WebGPU=" << webGpuStats.totalLength;

            ASSERT_GT(openClStats.enclosedArea, 0.0f);
            EXPECT_LT(std::abs(openClStats.enclosedArea - webGpuStats.enclosedArea) /
                        openClStats.enclosedArea,
                      0.05f)
              << "Enclosed area mismatch: OpenCL=" << openClStats.enclosedArea
              << ", WebGPU=" << webGpuStats.enclosedArea;

            // Analytic sanity: circle radius 0.6 -> circumference ~2*pi*0.6, area ~pi*0.36.
            EXPECT_NEAR(webGpuStats.enclosedArea, 3.14159265f * 0.36f, 0.05f);
        }

        void compareAdaptiveContoursAtManufacturingPrecision(
          PolyLines const & openClContours, PolyLines const & webGpuContours) const
        {
            constexpr float vertexTolerance_mm = 1.0e-3f;

            ASSERT_EQ(openClContours.size(), webGpuContours.size());
            for (std::size_t index = 0u; index < openClContours.size(); ++index)
            {
                auto const & openClContour = openClContours[index];
                auto const & webGpuContour = webGpuContours[index];

                EXPECT_EQ(openClContour.isClosed, webGpuContour.isClosed);
                EXPECT_EQ(openClContour.contourMode, webGpuContour.contourMode);
                ASSERT_EQ(openClContour.vertices.size(), webGpuContour.vertices.size());
                EXPECT_NEAR(openClContour.area, webGpuContour.area, vertexTolerance_mm);
                EXPECT_LE(findBestClosedLoopAlignment(openClContour, webGpuContour),
                          vertexTolerance_mm);
            }
        }

        static constexpr float4 c_clipArea{0.0f, 0.0f, 2.0f, 2.0f};
        static constexpr float c_zHeight = 1.0f;

        std::shared_ptr<ComputeContext> m_context;
        std::shared_ptr<events::Logger> m_logger;
    };

    TEST_F(ContourExtractionBackendParityTest, SphereSlice_SdfGridMatchesAcrossBackends)
    {
        constexpr int resolution = 128;
        bool skipped = false;
        auto const openClGrid = renderGridOnOpenCl(c_clipArea, resolution, resolution, c_zHeight);
        ASSERT_TRUE(openClGrid.has_value());
        auto const webGpuGrid =
          renderGridOnWebGpu(c_clipArea, resolution, resolution, c_zHeight, &skipped);
        if (skipped)
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }
        ASSERT_TRUE(webGpuGrid.has_value());
        compareGrids(*openClGrid, *webGpuGrid);
    }

    TEST_F(ContourExtractionBackendParityTest, SphereSlice_DenseContoursMatchAcrossBackends)
    {
        constexpr int resolution = 128;
        bool skipped = false;
        auto const openClGrid = renderGridOnOpenCl(c_clipArea, resolution, resolution, c_zHeight);
        ASSERT_TRUE(openClGrid.has_value());
        auto const webGpuGrid =
          renderGridOnWebGpu(c_clipArea, resolution, resolution, c_zHeight, &skipped);
        if (skipped)
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }
        ASSERT_TRUE(webGpuGrid.has_value());

        auto const openClContours = slicer::GridContourBuilder::extractDenseContours(
          *openClGrid, *m_context, m_logger);
        auto const webGpuContours = slicer::GridContourBuilder::extractDenseContours(
          *webGpuGrid, *m_context, m_logger);
        compareContours(openClContours, webGpuContours);
    }

    TEST_F(ContourExtractionBackendParityTest, SphereSlice_AdaptiveContoursMatchAcrossBackends)
    {
                auto const gridDefinition =
                    slicer::makeAdaptiveContourGrid({0.4f, 0.4f, 1.6f, 1.6f});
        bool skipped = false;
                auto const openClGrid = renderGridOnOpenCl(gridDefinition.clippingArea,
                                                                                                     gridDefinition.width,
                                                                                                     gridDefinition.height,
                                                                                                     c_zHeight);
        ASSERT_TRUE(openClGrid.has_value());
                auto const webGpuGrid = renderGridOnWebGpu(gridDefinition.clippingArea,
                                                                                                     gridDefinition.width,
                                                                                                     gridDefinition.height,
                                                                                                     c_zHeight,
                                                                                                     &skipped);
        if (skipped)
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }
        ASSERT_TRUE(webGpuGrid.has_value());

        constexpr float minFeatureSize = 0.2f;
        auto const openClContours = slicer::GridContourBuilder::extractAdaptiveContours(
          *openClGrid, minFeatureSize, m_logger);
        auto const webGpuContours = slicer::GridContourBuilder::extractAdaptiveContours(
          *webGpuGrid, minFeatureSize, m_logger);
        compareContours(openClContours, webGpuContours);
                compareAdaptiveContoursAtManufacturingPrecision(openClContours, webGpuContours);
    }

        TEST_F(ContourExtractionBackendParityTest,
                     MultiLoopSlice_AdaptiveContoursMatchAcrossBackends)
        {
                constexpr float4 clippingArea{0.0f, 0.0f, 2.6f, 2.0f};
                constexpr int resolution = 192;
                bool skipped = false;
                auto const openClGrid = renderGridOnOpenCl(
                    clippingArea, resolution, resolution, c_zHeight, c_multiLoopKernel);
                ASSERT_TRUE(openClGrid.has_value());
                auto const webGpuGrid = renderGridOnWebGpu(
                    clippingArea, resolution, resolution, c_zHeight, &skipped, c_multiLoopWgsl);
                if (skipped)
        {
                        GTEST_SKIP() << "WebGPU device unavailable";
        }
                ASSERT_TRUE(webGpuGrid.has_value());
                compareGrids(*openClGrid, *webGpuGrid);

                constexpr float minFeatureSize = 0.1f;
                auto const openClContours = slicer::GridContourBuilder::extractAdaptiveContours(
                    *openClGrid, minFeatureSize, m_logger);
                auto const webGpuContours = slicer::GridContourBuilder::extractAdaptiveContours(
                    *webGpuGrid, minFeatureSize, m_logger);

                ASSERT_EQ(openClContours.size(), 3u);
                EXPECT_EQ(std::count_if(openClContours.begin(),
                                                                openClContours.end(),
                                                                [](PolyLine const & contour)
                                                                { return contour.contourMode == ContourMode::Outer; }),
                                    2);
                EXPECT_EQ(std::count_if(openClContours.begin(),
                                                                openClContours.end(),
                                                                [](PolyLine const & contour)
                                                                { return contour.contourMode == ContourMode::Inner; }),
                                    1);
                EXPECT_TRUE(std::all_of(openClContours.begin(),
                                                                openClContours.end(),
                                                                [](PolyLine const & contour) { return contour.isClosed; }));
                compareAdaptiveContoursAtManufacturingPrecision(openClContours, webGpuContours);
        }

    TEST_F(ContourExtractionBackendParityTest,
           WebGPUContourGenerator_FullPipeline_ProducesSphereContour)
    {
        std::unique_ptr<webgpu::WebGPUContourGenerator> generator;
        try
        {
            generator = std::make_unique<webgpu::WebGPUContourGenerator>();
        }
        catch (std::exception const &)
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }
        if (!generator->isAvailable())
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }

        webgpu::ContourGridRequest gridRequest;
        gridRequest.zHeight_mm = c_zHeight;
        gridRequest.clippingArea = c_clipArea;
        gridRequest.width = 128;
        gridRequest.height = 128;
        gridRequest.minFeatureSize_mm = 0.2f;
        gridRequest.useAdaptiveContour = true;

        compute::SdfEvaluationRequest baseRequest;
        baseRequest.isoValue = 0.0f;
        baseRequest.shaderSource = webgpu::WebGPUSdfShaderComposer::compose(c_sphereWgsl);

        PolyLines contours;
        ASSERT_NO_THROW(
          { contours = generator->generateAdaptiveContours(baseRequest, gridRequest, m_logger); });

        auto const stats = computeStats(contours);
        EXPECT_EQ(stats.closedCount, 1u);
        EXPECT_NEAR(stats.enclosedArea, 3.14159265f * 0.36f, 0.05f);
    }
}

#endif // GLADIUS_ENABLE_OPENCL && GLADIUS_ENABLE_WEBGPU
