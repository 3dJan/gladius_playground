#include "compute/RenderContracts.h"
#include "compute/IComputeRenderer.h"

#include <gtest/gtest.h>

#include <limits>
#include <utility>

namespace gladius::compute::tests
{
    TEST(RenderCamera, IsValid_WithDefaultCamera_ReturnsTrue)
    {
        RenderCamera const camera;

        EXPECT_TRUE(camera.isValid());
    }

    TEST(RenderCamera, IsValid_WithZeroForwardDirection_ReturnsFalse)
    {
        RenderCamera camera;
        camera.forwardDirection = {0.0f, 0.0f, 0.0f};

        EXPECT_FALSE(camera.isValid());
    }

    TEST(RenderSettingsSnapshot, IsValid_WithInfiniteMaxTravelDistance_ReturnsFalse)
    {
        RenderSettingsSnapshot settings;
        settings.maxTravelDistance = std::numeric_limits<float>::infinity();

        EXPECT_FALSE(settings.isValid());
    }

    TEST(RenderFrustum, IsValid_WithZeroVerticalScale_ReturnsFalse)
    {
        RenderFrustum frustum;
        frustum.verticalScale = 0.0f;

        EXPECT_FALSE(frustum.isValid());
    }

    TEST(RenderSettingsSnapshot, IsValid_WithDefaultSettings_ReturnsTrue)
    {
        RenderSettingsSnapshot const settings;

        EXPECT_TRUE(settings.isValid());
    }

    TEST(RenderSettingsSnapshot, IsValid_WithNegativeFwnBeta_ReturnsFalse)
    {
        RenderSettingsSnapshot settings;
        settings.meshFwnBeta = -1.0f;

        EXPECT_FALSE(settings.isValid());
    }

    TEST(RenderViewport, PixelCount_WithProgressiveRows_ReturnsRowRangePixelCount)
    {
        RenderViewport const viewport{.width = 17u, .height = 33u, .firstRow = 10u, .endRow = 15u};

        ASSERT_TRUE(viewport.isValid());
        EXPECT_EQ(viewport.pixelCount(), 17u * 5u);
    }

    TEST(RenderViewport, IsValid_WithEmptyRowRange_ReturnsFalse)
    {
        RenderViewport const viewport{.width = 17u, .height = 33u, .firstRow = 10u, .endRow = 10u};

        EXPECT_FALSE(viewport.isValid());
        EXPECT_EQ(viewport.pixelCount(), 0u);
    }

    TEST(RenderRequest, IsValid_WithCompleteProgressiveRequest_ReturnsTrue)
    {
        RenderRequest const request{.viewport = {.width = 17u, .height = 33u, .firstRow = 0u, .endRow = 33u}};

        EXPECT_TRUE(request.isValid());
    }

    TEST(RenderFrame, IsValid_WithMatchingProgressivePixelCount_ReturnsTrue)
    {
        RenderFrame const frame{.width = 17u,
                                .height = 33u,
                                .firstRow = 10u,
                                .endRow = 15u,
                                .pixels = std::vector<std::uint32_t>(17u * 5u)};

        EXPECT_TRUE(frame.isValid());
    }

    TEST(RenderFrame, IsValid_WithMismatchedPixelCount_ReturnsFalse)
    {
        RenderFrame const frame{.width = 17u,
                                .height = 33u,
                                .firstRow = 10u,
                                .endRow = 15u,
                                .pixels = std::vector<std::uint32_t>(17u * 4u)};

        EXPECT_FALSE(frame.isValid());
    }

    TEST(RendererCapability, HasCapability_WithRequiredCombinedFlags_ReturnsTrue)
    {
        auto const capabilities = RendererCapability::AnalyticRendering | RendererCapability::ProgressiveRendering |
                                  RendererCapability::FramePresentation;

        EXPECT_TRUE(hasCapability(capabilities,
                                  RendererCapability::AnalyticRendering | RendererCapability::ProgressiveRendering));
        EXPECT_FALSE(hasCapability(capabilities, RendererCapability::MeshSdf));
    }

    class CompletedRenderSubmission final : public IRenderSubmission
    {
      public:
        explicit CompletedRenderSubmission(RenderFrame frame)
            : m_frame{std::move(frame)}
        {
        }

        [[nodiscard]] RenderSubmissionStatus getStatus() const noexcept override
        {
            return m_frame.has_value() ? RenderSubmissionStatus::Succeeded : RenderSubmissionStatus::Cancelled;
        }

        void requestCancellation() noexcept override
        {
            m_frame.reset();
        }

        void wait() override
        {
        }

        [[nodiscard]] std::optional<RenderFrame> takeFrame() override
        {
            return std::exchange(m_frame, std::nullopt);
        }

        [[nodiscard]] std::string getErrorMessage() const override
        {
            return {};
        }

      private:
        std::optional<RenderFrame> m_frame;
    };

    class TestComputeRenderer final : public IComputeRenderer
    {
      public:
        [[nodiscard]] ComputeBackendKind getBackendKind() const noexcept override
        {
            return ComputeBackendKind::OpenCL;
        }

        [[nodiscard]] RendererCapability getCapabilities() const noexcept override
        {
            return RendererCapability::AnalyticRendering | RendererCapability::FramePresentation;
        }

        [[nodiscard]] bool isAvailable() const noexcept override
        {
            return true;
        }

        [[nodiscard]] std::unique_ptr<IRenderSubmission> submitFrame(RenderRequest request) override
        {
            auto const & viewport = request.viewport;
            return std::make_unique<CompletedRenderSubmission>(
              RenderFrame{.width = viewport.width,
                          .height = viewport.height,
                          .firstRow = viewport.firstRow,
                          .endRow = viewport.endRow,
                          .freshness = request.freshness,
                          .pixels = std::vector<std::uint32_t>(viewport.pixelCount())});
        }
    };

    TEST(IComputeRenderer, SubmitFrame_WithValidRequest_TransfersFrameOwnershipOnce)
    {
        TestComputeRenderer renderer;
        RenderRequest const request{
          .viewport = {.width = 4u, .height = 3u, .firstRow = 1u, .endRow = 3u},
          .freshness = {.sceneGeneration = 1u, .viewGeneration = 2u, .parameterGeneration = 3u}};

        ASSERT_TRUE(request.isValid());
        EXPECT_EQ(renderer.getBackendKind(), ComputeBackendKind::OpenCL);
        EXPECT_TRUE(hasCapability(renderer.getCapabilities(), RendererCapability::AnalyticRendering));

        auto submission = renderer.submitFrame(request);
        ASSERT_NE(submission, nullptr);
        EXPECT_EQ(submission->getStatus(), RenderSubmissionStatus::Succeeded);

        auto frame = submission->takeFrame();
        ASSERT_TRUE(frame.has_value());
        EXPECT_TRUE(frame->isValid());
        EXPECT_EQ(frame->freshness, request.freshness);
        EXPECT_FALSE(submission->takeFrame().has_value());
    }
}
