#if defined(GLADIUS_ENABLE_OPENCL) && defined(GLADIUS_ENABLE_WEBGPU)

#include "BBox.h"
#include "ComputeContext.h"
#include "Document.h"
#include "EventLogger.h"
#include "ResourceManager.h"
#include "SpatialMeshResource.h"
#include "compute/AnalyticRenderSceneSnapshotFactory.h"
#include "compute/ComputeCore.h"
#include "compute/ManifoldDualContouringProgram.h"
#include "webgpu/WebGPUSdfEvaluator.h"
#include "webgpu/WebGPUSdfShaderComposer.h"
#include "nodes/Assembly.h"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gladius::tests
{
    namespace
    {
        struct GyroidFixture
        {
            const char * name;
            const char * path;
        };

                constexpr GyroidFixture GYROID_FIXTURES[] = {
                    {"SimpleGyroid", "testdata/SimpleGyroid.3mf"}};

        [[nodiscard]] bool gpuTestsEnabled()
        {
            return std::getenv("GLADIUS_SKIP_GPU_TESTS") == nullptr &&
                   std::getenv("GLADIUS_RUN_GPU_TESTS") != nullptr &&
                   std::getenv("GLADIUS_RUN_WEBGPU_TESTS") != nullptr;
        }

        [[nodiscard]] std::vector<Eigen::Vector3f> createTestPoints(BoundingBox const & bbox)
        {
            Eigen::Vector3f const minimum{bbox.min.x, bbox.min.y, bbox.min.z};
            Eigen::Vector3f const maximum{bbox.max.x, bbox.max.y, bbox.max.z};
            auto const center = (minimum + maximum) * 0.5f;
            auto const extent = maximum - minimum;
            std::vector<Eigen::Vector3f> points;
            points.reserve(40u);
            points.push_back(minimum);
            points.push_back(maximum);
            points.push_back(center);
            points.push_back(minimum + extent * 0.1f);
            points.push_back(minimum + extent * 0.25f);
            points.push_back(minimum + extent * 0.75f);
            points.push_back(minimum + extent * 0.9f);

            for (int z = 0; z < 3; ++z)
            {
                for (int y = 0; y < 3; ++y)
                {
                    for (int x = 0; x < 3; ++x)
                    {
                        Eigen::Vector3f const fraction{0.125f + 0.375f * static_cast<float>(x),
                                                       0.125f + 0.375f * static_cast<float>(y),
                                                       0.125f + 0.375f * static_cast<float>(z)};
                        points.push_back(minimum + extent.cwiseProduct(fraction));
                    }
                }
            }

            points.push_back(center + Eigen::Vector3f{extent.x() * 0.03125f, 0.0f, 0.0f});
            points.push_back(center - Eigen::Vector3f{0.0f, extent.y() * 0.03125f, 0.0f});
            points.push_back(center + Eigen::Vector3f{0.0f, 0.0f, extent.z() * 0.03125f});
            return points;
        }

        [[nodiscard]] std::vector<std::array<float, 3>> toWebGpuPoints(
          std::vector<Eigen::Vector3f> const & points)
        {
            std::vector<std::array<float, 3>> result;
            result.reserve(points.size());
            for (auto const & point : points)
            {
                result.push_back({point.x(), point.y(), point.z()});
            }
            return result;
        }

        void assertSdfValuesMatch(std::string const & fixture,
                                  std::vector<Eigen::Vector3f> const & points,
                                  std::vector<float> const & openClValues,
                                  std::vector<float> const & webGpuValues)
        {
            ASSERT_EQ(openClValues.size(), points.size());
            ASSERT_EQ(webGpuValues.size(), points.size());

            constexpr float absoluteTolerance = 2.0e-4f;
            constexpr float relativeTolerance = 1.0e-5f;
            for (std::size_t index = 0u; index < points.size(); ++index)
            {
                ASSERT_TRUE(std::isfinite(openClValues[index])) << fixture << " OpenCL value at " << index;
                ASSERT_TRUE(std::isfinite(webGpuValues[index])) << fixture << " WebGPU value at " << index;

                auto const error = std::abs(openClValues[index] - webGpuValues[index]);
                auto const tolerance = absoluteTolerance + relativeTolerance * std::abs(openClValues[index]);
                EXPECT_LE(error, tolerance)
                  << fixture << " point " << index << " (" << points[index].x() << ", "
                  << points[index].y() << ", " << points[index].z() << "): OpenCL="
                  << openClValues[index] << ", WebGPU=" << webGpuValues[index]
                  << ", error=" << error << ", tolerance=" << tolerance;
            }
        }
    }

    class SdfBackendParityTest : public ::testing::TestWithParam<GyroidFixture>
    {
      protected:
        void SetUp() override
        {
            if (!gpuTestsEnabled())
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

        std::shared_ptr<ComputeContext> m_context;
        std::shared_ptr<events::Logger> m_logger;
    };

    TEST_P(SdfBackendParityTest, AnalyticGyroidSdfValuesMatchAcrossBackends)
    {
        auto const fixture = GetParam();
        auto document = std::make_shared<Document>(
          std::make_shared<ComputeCore>(m_context, RequiredCapabilities::ComputeOnly, m_logger));
        ASSERT_TRUE(std::filesystem::exists(fixture.path)) << fixture.path;
        ASSERT_NO_THROW(document->load(fixture.path));
        ASSERT_NO_THROW(document->refreshModelBlocking());

        auto const assembly = document->getAssembly();
        auto const flatAssembly = document->getFlatAssembly();
        ASSERT_NE(assembly, nullptr);
        ASSERT_NE(assembly->assemblyModel(), nullptr);
        ASSERT_NE(flatAssembly, nullptr);
        ASSERT_NE(flatAssembly->assemblyModel(), nullptr);
        auto const core = document->getCore();
        ASSERT_NE(core, nullptr);
        ASSERT_TRUE(core->updateParameterBlocking(*assembly));
        ASSERT_TRUE(core->updateBBox());

        std::size_t spatialMeshCount = 0u;
        for (auto const & [key, resource] : document->getResourceManager().getResourceMap())
        {
            (void)key;
            if (dynamic_cast<SpatialMeshResource *>(resource.get()) != nullptr)
            {
                ++spatialMeshCount;
            }
        }
        ASSERT_EQ(spatialMeshCount, 0u) << fixture.name << " must remain an analytic/no-mesh fixture";

        auto const bbox = core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());
        auto const points = createTestPoints(*bbox);

        auto const openClProgram = core->getProgramManager().getManifoldDualContouringProgram();
        ASSERT_NE(openClProgram, nullptr);
        auto const primitives = core->getPrimitives();
        ASSERT_NE(primitives, nullptr);
        auto const openClValues = openClProgram->evaluateSdfBatch(points, *primitives, 0.0f);

        auto const snapshot = compute::AnalyticRenderSceneSnapshotFactory::create(*flatAssembly, 1u);
        ASSERT_TRUE(snapshot.isValid());
        std::unique_ptr<webgpu::WebGPUSdfEvaluator> webGpuEvaluator;
        try
        {
            webGpuEvaluator = std::make_unique<webgpu::WebGPUSdfEvaluator>();
        }
        catch (std::exception const & exception)
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << exception.what();
        }
        if (!webGpuEvaluator->isAvailable())
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << webGpuEvaluator->getErrorMessage();
        }

        auto const request = compute::SdfEvaluationRequest{
          .positions = toWebGpuPoints(points),
          .isoValue = 0.0f,
          .shaderSource = webgpu::WebGPUSdfShaderComposer::compose(snapshot.analyticEvaluatorWgsl),
          .parameterValues = snapshot.parameterValues};
        auto const webGpuValues = webGpuEvaluator->evaluate(request).values;

        assertSdfValuesMatch(fixture.name, points, openClValues, webGpuValues);
    }

    INSTANTIATE_TEST_SUITE_P(GyroidFixtures,
                             SdfBackendParityTest,
                             ::testing::ValuesIn(GYROID_FIXTURES),
                             [](auto const & info)
                             { return info.param.name; });
}

#endif
