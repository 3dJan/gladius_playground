/// @file MeshSdfBackendParity_tests.cpp
/// @brief Compares signed-distance values computed by the OpenCL and WebGPU
///        mesh SDF implementations on the same mesh payload.
///
/// The OpenCL side evaluates the full sdf.cl dispatch (pure-BVH path, matching
/// what SpatialMeshResource uploads), while the WebGPU side evaluates the WGSL
/// port (mesh_sdf.wgsl) through WebGPUSdfEvaluator. Both must agree on sign
/// and magnitude within a tight tolerance for points inside, outside and near
/// sharp features of the mesh.

#include "BBox.h"
#include "ComputeContext.h"
#include "EventLogger.h"
#include "MeshBVH.h"
#include "MeshPayloadSerializer.h"
#include "SpatialMeshResource.h"
#include "compute/ComputeCore.h"
#include "compute/ManifoldDualContouringProgram.h"
#include "webgpu/WebGPUSdfEvaluator.h"
#include "webgpu/WebGPUSdfShaderComposer.h"

#include <gtest/gtest.h>

#include <Eigen/Core>

#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#if defined(GLADIUS_ENABLE_OPENCL) && defined(GLADIUS_ENABLE_WEBGPU)

namespace gladius::tests
{
    namespace
    {
        void createCubeMesh(std::vector<float4> & vertices, std::vector<TriangleIndices> & indices)
        {
            vertices = {
              {-0.5f, -0.5f, -0.5f, 0.f}, {0.5f, -0.5f, -0.5f, 0.f},  {0.5f, 0.5f, -0.5f, 0.f},
              {-0.5f, 0.5f, -0.5f, 0.f},  {-0.5f, -0.5f, 0.5f, 0.f},  {0.5f, -0.5f, 0.5f, 0.f},
              {0.5f, 0.5f, 0.5f, 0.f},    {-0.5f, 0.5f, 0.5f, 0.f},
            };

            indices = {
              {4, 5, 6}, {4, 6, 7}, {1, 0, 3}, {1, 3, 2},
              {5, 1, 2}, {5, 2, 6}, {0, 4, 7}, {0, 7, 3},
              {7, 6, 2}, {7, 2, 3}, {0, 1, 5}, {0, 5, 4},
            };
        }

        [[nodiscard]] std::vector<Eigen::Vector3f> createTestPoints()
        {
            // Points exercising inside/outside and axis-aligned face proximity.
            return {
                {0.0f, 0.0f, 0.0f},     // deep inside
                {2.0f, 0.0f, 0.0f},     // outside
                {0.45f, 0.0f, 0.0f},    // near +X face, inside
                {0.55f, 0.0f, 0.0f},    // near +X face, outside
                {-0.55f, 0.0f, 0.0f},   // near -X face, outside
                {0.0f, 0.48f, 0.1f},    // near +Y face, inside
                {0.0f, -0.52f, 0.1f},   // near -Y face, outside
                {0.3f, 0.3f, 0.49f},    // near +Z face, inside
                {0.3f, 0.3f, 0.51f},    // near +Z face, outside
                {0.6f, 0.6f, 0.6f},     // diagonal outside near corner
            };
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
    }

    class MeshSdfBackendParityTest : public ::testing::Test
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

        [[nodiscard]] std::vector<float> evaluateOnOpenCl(
          std::vector<Eigen::Vector3f> const & points,
          SpatialMeshData meshData) const
        {
            auto core = std::make_shared<ComputeCore>(
              m_context, RequiredCapabilities::ComputeOnly, m_logger);
            auto primitives = core->getPrimitives();
            EXPECT_NE(primitives, nullptr);

            gladius::compute::ManifoldDualContouringProgram sampler(m_context,
                                                                    core->getResourceContext());
            sampler.setLogger(m_logger);
            sampler.setModelKernel(R"CLC(
    float4 model(float3 pos, PAYLOAD_ARGS)
    {
        float const distance = payload(pos, 0, primitivesSize, PASS_PAYLOAD_ARGS);
        return (float4)(0.0f, 0.0f, 0.0f, distance);
    }
    )CLC");

            // Pure BVH: no acceleration structures, matching the WebGPU payload ABI.
            MeshSdfEvaluationConfig cfg{};
            cfg.method = MeshSdfMethod::PureBVH;
            cfg.useEarlyExit = false;

            ResourceKey key(ResourceId{2000}, ResourceType::Mesh);
            SpatialMeshResource resource(key, std::move(meshData), cfg);

            auto & settings = core->getResourceContext()->getRenderingSettings();
            settings.meshInflationDistance = 0.0f;
            settings.flags |= RF_DISABLE_MESH_EARLY_EXIT;
            settings.flags &= ~RF_USE_MESH_FWN;

            primitives->clear();
            resource.write(*primitives);
            primitives->write();

            return sampler.evaluateSdfBatch(points, *primitives, 0.0f);
        }

