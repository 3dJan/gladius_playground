#include "Document.h"
#include "EventLogger.h"
#include "compute/ComputeCore.h"
#include "nodes/Assembly.h"
#include "webgpu/WebGPUComputeBackend.h"
#include "webgpu/WebGPUModelSliceRequestFactory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_set>
#include <utility>

namespace gladius::tests
{
#if defined(GLADIUS_ENABLE_WEBGPU)
    TEST(WebGPU3mfRendering, SimpleGyroid3mf_RendersNonBackgroundPixels)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        auto context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
        if (!context->isValid())
        {
            GTEST_SKIP() << "OpenCL context unavailable for 3MF loading";
        }

        auto logger = std::make_shared<events::Logger>();
        auto core = std::make_shared<ComputeCore>(context, RequiredCapabilities::ComputeOnly, logger);
        Document document(core);

        auto const filePath = std::filesystem::path("testdata") / "SimpleGyroid.3mf";
        ASSERT_TRUE(std::filesystem::exists(filePath)) << filePath;
        ASSERT_NO_THROW(document.load(filePath));

        auto const assembly = document.getAssembly();
        ASSERT_NE(assembly, nullptr);
        ASSERT_NE(assembly->assemblyModel(), nullptr);
        ASSERT_TRUE(core->updateParameterBlocking(*assembly));

        auto request = webgpu::WebGPUModelSliceRequestFactory::createFrame(
          *assembly,
          compute::FrameRequest{.width = 64u,
                                .height = 64u,
                                .firstRow = 0u,
                                .endRow = 64u,
                                .eyePosition = {200.0f, 200.0f, 800.0f},
                                .forwardDirection = {0.0f, 0.0f, -1.0f},
                                .rightDirection = {1.0f, 0.0f, 0.0f},
                                .upDirection = {0.0f, 1.0f, 0.0f},
                                .horizontalScale = 0.5f,
                                .verticalScale = 0.5f,
                                .maxRaySteps = 512u,
                                .maxTravelDistance = 1000.0f});

        webgpu::WebGPUComputeBackend backend;
        ASSERT_TRUE(backend.isAvailable());
        auto submission = backend.submitFrame(std::move(request));
        submission->wait();
        ASSERT_EQ(submission->getStatus(), compute::ComputeCompletionStatus::Succeeded)
          << submission->getErrorMessage();

        auto result = submission->takeResult();
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->pixels.size(), 64u * 64u);

        auto const background = 0xFF140D08u;
        auto const nonBackgroundPixels = std::count_if(result->pixels.begin(),
                                                       result->pixels.end(),
                                                       [background](std::uint32_t pixel)
                                                       { return pixel != background; });
        EXPECT_GT(nonBackgroundPixels, 64u);

        std::unordered_set<std::uint32_t> uniquePixels(result->pixels.begin(), result->pixels.end());
                ASSERT_GT(uniquePixels.size(), 4u)
                    << "image has too little shading variation; first pixel: 0x" << std::hex << *uniquePixels.begin();
    }
#endif
}
