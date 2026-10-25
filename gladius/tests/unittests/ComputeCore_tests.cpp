#include "Document.h"
#include "Mesh.h"
#include "SpatialMeshResource.h"
#include "opencl_test_helper.h"
#include "testhelper.h"
#include <compute/ComputeCore.h>
#include <fmt/core.h>
#include <io/3mf/MeshWriter3mf.h>
#include <nodes/Assembly.h>
#include <nodes/DerivedNodes.h>
#include <nodes/Model.h>
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <gtest/gtest.h>
#include <thread>

namespace gladius_tests
{

    using namespace gladius;

    class ComputeCore_Test : public ::testing::Test
    {
        void SetUp() override
        { m_logger = std::make_shared<events::Logger>(); }

      protected:
        std::shared_ptr<ComputeCore> createCore()
        {
            auto context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);

            if (!context->isValid())
            {
                throw std::runtime_error(
                  "Failed to create OpenCL Context. Did you install proper GPU drivers?");
            }

            return std::make_shared<ComputeCore>(
              context, RequiredCapabilities::ComputeOnly, m_logger);
        }

        std::shared_ptr<ComputeCore> load3mf(std::filesystem::path const & path)
        {

            auto context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);

            if (!context->isValid())
            {
                throw std::runtime_error(
                  "Failed to create OpenCL Context. Did you install proper GPU drivers?");
            }

            m_core =
              std::make_shared<ComputeCore>(context, RequiredCapabilities::ComputeOnly, m_logger);
            m_doc = std::make_shared<Document>(m_core);

            m_doc->load(path);

