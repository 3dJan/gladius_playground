#include "compute/AnalyticRenderSceneSnapshotFactory.h"
#include "compute/RenderSceneSnapshot.h"
#include "testhelper.h"
#include "webgpu/WebGPUComputeRenderer.h"
#include "webgpu/WebGPUFrameShaderComposer.h"

#include <BeamLatticeResource.h>
#include <Document.h>
#include <ResourceManager.h>
#include <SpatialMeshResource.h>
#include <nodes/Builder.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace gladius_tests
{
    using namespace gladius;

    /// End-to-end tests for materializing WebGPU render scenes from mesh 3MF documents.
    class WebGPUMeshScene_Test : public ::testing::Test
    {
      protected:
        std::shared_ptr<Document> loadCorelessDocument(std::filesystem::path const & path)
        {
            auto logger = std::make_shared<events::Logger>();
            // Coreless document: mirrors the pure-WebGPU runtime (no OpenCL core).
            auto doc = std::make_shared<Document>(logger);
            doc->load(path);
            return doc;
        }

        compute::RenderSceneSnapshot createBeamOnlySnapshot(std::uint64_t const sceneGeneration)
        {
            constexpr ResourceId BEAM_RESOURCE_ID = 7u;
            ResourceManager resourceManager(nullptr, {});
            auto const resourceKey = ResourceKey{BEAM_RESOURCE_ID, ResourceType::BeamLattice};
            std::vector<BeamData> beams{{.startPos = {0.0f, 0.0f, 0.0f, 0.0f},
                                         .endPos = {10.0f, 0.0f, 0.0f, 0.0f},
                                         .startRadius = 0.5f,
                                         .endRadius = 0.5f}};
            resourceManager.addResource(
              resourceKey,
              std::make_unique<BeamLatticeResource>(resourceKey,
                                                    std::move(beams),
                                                    std::vector<BallData>{},
                                                    BeamLatticeBallConfig{}));

            nodes::Model model;
            model.createBeginEndWithDefaultInAndOuts();
            nodes::Builder builder;
            builder.addBeamLatticeRef(
              model, resourceKey, model.getBeginNode()->getOutputs().at(nodes::FieldNames::Pos));
            return compute::AnalyticRenderSceneSnapshotFactory::create(
              model, sceneGeneration, resourceManager);
        }
    };

    TEST_F(WebGPUMeshScene_Test, MaterializeMesh3mf_ProducesSnapshotWithMeshPayloads)
    {
        auto doc = loadCorelessDocument("testdata/SphereInACageSimplifiedMesh.3mf");
        ASSERT_NE(doc, nullptr);

        auto const assembly = doc->getFlatAssembly();
        ASSERT_NE(assembly, nullptr);
        ASSERT_TRUE(assembly->assemblyModel());

        auto snapshot = compute::AnalyticRenderSceneSnapshotFactory::create(
          *assembly, 1u, &doc->getResourceManager());

        // Diagnostic detail: identify which validity condition fails.
        std::string invalidReason;
        if (snapshot.sceneGeneration == 0u)
        {
            invalidReason += "sceneGeneration==0; ";
        }
        if (snapshot.analyticEvaluatorWgsl.empty())
        {
            invalidReason += "evaluator empty; ";
        }
        if (!compute::hasCapability(snapshot.requiredCapabilities,
                                    compute::RendererCapability::AnalyticRendering))
        {
            invalidReason += "missing AnalyticRendering; ";
        }
        bool const declaresMesh =
          compute::hasCapability(snapshot.requiredCapabilities, compute::RendererCapability::MeshSdf);
        if (declaresMesh != !snapshot.meshResources.empty())
        {
            invalidReason += "mesh capability/payload mismatch; ";
        }
        std::string payloadSizes;
        for (auto const & mesh : snapshot.meshResources)
        {
            payloadSizes += std::to_string(mesh.data.size()) + ",";
        }
        ASSERT_TRUE(snapshot.isValid())
          << "Snapshot must be valid for a mesh scene. Details: " << invalidReason
          << " meshCount=" << snapshot.meshResources.size()
          << " payloadSizes=[" << payloadSizes << "]";
        EXPECT_FALSE(snapshot.meshResources.empty());
        EXPECT_TRUE(declaresMesh);
        EXPECT_FALSE(snapshot.analyticEvaluatorWgsl.empty());
    }

    TEST_F(WebGPUMeshScene_Test, MaterializeMesh3mf_MeshPayloadContainsValidBvh)
    {
        auto doc = loadCorelessDocument("testdata/SphereInACageSimplifiedMesh.3mf");
        auto const assembly = doc->getFlatAssembly();
        ASSERT_NE(assembly, nullptr);

        auto snapshot = compute::AnalyticRenderSceneSnapshotFactory::create(
          *assembly, 2u, &doc->getResourceManager());
        ASSERT_TRUE(snapshot.isValid());

        bool foundValidPayload = false;
        for (auto const & mesh : snapshot.meshResources)
        {
            if (!mesh.isValid())
            {
                continue;
            }
            foundValidPayload = true;
            ASSERT_GE(mesh.data.size(), 34u);
            // Header counts: nodeCount and triCount must be non-zero for a real mesh.
            EXPECT_GT(mesh.data[8], 0.0f);
            EXPECT_GT(mesh.data[9], 0.0f);
            // BVH nodes offset points inside the payload.
            auto const nodesOffset = static_cast<std::size_t>(mesh.data[12]);
            EXPECT_GT(nodesOffset, 34u);
            EXPECT_LT(nodesOffset, mesh.data.size());
        }
        EXPECT_TRUE(foundValidPayload) << "At least one non-empty mesh payload expected";
    }

    TEST_F(WebGPUMeshScene_Test, MaterializeMesh3mf_EvaluatorReferencesMeshHook)
    {
        auto doc = loadCorelessDocument("testdata/SphereInACageSimplifiedMesh.3mf");
        auto const assembly = doc->getFlatAssembly();
        ASSERT_NE(assembly, nullptr);

        auto snapshot = compute::AnalyticRenderSceneSnapshotFactory::create(
          *assembly, 3u, &doc->getResourceManager());
        ASSERT_TRUE(snapshot.isValid());

        EXPECT_NE(snapshot.analyticEvaluatorWgsl.find("gladiusSignedDistanceToMesh"),
                  std::string::npos)
          << "Evaluator must call the mesh SDF hook for mesh-based models";
    }

    TEST_F(WebGPUMeshScene_Test, MaterializeBeamOnlyScene_ProducesSnapshotWithBeamPayload)
    {
        constexpr ResourceId BEAM_RESOURCE_ID = 7u;
        auto const snapshot = createBeamOnlySnapshot(4u);

        ASSERT_TRUE(snapshot.isValid());
        EXPECT_TRUE(compute::hasCapability(snapshot.requiredCapabilities,
                                           compute::RendererCapability::BeamLattice));
        EXPECT_FALSE(compute::hasCapability(snapshot.requiredCapabilities,
                                            compute::RendererCapability::MeshSdf));
        ASSERT_GT(snapshot.beamLatticeResources.size(), BEAM_RESOURCE_ID);
        EXPECT_TRUE(snapshot.beamLatticeResources[BEAM_RESOURCE_ID].isValid());
        EXPECT_TRUE(snapshot.meshResources.empty());
        EXPECT_NE(snapshot.analyticEvaluatorWgsl.find("gladiusSignedDistanceToBeamLattice"),
                  std::string::npos);
    }

    TEST_F(WebGPUMeshScene_Test, MaterializeMixedMeshImplicit3mf_ProducesMeshEnabledShader)
    {
        auto doc = loadCorelessDocument("testdata/honeycombecase_connectable_007.3mf");
        auto const assembly = doc->getFlatAssembly();
        ASSERT_NE(assembly, nullptr);
        ASSERT_TRUE(assembly->assemblyModel());

        auto snapshot = compute::AnalyticRenderSceneSnapshotFactory::create(
          *assembly, 4u, &doc->getResourceManager());

        ASSERT_TRUE(snapshot.isValid());
        EXPECT_TRUE(compute::hasCapability(snapshot.requiredCapabilities,
                                           compute::RendererCapability::AnalyticRendering));
        EXPECT_TRUE(compute::hasCapability(snapshot.requiredCapabilities,
                                           compute::RendererCapability::MeshSdf));
        EXPECT_FALSE(snapshot.meshResources.empty());
        EXPECT_NE(snapshot.analyticEvaluatorWgsl.find("gladiusSignedDistanceToMesh"),
                  std::string::npos);

        auto const shader = webgpu::WebGPUFrameShaderComposer::composeWithMeshSupport(
          snapshot.analyticEvaluatorWgsl);
        EXPECT_NE(shader.find("fn evaluateModel("), std::string::npos);
        EXPECT_NE(shader.find("fn gladiusSignedDistanceToMesh("), std::string::npos);
        EXPECT_NE(shader.find("@group(0) @binding(4)"), std::string::npos);
        EXPECT_NE(shader.find("@group(0) @binding(5)"), std::string::npos);
    }

#if defined(GLADIUS_ENABLE_WEBGPU)
    TEST_F(WebGPUMeshScene_Test, RenderBeamOnlyScene_WithWebGpu_ProducesShadedPixels)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        auto snapshot = createBeamOnlySnapshot(5u);
        ASSERT_TRUE(snapshot.isValid());

        webgpu::WebGPUComputeRenderer renderer;
        if (!renderer.isAvailable())
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }
        ASSERT_TRUE(compute::hasCapability(renderer.getCapabilities(),
                                           compute::RendererCapability::BeamLattice));

        auto scene = renderer.materializeScene(std::move(snapshot));
        auto submission = renderer.submitFrame(
          *scene,
          compute::RenderRequest{
            .camera = {.eyePosition = {5.0f, 0.0f, 20.0f},
                       .forwardDirection = {0.0f, 0.0f, -1.0f},
                       .rightDirection = {1.0f, 0.0f, 0.0f},
                       .upDirection = {0.0f, 1.0f, 0.0f}},
            .frustum = {.horizontalScale = 0.35f, .verticalScale = 0.35f},
            .settings = {.maxRaySteps = 512u, .maxTravelDistance = 100.0f},
            .modelBounds = compute::RenderBounds{.min = {-1.0f, -1.0f, -1.0f},
                                                  .max = {11.0f, 1.0f, 1.0f}},
            .viewport = {.width = 64u, .height = 64u, .firstRow = 0u, .endRow = 64u}});
        submission->wait();

        ASSERT_EQ(submission->getStatus(), compute::RenderSubmissionStatus::Succeeded)
          << submission->getErrorMessage();
        auto frame = submission->takeFrame();
        ASSERT_TRUE(frame.has_value());
        ASSERT_TRUE(frame->isValid());

        constexpr std::uint32_t BACKGROUND = 0xFF1A1A1Au;
        auto const nonBackground = std::count_if(frame->pixels.begin(),
                                                 frame->pixels.end(),
                                                 [](std::uint32_t const pixel)
                                                 { return pixel != BACKGROUND; });
        EXPECT_GT(nonBackground, 32u);
    }

    TEST_F(WebGPUMeshScene_Test, RenderMixedMeshImplicit3mf_WithWebGpu_ProducesShadedPixels)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        auto doc = loadCorelessDocument("testdata/honeycombecase_connectable_007.3mf");
        auto const assembly = doc->getFlatAssembly();
        ASSERT_NE(assembly, nullptr);
        ASSERT_TRUE(assembly->assemblyModel());

        auto snapshot = compute::AnalyticRenderSceneSnapshotFactory::create(
          *assembly, 5u, &doc->getResourceManager());
        ASSERT_TRUE(snapshot.isValid());

        webgpu::WebGPUComputeRenderer renderer;
        if (!renderer.isAvailable())
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }
        ASSERT_TRUE(compute::hasCapability(renderer.getCapabilities(),
                                           compute::RendererCapability::MeshSdf));

        auto scene = renderer.materializeScene(std::move(snapshot));
        auto submission = renderer.submitFrame(
          *scene,
          compute::RenderRequest{
            .camera = {.eyePosition = {70.0f, 70.0f, 300.0f},
                       .forwardDirection = {0.0f, 0.0f, -1.0f},
                       .rightDirection = {1.0f, 0.0f, 0.0f},
                       .upDirection = {0.0f, 1.0f, 0.0f}},
            .frustum = {.horizontalScale = 0.5f, .verticalScale = 0.5f},
            .settings = {.maxRaySteps = 1024u, .maxTravelDistance = 500.0f},
            .modelBounds = compute::RenderBounds{.min = {-70.0f, -60.0f, -5.0f},
                                                  .max = {205.0f, 205.0f, 105.0f}},
            .viewport = {.width = 96u, .height = 96u, .firstRow = 0u, .endRow = 96u}});
        submission->wait();

        ASSERT_EQ(submission->getStatus(), compute::RenderSubmissionStatus::Succeeded)
          << submission->getErrorMessage();
        auto frame = submission->takeFrame();
        ASSERT_TRUE(frame.has_value());
        ASSERT_TRUE(frame->isValid());

        constexpr std::uint32_t BACKGROUND = 0xFF1A1A1Au;
        auto const nonBackground = std::count_if(frame->pixels.begin(),
                                                 frame->pixels.end(),
                                                 [](std::uint32_t const pixel)
                                                 { return pixel != BACKGROUND; });
        std::unordered_set<std::uint32_t> const uniquePixels(frame->pixels.begin(),
                                                             frame->pixels.end());
        EXPECT_GT(nonBackground, 96u);
        EXPECT_GT(uniquePixels.size(), 4u);
    }
#endif
}
