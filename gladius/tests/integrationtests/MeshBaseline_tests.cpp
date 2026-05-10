#include "BBox.h"
#include "Document.h"
#include "EventLogger.h"
#include "ComputeContext.h"

#include <compute/ComputeCore.h>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#include <chrono>
#include <filesystem>
#include <fmt/core.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

namespace gladius_tests
{

    using namespace gladius;

    namespace
    {
        struct BaselineCase
        {
            std::string name;
            std::filesystem::path relativePath;
            std::optional<size_t> expectedTriangles;
            std::optional<Eigen::Vector3f> expectedMeshMin;
            std::optional<Eigen::Vector3f> expectedMeshMax;
            std::optional<double> expectedRuntimeMs;
        };

        Eigen::Vector3f toEigen(cl_float4 const & value)
        {
            return Eigen::Vector3f{value.s[0], value.s[1], value.s[2]};
        }

        BBox computeMeshBounds(Mesh & mesh)
        {
            mesh.read();
            auto const & vertices = mesh.getVertices().getData();
            BBox bounds;
            for (auto const & vertex : vertices)
            {
                bounds.extend(Eigen::Vector3f{vertex.s[0], vertex.s[1], vertex.s[2]});
            }
            return bounds;
        }

        std::vector<BaselineCase> const & baselineCases()
        {
            static std::vector<BaselineCase> const cases = {
              BaselineCase{
                "SimpleGyroid",
                "testdata/SimpleGyroid.3mf",
                std::optional<size_t>{412562U},
                std::optional<Eigen::Vector3f>{
                  Eigen::Vector3f{0.0036280744F, 0.0035416728F, 0.0034536633F}},
                std::optional<Eigen::Vector3f>{Eigen::Vector3f{9.9176826F, 9.9176149F, 9.9176636F}},
                std::optional<double>{200.0}},
              BaselineCase{
                "ImplicitGyroid",
                "testdata/ImplicitGyroid.3mf",
                std::optional<size_t>{846436U},
                std::optional<Eigen::Vector3f>{
                  Eigen::Vector3f{-7.6393428F, -1.9578172F, -0.000014071315F}},
                std::optional<Eigen::Vector3f>{Eigen::Vector3f{64.156013F, 73.532295F, 49.609360F}},
                std::optional<double>{350.0}},
              BaselineCase{
                "SphereInACage",
                "testdata/SphereInACage_small.3mf",
                std::optional<size_t>{62692U},
                std::optional<Eigen::Vector3f>{Eigen::Vector3f{79.721405F, 91.074524F, 45.0F}},
                std::optional<Eigen::Vector3f>{
                  Eigen::Vector3f{89.643272F, 100.996391F, 54.921875F}},
                std::optional<double>{120.0}}};
            return cases;
        }
    }

    class MeshBaseline_Test : public ::testing::Test
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

    TEST_F(MeshBaseline_Test, GenerateMeshMatchesRecordedBaselines)
    {
        for (auto const & testCase : baselineCases())
        {
            SCOPED_TRACE(testCase.name);

            auto bundle = loadDocument(testCase.relativePath);

            ASSERT_TRUE(bundle.core->updateBBox());
            auto const boundingBox = bundle.core->getBoundingBox();
            ASSERT_TRUE(boundingBox.has_value());

            auto const bboxMin = toEigen(boundingBox->min);
            auto const bboxMax = toEigen(boundingBox->max);

            auto const startTime = std::chrono::steady_clock::now();
            auto mesh = bundle.document->generateMesh();
            auto const elapsed =
              std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(std::chrono::steady_clock::now() -
                                                                                     startTime)
                .count();

            auto const meshBounds = computeMeshBounds(mesh);
            auto const meshMin = meshBounds.getMin();
            auto const meshMax = meshBounds.getMax();

            auto const triangleCount = mesh.getNumberOfFaces();

            fmt::print("Baseline {}: triangles={}, runtime_ms={:.3f}, meshMin=({}, {}, {}), meshMax=({}, {}, {})\n",
                       testCase.name,
                       triangleCount,
                       elapsed,
                       meshMin.x(),
                       meshMin.y(),
                       meshMin.z(),
                       meshMax.x(),
                       meshMax.y(),
                       meshMax.z());

            EXPECT_GT(triangleCount, 0U);

            constexpr float boundingBoxTolerance = 0.25F;
            constexpr float meshTolerance = 1e-3F;

            EXPECT_GE(meshMin.x(), bboxMin.x() - boundingBoxTolerance);
            EXPECT_GE(meshMin.y(), bboxMin.y() - boundingBoxTolerance);
            EXPECT_GE(meshMin.z(), bboxMin.z() - boundingBoxTolerance);

            EXPECT_LE(meshMax.x(), bboxMax.x() + boundingBoxTolerance);
            EXPECT_LE(meshMax.y(), bboxMax.y() + boundingBoxTolerance);
            EXPECT_LE(meshMax.z(), bboxMax.z() + boundingBoxTolerance);

            if (testCase.expectedTriangles.has_value())
            {
                EXPECT_EQ(triangleCount, testCase.expectedTriangles.value());
            }

            if (testCase.expectedMeshMin.has_value())
            {
                auto const expectedMin = testCase.expectedMeshMin.value();
                EXPECT_NEAR(meshMin.x(), expectedMin.x(), meshTolerance);
                EXPECT_NEAR(meshMin.y(), expectedMin.y(), meshTolerance);
                EXPECT_NEAR(meshMin.z(), expectedMin.z(), meshTolerance);
            }

            if (testCase.expectedMeshMax.has_value())
            {
                auto const expectedMax = testCase.expectedMeshMax.value();
                EXPECT_NEAR(meshMax.x(), expectedMax.x(), meshTolerance);
                EXPECT_NEAR(meshMax.y(), expectedMax.y(), meshTolerance);
                EXPECT_NEAR(meshMax.z(), expectedMax.z(), meshTolerance);
            }

            if (testCase.expectedRuntimeMs.has_value())
            {
                EXPECT_LT(elapsed, testCase.expectedRuntimeMs.value());
            }
        }
    }
}