            return m_core;
        }

      private:
        std::shared_ptr<ComputeCore> m_core;
        std::shared_ptr<Document> m_doc;

      protected:
        events::SharedLogger m_logger;
    };

    TEST_F(ComputeCore_Test, RefreshProgram_WithCodeGenerator_UsesOptimizedRenderProgramOnly)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto core = createCore();
        auto assembly = std::make_shared<nodes::Assembly>();
        assembly->assemblyModel()->createBeginEndWithDefaultInAndOuts();

        core->setCodeGenerator(CodeGenerator::Code);
        core->refreshProgram(assembly);

        EXPECT_FALSE(core->getProgramManager().hasPreviewModelSource());
        EXPECT_EQ(core->getBestRenderProgram().get(), core->getOptimzedRenderProgram().get());
        EXPECT_EQ(core->getSelectedRenderBackend(), RenderBackend::Optimized);
    }

    TEST_F(ComputeCore_Test,
           RefreshProgram_WithAutomaticCodeGenerator_SelectsPreviewUntilOptimizedIsReady)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto core = createCore();
        auto assembly = std::make_shared<nodes::Assembly>();
        assembly->assemblyModel()->createBeginEndWithDefaultInAndOuts();

        core->setCodeGenerator(CodeGenerator::Automatic);
        core->refreshProgram(assembly);

        auto previewProgram = core->getPreviewRenderProgram();
        auto optimizedProgram = core->getOptimzedRenderProgram();

        ASSERT_NE(previewProgram.get(), nullptr);
        ASSERT_NE(optimizedProgram.get(), nullptr);
        EXPECT_NE(previewProgram.get(), optimizedProgram.get());
        EXPECT_TRUE(core->getProgramManager().hasModelSource());
        EXPECT_TRUE(core->getProgramManager().hasPreviewModelSource());
        EXPECT_NE(core->getProgramManager().getModelSource(),
                  core->getProgramManager().getPreviewModelSource());
        EXPECT_EQ(core->getBestRenderProgram().get(), previewProgram.get());
        EXPECT_EQ(core->getSelectedRenderBackend(), RenderBackend::CommandStream);

        ASSERT_NO_THROW(core->recompileBlockingNoLock());
        EXPECT_EQ(core->getBestRenderProgram().get(), optimizedProgram.get());
        EXPECT_EQ(core->getSelectedRenderBackend(), RenderBackend::Optimized);
    }

    TEST_F(ComputeCore_Test,
           RecompileIfRequired_WithDeferredOptimizedRender_KeepsInteractiveBackend)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto core = createCore();
        auto assembly = std::make_shared<nodes::Assembly>();
        assembly->assemblyModel()->createBeginEndWithDefaultInAndOuts();

        core->setCodeGenerator(CodeGenerator::Automatic);
        core->setOptimizedRenderCompilationDeferred(true);
        core->refreshProgram(assembly);

        auto previewProgram = core->getPreviewRenderProgram();
        auto optimizedProgram = core->getOptimzedRenderProgram();
        ASSERT_NE(previewProgram.get(), nullptr);
        ASSERT_NE(optimizedProgram.get(), nullptr);

        core->recompileIfRequired();

        for (auto attempts = 0; attempts < 500 && core->isCompilationInProgress(); ++attempts)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        ASSERT_FALSE(core->isCompilationInProgress());
        core->recompileIfRequired();

        EXPECT_TRUE(core->isOptimizedRenderCompilationDeferred());
        EXPECT_TRUE(core->isRenderProgramReady());
        EXPECT_EQ(core->getBestRenderProgram().get(), previewProgram.get());
        EXPECT_EQ(core->getSelectedRenderBackend(), RenderBackend::CommandStream);
        EXPECT_FALSE(optimizedProgram->isCompilationInProgress());
        EXPECT_FALSE(optimizedProgram->isValid());

        core->setOptimizedRenderCompilationDeferred(false);
        core->recompileIfRequired();

        for (auto attempts = 0; attempts < 500 && optimizedProgram->isCompilationInProgress();
             ++attempts)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        ASSERT_FALSE(optimizedProgram->isCompilationInProgress());
        core->recompileIfRequired();
        EXPECT_EQ(core->getBestRenderProgram().get(), optimizedProgram.get());
        EXPECT_EQ(core->getSelectedRenderBackend(), RenderBackend::Optimized);
    }

    TEST_F(ComputeCore_Test, TryRenderProgramAccess_WithProgramManagerLockHeld_ReturnsCachedPreview)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto core = createCore();
        auto assembly = std::make_shared<nodes::Assembly>();
        assembly->assemblyModel()->createBeginEndWithDefaultInAndOuts();

        core->setCodeGenerator(CodeGenerator::Automatic);
        core->setOptimizedRenderCompilationDeferred(true);
        core->refreshProgram(assembly);

        auto previewProgram = core->getPreviewRenderProgram();
        ASSERT_NE(previewProgram.get(), nullptr);

        core->recompileIfRequired();
        for (auto attempts = 0; attempts < 500 && core->isCompilationInProgress(); ++attempts)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_FALSE(core->isCompilationInProgress());
        core->recompileIfRequired();

        ASSERT_TRUE(core->isRenderProgramReady());
        ASSERT_EQ(core->getSelectedRenderBackend(), RenderBackend::CommandStream);
        ASSERT_EQ(core->getBestRenderProgram().get(), previewProgram.get());

        std::promise<void> lockAcquired;
        std::promise<void> releaseLock;
        auto lockAcquiredFuture = lockAcquired.get_future();
        auto releaseLockFuture = releaseLock.get_future();

        std::thread lockHolder(
          [&programManager = core->getProgramManager(),
           &lockAcquired,
           releaseLockFuture = std::move(releaseLockFuture)]() mutable
          {
              auto computeToken = programManager.waitForComputeToken();
              lockAcquired.set_value();
              releaseLockFuture.wait();
          });

        auto const lockWasAcquired =
          lockAcquiredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;

        auto tryReady = std::optional<bool>{};
        auto tryBackend = std::optional<RenderBackend>{};
        auto tryProgram = std::optional<SharedRenderProgram>{};
        if (lockWasAcquired)
        {
            tryReady = core->tryIsRenderProgramReady();
            tryBackend = core->tryGetSelectedRenderBackend();
            tryProgram = core->tryGetBestRenderProgram();
        }

        releaseLock.set_value();
        lockHolder.join();

        ASSERT_TRUE(lockWasAcquired);
        ASSERT_TRUE(tryReady.has_value());
        EXPECT_TRUE(*tryReady);
        ASSERT_TRUE(tryBackend.has_value());
        EXPECT_EQ(*tryBackend, RenderBackend::CommandStream);
        ASSERT_TRUE(tryProgram.has_value());
        EXPECT_EQ(tryProgram->get(), previewProgram.get());
    }

    TEST_F(ComputeCore_Test, IsRenderProgramReady_WithModelRefreshInProgress_UsesCompiledPreview)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto core = createCore();
        auto assembly = std::make_shared<nodes::Assembly>();
        assembly->assemblyModel()->createBeginEndWithDefaultInAndOuts();

        core->setCodeGenerator(CodeGenerator::CommandStream);
        core->refreshProgram(assembly);
        ASSERT_NO_THROW(core->recompileBlockingNoLock());

        ASSERT_TRUE(core->isRendererReady());
        ASSERT_TRUE(core->isRenderProgramReady());

        core->getMeshResourceState()->signalCompilationStarted();

        EXPECT_FALSE(core->isRendererReady());
        EXPECT_TRUE(core->isRenderProgramReady());

        core->getMeshResourceState()->signalCompilationFinished();
    }

    TEST_F(ComputeCore_Test,
           RefreshProgram_WithCommandStreamCodeGenerator_CompilesAndRunsRenderKernel)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto core = createCore();
        auto assembly = std::make_shared<nodes::Assembly>();
        auto model = assembly->assemblyModel();
        model->createBeginEndWithDefaultInAndOuts();

        auto * minCorner = model->create<nodes::ConstantVector>();
        minCorner->parameter()[nodes::FieldNames::X].setValue(nodes::VariantType{-5.0f});
        minCorner->parameter()[nodes::FieldNames::Y].setValue(nodes::VariantType{-5.0f});
        minCorner->parameter()[nodes::FieldNames::Z].setValue(nodes::VariantType{-5.0f});

        auto * maxCorner = model->create<nodes::ConstantVector>();
        maxCorner->parameter()[nodes::FieldNames::X].setValue(nodes::VariantType{5.0f});
        maxCorner->parameter()[nodes::FieldNames::Y].setValue(nodes::VariantType{5.0f});
        maxCorner->parameter()[nodes::FieldNames::Z].setValue(nodes::VariantType{5.0f});

        auto * box = model->create<nodes::BoxMinMax>();
        ASSERT_TRUE(model->addLink(model->getInputs().at(nodes::FieldNames::Pos).getId(),
                                   box->parameter()[nodes::FieldNames::Pos].getId()));
        ASSERT_TRUE(model->addLink(minCorner->getOutputs().at(nodes::FieldNames::Vector).getId(),
                                   box->parameter()[nodes::FieldNames::Min].getId()));
        ASSERT_TRUE(model->addLink(maxCorner->getOutputs().at(nodes::FieldNames::Vector).getId(),
                                   box->parameter()[nodes::FieldNames::Max].getId()));
        ASSERT_TRUE(
          model->addLink(box->getOutputs().at(nodes::FieldNames::Shape).getId(),
                         model->getEndNode()->parameter().at(nodes::FieldNames::Shape).getId()));
        model->updateGraphAndOrderIfNeeded();

        core->setCodeGenerator(CodeGenerator::CommandStream);
        core->refreshProgram(assembly);

        auto previewProgram = core->getPreviewRenderProgram();
        ASSERT_NE(previewProgram.get(), nullptr);
        EXPECT_EQ(core->getBestRenderProgram().get(), previewProgram.get());
        EXPECT_EQ(core->getSelectedRenderBackend(), RenderBackend::CommandStream);

        ASSERT_TRUE(core->getProgramManager().hasPreviewModelSource());
        auto const previewSource = core->getProgramManager().getPreviewModelSource();
        EXPECT_NE(previewSource.find("cmds[i].type"), std::string::npos);
        EXPECT_NE(previewSource.find("bbBox(pos, min, max)"), std::string::npos);
        EXPECT_EQ(previewSource.find("#ifdef ENABLE_VDB"), std::string::npos);

        ASSERT_NO_THROW(core->recompileBlockingNoLock());
        ASSERT_FALSE(previewProgram->isCompilationInProgress());
        ASSERT_TRUE(previewProgram->isValid());

        auto primitives = core->getPrimitives();
        ASSERT_NE(primitives.get(), nullptr);
        ASSERT_NO_THROW(primitives->write());

        constexpr size_t imageSize = 16u;
        ImageRGBA targetImage(*core->getComputeContext(), imageSize, imageSize);
        ASSERT_NO_THROW(targetImage.allocateOnDevice());

        ASSERT_NO_THROW(previewProgram->renderScene(
          core->getComputeContext()->GetQueue(), *primitives, targetImage, 0.0f, 0u, imageSize));
        ASSERT_NO_THROW(targetImage.read());

        auto const & pixels = targetImage.getData();
        ASSERT_EQ(pixels.size(), imageSize * imageSize);
        EXPECT_TRUE(std::all_of(pixels.begin(),
                                pixels.end(),
                                [](cl_float4 const & pixel)
                                {
                                    return std::isfinite(pixel.s[0]) && std::isfinite(pixel.s[1]) &&
                                           std::isfinite(pixel.s[2]) && std::isfinite(pixel.s[3]);
                                }));
    }

    TEST_F(ComputeCore_Test, LoadMesh3mf_WithCommandStreamCodeGenerator_CompilesAndRunsRenderKernel)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto core = createCore();

        Mesh mesh(*core->getComputeContext());
        auto const a = Vector3{-1.0f, -1.0f, -1.0f};
        auto const b = Vector3{1.0f, -1.0f, -1.0f};
        auto const c = Vector3{0.0f, 1.0f, -1.0f};
        auto const d = Vector3{0.0f, 0.0f, 1.0f};
        mesh.addFace(a, c, b);
        mesh.addFace(a, b, d);
        mesh.addFace(b, c, d);
        mesh.addFace(c, a, d);

        auto const meshPath =
          std::filesystem::temp_directory_path() /
          fmt::format("gladius-command-stream-mesh-{}.3mf",
                      std::chrono::steady_clock::now().time_since_epoch().count());

        io::MeshWriter3mf writer(m_logger);
        ASSERT_NO_THROW(writer.exportMesh(meshPath, mesh, "TinyTetrahedron"));

        core->setCodeGenerator(CodeGenerator::CommandStream);
        auto document = std::make_shared<Document>(core);
        MeshSdfEvaluationConfig meshEvaluationConfig;
        meshEvaluationConfig.method = MeshSdfMethod::VoxelAccelerated;
        document->setMeshSdfEvaluationConfig(meshEvaluationConfig);

        ASSERT_NO_THROW(document->loadNonBlocking(meshPath));
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (document->isLoadingInProgress() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_FALSE(document->isLoadingInProgress());
        ASSERT_TRUE(document->getLoadingError().empty()) << document->getLoadingError();

        EXPECT_EQ(document->getMeshSdfEvaluationConfig().method, MeshSdfMethod::VoxelAccelerated);
        SpatialMeshResource const * importedMesh = nullptr;
        for (auto const & [key, resource] : document->getResourceManager().getResourceMap())
        {
            if (key.getResourceType() != ResourceType::Mesh)
            {
                continue;
            }
            importedMesh = dynamic_cast<SpatialMeshResource const *>(resource.get());
            if (importedMesh != nullptr)
            {
                break;
            }
        }
        ASSERT_NE(importedMesh, nullptr);
        EXPECT_EQ(importedMesh->evaluationConfig().method, MeshSdfMethod::PureBVH);
        EXPECT_FALSE(importedMesh->needsVoxelGridBuild());

        std::error_code ec;
        std::filesystem::remove(meshPath, ec);

        auto previewProgram = core->getPreviewRenderProgram();
        ASSERT_NE(previewProgram.get(), nullptr);
        EXPECT_EQ(core->getBestRenderProgram().get(), previewProgram.get());
        EXPECT_EQ(core->getSelectedRenderBackend(), RenderBackend::CommandStream);

        ASSERT_TRUE(core->getProgramManager().hasPreviewModelSource());
        auto const previewSource = core->getProgramManager().getPreviewModelSource();
        EXPECT_NE(previewSource.find("CT_SIGNED_DISTANCE_TO_MESH"), std::string::npos);
        EXPECT_NE(previewSource.find("payload((float3)"), std::string::npos);

        ASSERT_FALSE(previewProgram->isCompilationInProgress());
        ASSERT_TRUE(previewProgram->isValid());
        ASSERT_TRUE(core->isRenderProgramReady());

        auto primitives = core->getPrimitives();
        ASSERT_NE(primitives.get(), nullptr);
        ASSERT_NO_THROW(primitives->write());

        auto const & primitiveMeta = primitives->primitives.getData();
        auto const meshPrimitiveIt = std::find_if(
          primitiveMeta.begin(),
          primitiveMeta.end(),
          [](PrimitiveMeta const & meta) { return meta.primitiveType == SDF_SPATIAL_MESH_ROOT; });
        ASSERT_NE(meshPrimitiveIt, primitiveMeta.end());

        auto const & payload = primitives->data.getData();
        auto const headerStart = static_cast<size_t>(meshPrimitiveIt->start);
        ASSERT_GT(payload.size(), headerStart + 33u);

        auto const nodesOffset = static_cast<int>(payload[headerStart + 12u]);
        auto const trianglesOffset = static_cast<int>(payload[headerStart + 13u]);
        auto const normalsOffset = static_cast<int>(payload[headerStart + 14u]);
        auto const indicesOffset = static_cast<int>(payload[headerStart + 15u]);
        auto const voxelDataOffset = static_cast<int>(payload[headerStart + 26u]);
        auto const edgeNeighborsOffset = static_cast<int>(payload[headerStart + 28u]);
        EXPECT_EQ(nodesOffset % 4, 0);
        EXPECT_EQ(trianglesOffset % 4, 0);
        EXPECT_EQ(normalsOffset % 4, 0);
        EXPECT_EQ(indicesOffset % 4, 0);
        EXPECT_EQ(voxelDataOffset % 2, 0);
        EXPECT_EQ(edgeNeighborsOffset % 4, 0);

        constexpr size_t imageSize = 1u;
        ImageRGBA targetImage(*core->getComputeContext(), imageSize, imageSize);
        ASSERT_NO_THROW(targetImage.allocateOnDevice());

        ASSERT_NO_THROW(previewProgram->renderScene(
          core->getComputeContext()->GetQueue(), *primitives, targetImage, 0.0f, 0u, imageSize));
        ASSERT_NO_THROW(targetImage.read());

        auto const & pixels = targetImage.getData();
        ASSERT_EQ(pixels.size(), imageSize * imageSize);
        EXPECT_TRUE(std::all_of(pixels.begin(),
                                pixels.end(),
                                [](cl_float4 const & pixel)
                                {
                                    return std::isfinite(pixel.s[0]) && std::isfinite(pixel.s[1]) &&
                                           std::isfinite(pixel.s[2]) && std::isfinite(pixel.s[3]);
                                }));
    }

    TEST_F(ComputeCore_Test, DISABLED_PreComputeSDF_LoadedAssembly_EqualsExpectedResult)
    {
        auto core = load3mf("testdata/ImplicitGyroid.3mf");
        auto primitives = core->getPrimitives();
        auto const & payloadData = primitives->data.getData();
        auto const payloadDataHash = helper::computeHash(payloadData.cbegin(), payloadData.cend());
        EXPECT_EQ(payloadDataHash, 0u);

        auto resources = core->getResourceContext();
        auto const parameter = resources->getParameterBuffer().getData();
        for (auto const & param : parameter)
        {
            std::cout << param << std::endl;
        }

        auto const parameterHash = helper::computeHash(parameter.cbegin(), parameter.cend());
        constexpr auto expectedHash = 6494502327630714298u;
        EXPECT_EQ(parameterHash, expectedHash);
        EXPECT_TRUE(core->precomputeSdfForWholeBuildPlatform());

        // Reuse the previously defined resources variable instead of redefining it
        auto & preComp = resources->getPrecompSdfBuffer();
        preComp.read();
        auto const bufSize = preComp.getData().size();
        EXPECT_EQ(bufSize, 16777216u);

        auto const & data = preComp.getData();
        std::vector<float> distances;
        distances.reserve(data.size());
        for (auto const & sample : data)
        {
            distances.push_back(sample.s[3]);
        }
        auto const hash = helper::computeHash(distances.cbegin(), distances.cend());
        EXPECT_EQ(hash, 13095517456146691086u);

        auto bBox = core->getBoundingBox();
        EXPECT_TRUE(bBox.has_value());

        auto const tolerance = 1E-3f;
        EXPECT_NEAR(bBox->min.x, -7.6475257873535156f, tolerance);
        EXPECT_NEAR(bBox->min.y, -1.9666776657104492f, tolerance);
        EXPECT_NEAR(bBox->min.z, -0.00098828284535557032f, tolerance);

        EXPECT_NEAR(bBox->max.x, 64.728408813476562f, tolerance);
        EXPECT_NEAR(bBox->max.y, 74.136703491210938f, tolerance);
        EXPECT_NEAR(bBox->max.z, 50.00640869140625f, tolerance);
    }

    // Regression test for the Document lifecycle fix (review: F7).
    // Destroying a Document while an asynchronous model refresh is in flight must join the
    // worker before any member is torn down. Previously the implicit std::future destructors
    // joined the worker only after later-declared members were already gone (use-after-free).
    TEST_F(ComputeCore_Test, DocumentDestruction_WithInFlightModelRefresh_JoinsWorkerWithoutCrash)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto core = createCore();
        auto document = std::make_unique<Document>(core);

        // Launch the background refresh worker (sets m_futureModelRefresh).
        static_cast<void>(document->refreshModelIfNoCompilationIsRunning());

        // Destroy while the worker may still be running. Run the teardown on a separate
        // thread guarded by a watchdog so a regression that deadlocks fails the test
        // instead of hanging the whole suite.
        auto teardown =
          std::async(std::launch::async, [doc = std::move(document)]() mutable { doc.reset(); });

        ASSERT_EQ(teardown.wait_for(std::chrono::seconds(30)), std::future_status::ready)
          << "Document destruction deadlocked while joining the async refresh worker";
        ASSERT_NO_THROW(teardown.get());
    }

    TEST_F(ComputeCore_Test, RenderSession_RetainsPreviousSceneAcrossContextReplacement)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto core = createCore();
        auto session = core->createRenderSession();
        ASSERT_NE(session, nullptr);

        auto const previousContext = session->getComputeContext();
        ASSERT_NE(previousContext, nullptr);
        ASSERT_TRUE(session->isPayloadCurrent());

        auto replacementContext = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
        ASSERT_TRUE(replacementContext->isValid());
        core->setComputeContext(replacementContext);

        EXPECT_NE(core->getComputeContext().get(), previousContext.get());
        EXPECT_EQ(session->getComputeContext().get(), previousContext.get());
        EXPECT_TRUE(session->isPayloadCurrent());
    }

    TEST_F(ComputeCore_Test, PublishRenderSceneGeneration_RetainsExistingSessionGeneration)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto core = createCore();
        auto session = core->createRenderSession();
        ASSERT_NE(session, nullptr);

        auto const previousGenerationContext = session->getComputeContext();
        ASSERT_NE(previousGenerationContext, nullptr);

        auto replacement = std::make_shared<RenderSceneGeneration>(
          previousGenerationContext, RequiredCapabilities::ComputeOnly, m_logger);

        core->publishRenderSceneGeneration(replacement);

        auto currentSession = core->createRenderSession();
        ASSERT_NE(currentSession, nullptr);
        EXPECT_EQ(currentSession->getComputeContext().get(), previousGenerationContext.get());
        EXPECT_NE(currentSession->getPayloadSnapshot().primitiveMeta.resourceId,
                  session->getPayloadSnapshot().primitiveMeta.resourceId);
        EXPECT_EQ(session->getComputeContext().get(), previousGenerationContext.get());
        EXPECT_TRUE(session->isPayloadCurrent());
    }

    TEST_F(ComputeCore_Test, CreateRenderSession_WithMatchingRevision_ReturnsTaggedGeneration)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto core = createCore();
        auto const context = core->getComputeContext();
        RenderSceneRevision const revision{.documentIdentity = 41, .documentVersion = 7};
        auto generation = std::make_shared<RenderSceneGeneration>(
          context, RequiredCapabilities::ComputeOnly, m_logger, revision);
        core->publishRenderSceneGeneration(std::move(generation));

        auto session = core->createRenderSession(revision);

        ASSERT_TRUE(session.has_value());
        ASSERT_NE(*session, nullptr);
        EXPECT_EQ((*session)->getRevision(), revision);
    }

    TEST_F(ComputeCore_Test, CreateRenderSession_WithDifferentRevision_RejectsGeneration)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto core = createCore();
        auto const context = core->getComputeContext();
        RenderSceneRevision const publishedRevision{.documentIdentity = 41, .documentVersion = 7};
        auto generation = std::make_shared<RenderSceneGeneration>(
          context, RequiredCapabilities::ComputeOnly, m_logger, publishedRevision);
        core->publishRenderSceneGeneration(std::move(generation));

        EXPECT_FALSE(
          core->createRenderSession({.documentIdentity = 41, .documentVersion = 8}).has_value());
        EXPECT_FALSE(
          core->createRenderSession({.documentIdentity = 42, .documentVersion = 7}).has_value());
    }

    TEST_F(ComputeCore_Test, CreateRenderSession_WithUntaggedRevision_RejectsGeneration)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto core = createCore();

        EXPECT_FALSE(core->createRenderSession({}).has_value());
    }
}