        [[nodiscard]] std::vector<float> evaluateOnWebGpu(
          std::vector<Eigen::Vector3f> const & points,
          SpatialMeshData const & meshData,
          bool * skipped) const
        {
            *skipped = false;
            std::unique_ptr<webgpu::WebGPUSdfEvaluator> evaluator;
            try
            {
                evaluator = std::make_unique<webgpu::WebGPUSdfEvaluator>();
            }
            catch (std::exception const & exception)
            {
                *skipped = true;
                return {};
            }
            if (!evaluator->isAvailable())
            {
                *skipped = true;
                return {};
            }

            // Minimal WGSL model: pure signed distance to mesh resource 0.
            std::string const modelEvaluator = R"WGSL(
fn evaluateModel(pos: vec3<f32>) -> vec4<f32> {
    let d = gladiusSignedDistanceToMesh(pos, 0u);
    return vec4<f32>(0.0, 0.0, 0.0, d);
}
)WGSL";

            compute::SdfEvaluationRequest request{
              .positions = toWebGpuPoints(points),
              .isoValue = 0.0f,
              .shaderSource =
                webgpu::WebGPUSdfShaderComposer::composeWithMeshSupport(modelEvaluator),
              .parameterValues = {},
              .meshPayloadTable = {io::serializeSpatialMeshPayload(meshData)}};
            return evaluator->evaluate(request).values;
        }

        std::shared_ptr<ComputeContext> m_context;
        std::shared_ptr<events::Logger> m_logger;
    };

    TEST_F(MeshSdfBackendParityTest, CubeMesh_SignedDistanceMatchesAcrossBackends)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        MeshBVHBuilder builder;
        auto const meshData = builder.build(vertices, indices);
        ASSERT_FALSE(meshData.nodes.empty());
        ASSERT_FALSE(meshData.triangles.empty());

        auto const points = createTestPoints();

        auto const openClValues = evaluateOnOpenCl(points, meshData);
        ASSERT_EQ(openClValues.size(), points.size());
        bool skipped = false;
        auto const webGpuValues = evaluateOnWebGpu(points, meshData, &skipped);
        if (skipped)
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }
        ASSERT_EQ(webGpuValues.size(), points.size());

        constexpr float absoluteTolerance = 2.0e-4f;
        constexpr float relativeTolerance = 1.0e-4f;
        for (std::size_t index = 0u; index < points.size(); ++index)
        {
            ASSERT_TRUE(std::isfinite(openClValues[index]))
              << "OpenCL value at point " << index;
            ASSERT_TRUE(std::isfinite(webGpuValues[index]))
              << "WebGPU value at point " << index;

            // Sign must match exactly: sign flips are the primary failure mode.
            ASSERT_EQ((openClValues[index] < 0.0f), (webGpuValues[index] < 0.0f))
              << "Sign mismatch at point " << index << " (" << points[index].x() << ", "
              << points[index].y() << ", " << points[index].z() << "): OpenCL="
              << openClValues[index] << ", WebGPU=" << webGpuValues[index];

            auto const error = std::abs(openClValues[index] - webGpuValues[index]);
            auto const tolerance =
              absoluteTolerance + relativeTolerance * std::abs(openClValues[index]);
            EXPECT_LE(error, tolerance)
              << "Point " << index << " (" << points[index].x() << ", " << points[index].y()
              << ", " << points[index].z() << "): OpenCL=" << openClValues[index]
              << ", WebGPU=" << webGpuValues[index] << ", error=" << error;
        }
    }

    TEST_F(MeshSdfBackendParityTest, CubeMesh_KnownAnalyticDistancesMatchOnBothBackends)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        MeshBVHBuilder builder;
        auto const meshData = builder.build(vertices, indices);

        auto const points = createTestPoints();
        auto const openClValues = evaluateOnOpenCl(points, meshData);
        bool skipped = false;
        auto const webGpuValues = evaluateOnWebGpu(points, meshData, &skipped);
        if (skipped)
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }
        ASSERT_EQ(openClValues.size(), webGpuValues.size());

        // Analytic expectations for the unit cube (half extent 0.5).
        struct Expectation
        {
            float expected;
            float tolerance;
        };
        std::array<Expectation, 10u> const expectations{{
          {-0.5f, 0.01f},   // center
          {1.5f, 0.05f},    // far outside along X
          {-0.05f, 0.01f},  // near +X face inside
          {0.05f, 0.01f},   // near +X face outside
          {0.05f, 0.01f},   // near -X face outside
          {-0.02f, 0.02f},  // near +Y face inside
          {0.02f, 0.02f},   // near -Y face outside
          {-0.01f, 0.02f},  // near +Z face inside
          {0.01f, 0.02f},   // near +Z face outside
          {std::sqrt(3.0f) * 0.1f, 0.05f}, // diagonal outside near corner
        }};

        for (std::size_t index = 0u; index < openClValues.size(); ++index)
        {
            EXPECT_NEAR(openClValues[index], expectations[index].expected, expectations[index].tolerance)
              << "OpenCL analytic mismatch at point " << index;
            EXPECT_NEAR(webGpuValues[index], expectations[index].expected, expectations[index].tolerance)
              << "WebGPU analytic mismatch at point " << index;
        }
    }
}

#endif // GLADIUS_ENABLE_OPENCL && GLADIUS_ENABLE_WEBGPU
