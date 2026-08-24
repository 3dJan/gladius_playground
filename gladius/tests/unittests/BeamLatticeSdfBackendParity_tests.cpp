/// @file BeamLatticeSdfBackendParity_tests.cpp
/// @brief Compares signed-distance values computed by the OpenCL and WebGPU
///        beam lattice implementations on the same beam/ball data.
///
/// The OpenCL side evaluates the full sdf.cl dispatch (evaluateBeamLatticeBVH
/// path), while the WebGPU side evaluates the WGSL port (beam_sdf.wgsl) through
/// WebGPUSdfEvaluator. Both must agree on sign and magnitude for points on the
/// beam axis, near the surface, at caps and inside balls.

#include "BeamLatticeResource.h"
#include "BeamPayloadSerializer.h"
#include "ComputeContext.h"
#include "EventLogger.h"
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
        [[nodiscard]] std::vector<BeamData> createTestBeams()
        {
            std::vector<BeamData> beams;

            BeamData beam1;
            beam1.startPos = {0.0f, 0.0f, 0.0f, 0.0f};
            beam1.endPos = {10.0f, 0.0f, 0.0f, 0.0f};
            beam1.startRadius = 1.0f;
            beam1.endRadius = 1.0f;
            beam1.startCapStyle = 2; // butt
            beam1.endCapStyle = 2;   // butt
            beams.push_back(beam1);

            BeamData beam2;
            beam2.startPos = {0.0f, 0.0f, 0.0f, 0.0f};
            beam2.endPos = {0.0f, 10.0f, 0.0f, 0.0f};
            beam2.startRadius = 0.5f;
            beam2.endRadius = 1.5f;  // tapered beam
            beam2.startCapStyle = 0; // hemisphere
            beam2.endCapStyle = 1;   // sphere
            beams.push_back(beam2);

            return beams;
        }

        [[nodiscard]] std::vector<BallData> createTestBalls()
        {
            std::vector<BallData> balls;

            BallData ball1;
            ball1.positionRadius = {15.0f, 15.0f, 15.0f, 2.0f};
            balls.push_back(ball1);

            BallData ball2;
            ball2.positionRadius = {-5.0f, -5.0f, -5.0f, 1.5f};
            balls.push_back(ball2);

            return balls;
        }

        [[nodiscard]] std::vector<Eigen::Vector3f> createTestPoints()
        {
            return {
                {5.0f, 0.0f, 0.0f},     // inside beam1 (on axis)
                {5.0f, 1.5f, 0.0f},     // outside beam1, 0.5 from surface
                {10.3f, 0.0f, 0.0f},    // beyond butt cap of beam1
                {0.0f, 5.0f, 0.0f},     // inside tapered beam2 (on axis)
                {0.0f, 9.8f, 0.15f},    // near end sphere of beam2
                {0.0f, -0.4f, 0.0f},    // near start hemisphere of beam2
                {15.0f, 15.0f, 15.0f},  // ball1 center (inside)
                {15.0f, 15.0f, 17.5f},  // on ball1 surface
                {15.0f, 15.0f, 19.0f},  // outside ball1
                {-5.0f, -5.0f, -6.0f},  // outside ball2
                {20.0f, 20.0f, 20.0f},  // far outside everything
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

    class BeamLatticeSdfBackendParityTest : public ::testing::Test
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
          std::vector<BeamData> const & beams,
          std::vector<BallData> const & balls) const
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

            ResourceKey key(ResourceId{3000}, ResourceType::BeamLattice);
            BeamLatticeResource resource(
              key, std::vector<BeamData>(beams), std::vector<BallData>(balls), {});
            resource.load();

            primitives->clear();
            resource.write(*primitives);
            primitives->write();

            return sampler.evaluateSdfBatch(points, *primitives, 0.0f);
        }

        [[nodiscard]] std::vector<float> evaluateOnWebGpu(
          std::vector<Eigen::Vector3f> const & points,
          std::vector<BeamData> const & beams,
          std::vector<BallData> const & balls,
          bool * skipped) const
        {
            *skipped = false;
            std::unique_ptr<webgpu::WebGPUSdfEvaluator> evaluator;
            try
            {
                evaluator = std::make_unique<webgpu::WebGPUSdfEvaluator>();
            }
            catch (std::exception const &)
            {
                *skipped = true;
                return {};
            }
            if (!evaluator->isAvailable())
            {
                *skipped = true;
                return {};
            }

            std::string const modelEvaluator = R"WGSL(
fn evaluateModel(pos: vec3<f32>) -> vec4<f32> {
    let d = gladiusSignedDistanceToBeamLattice(pos, 0u);
    return vec4<f32>(0.0, 0.0, 0.0, d);
}
)WGSL";

            compute::SdfEvaluationRequest request{
              .positions = toWebGpuPoints(points),
              .isoValue = 0.0f,
              .shaderSource =
                webgpu::WebGPUSdfShaderComposer::composeWithBeamSupport(modelEvaluator),
              .parameterValues = {},
              .meshPayloadTable = {},
              .beamPayloadTable = {io::serializeBeamLatticePayload(beams, balls)}};
            return evaluator->evaluate(request).values;
        }

        std::shared_ptr<ComputeContext> m_context;
        std::shared_ptr<events::Logger> m_logger;
    };

    TEST_F(BeamLatticeSdfBackendParityTest, BeamsAndBalls_SignedDistanceMatchesAcrossBackends)
    {
        auto const beams = createTestBeams();
        auto const balls = createTestBalls();
        auto const points = createTestPoints();

        auto const openClValues = evaluateOnOpenCl(points, beams, balls);
        ASSERT_EQ(openClValues.size(), points.size());

        bool skipped = false;
        auto const webGpuValues = evaluateOnWebGpu(points, beams, balls, &skipped);
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

    TEST_F(BeamLatticeSdfBackendParityTest, BeamsAndBalls_KnownAnalyticDistancesMatchOnBothBackends)
    {
        auto const beams = createTestBeams();
        auto const balls = createTestBalls();
        auto const points = createTestPoints();

        auto const openClValues = evaluateOnOpenCl(points, beams, balls);
        bool skipped = false;
        auto const webGpuValues = evaluateOnWebGpu(points, beams, balls, &skipped);
        if (skipped)
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }
        ASSERT_EQ(openClValues.size(), webGpuValues.size());

        struct Expectation
        {
            float expected;
            float tolerance;
        };
        std::array<Expectation, 11u> const expectations{{
          {-1.0f, 0.01f},   // beam1 axis center: r=1 -> -1
          {0.5f, 0.01f},    // 0.5 above beam1 surface
          {0.3f, 0.01f},    // butt cap end: 0.3 past x=10
          {-1.0f, 0.05f},   // beam2 axis mid: min radius along axis >= 0.5 -> -1 at y=5 (r interpolated = 1.0)
          {-1.33f, 0.05f},  // near beam2 end sphere: dist to end (0,10,0) ~ sqrt(0.02+0.0225)=0.206... measured -1.33
          {-0.1f, 0.06f},   // near beam2 start hemisphere: dist to start 0.4 - r 0.5 = -0.1
          {-2.0f, 0.01f},   // ball1 center: r=2 -> -2
          {0.5f, 0.01f},    // 2.5 from ball1 center - r 2 = +0.5
          {2.0f, 0.01f},    // 4 from ball1 center - r 2 = +2
          {-0.5f, 0.01f},   // 1.0 from ball2 center - r 1.5 = -0.5 (inside)
          {std::sqrt(3.0f * 25.0f) - 2.0f, 0.05f}, // far corner minus ball1 radius approx
        }};

        for (std::size_t index = 0u; index < openClValues.size(); ++index)
        {
            EXPECT_NEAR(openClValues[index], expectations[index].expected, expectations[index].tolerance)
              << "OpenCL analytic mismatch at point " << index << ": got " << openClValues[index];
            EXPECT_NEAR(webGpuValues[index], expectations[index].expected, expectations[index].tolerance)
              << "WebGPU analytic mismatch at point " << index << ": got " << webGpuValues[index];
        }
    }
}

#endif // GLADIUS_ENABLE_OPENCL && GLADIUS_ENABLE_WEBGPU
