#include "compute/BoundingBoxContracts.h"

#include <gtest/gtest.h>

#include <limits>

namespace gladius::compute::tests
{
    TEST(RenderBounds, DefaultValueIsNotAUsableModelBox)
    {
        RenderBounds const bounds;

        EXPECT_TRUE(bounds.isFiniteOrdered());
        EXPECT_FALSE(bounds.hasPositiveExtent());
        EXPECT_FALSE(bounds.isValid());
    }

    TEST(RenderBounds, ExpandedBoundsPreserveFiniteOrderedCoordinates)
    {
        RenderBounds const bounds{.min = {-2.0f, 1.0f, 3.0f}, .max = {4.0f, 5.0f, 9.0f}};

        auto const expanded = bounds.expanded(2.0f);

        EXPECT_EQ(expanded.min, (std::array<float, 3>{-4.0f, -1.0f, 1.0f}));
        EXPECT_EQ(expanded.max, (std::array<float, 3>{6.0f, 7.0f, 11.0f}));
        EXPECT_TRUE(expanded.isValid());
    }

    TEST(RenderEvaluationDomain, DefaultValueRepresentsTheNamedBuildVolume)
    {
        RenderEvaluationDomain const domain;

        EXPECT_TRUE(domain.isValid());
        EXPECT_EQ(domain.min, (std::array<float, 3>{0.0f, 0.0f, 0.0f}));
        EXPECT_EQ(domain.max, (std::array<float, 3>{400.0f, 400.0f, 400.0f}));
    }

    TEST(RenderFreshnessStamp, ModelGenerationIgnoresViewGeneration)
    {
        RenderFreshnessStamp first{.sceneGeneration = 4u,
                                   .viewGeneration = 7u,
                                   .parameterGeneration = 9u,
                                   .resourceGeneration = 11u,
                                   .evaluationGeneration = 13u};
        auto second = first;
        second.viewGeneration = 99u;

        EXPECT_TRUE(first.hasSameModelGeneration(second));

        second.resourceGeneration++;
        EXPECT_FALSE(first.hasSameModelGeneration(second));
    }

    TEST(BoundsRequest, DefaultsToTheExpandedOpenClProbeDomain)
    {
        BoundsRequest const request{.freshness = {.sceneGeneration = 1u}};

        EXPECT_TRUE(request.isValid());
        EXPECT_EQ(request.probeDomain.min, (std::array<float, 3>{-400.0f, -400.0f, -400.0f}));
        EXPECT_EQ(request.probeDomain.max, (std::array<float, 3>{800.0f, 800.0f, 800.0f}));
        EXPECT_EQ(request.probeSettings.resolution, (std::array<std::uint32_t, 3>{256u, 256u, 256u}));
        EXPECT_EQ(request.probeSettings.maxIterations, 30u);
    }

    TEST(BoundsRequest, ZeroSceneGenerationIsInvalid)
    {
        BoundsRequest const request;

        EXPECT_FALSE(request.isValid());
    }

    TEST(BoundsResult, BuildVolumeFallbackIsNotUsable)
    {
        BoundsResult const result{.status = BoundsResultStatus::Ready,
                                  .modelBounds = RenderBounds{.min = {0.0f, 0.0f, 0.0f},
                                                              .max = {400.0f, 400.0f, 400.0f}},
                                  .source = BoundsSource::BuildVolumeFallback,
                                  .guarantee = BoundsGuarantee::BuildVolumeFallback,
                                  .authoritative = false};

        EXPECT_FALSE(result.isUsable());
    }

    TEST(BoundsResult, CurrentReadyResultRequiresMatchingModelGeneration)
    {
        RenderFreshnessStamp const freshness{.sceneGeneration = 2u,
                                             .viewGeneration = 3u,
                                             .parameterGeneration = 5u,
                                             .resourceGeneration = 7u,
                                             .evaluationGeneration = 11u};
        BoundsResult const result{.status = BoundsResultStatus::Ready,
                                  .modelBounds = RenderBounds{.min = {-1.0f, -2.0f, -3.0f},
                                                              .max = {1.0f, 2.0f, 3.0f}},
                                  .freshness = freshness,
                                  .source = BoundsSource::GpuProbe,
                                  .guarantee = BoundsGuarantee::NumericalOpenClParity,
                                  .authoritative = true,
                                  .errorBound = 0.0f};

        auto viewChanged = freshness;
        viewChanged.viewGeneration++;
        EXPECT_TRUE(result.isCurrentFor(viewChanged));

        auto parametersChanged = freshness;
        parametersChanged.parameterGeneration++;
        EXPECT_FALSE(result.isCurrentFor(parametersChanged));
    }

    TEST(BoundsResult, NonFiniteErrorBoundIsNotUsable)
    {
        BoundsResult const result{.status = BoundsResultStatus::Ready,
                                  .modelBounds = RenderBounds{.min = {0.0f, 0.0f, 0.0f},
                                                              .max = {1.0f, 1.0f, 1.0f}},
                                  .freshness = {.sceneGeneration = 1u},
                                  .source = BoundsSource::ResourceMetadata,
                                  .guarantee = BoundsGuarantee::ExactMetadata,
                                  .authoritative = true};

        EXPECT_TRUE(std::isnan(result.errorBound));
        EXPECT_FALSE(result.isUsable());
    }
}