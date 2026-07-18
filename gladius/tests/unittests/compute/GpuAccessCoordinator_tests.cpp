#include "compute/GpuAccessCoordinator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace gladius::tests
{
    namespace
    {
        [[nodiscard]] bool containsEvent(std::vector<GpuEventId> const & events,
                                         GpuEventId const eventId)
        {
            return std::find(events.begin(), events.end(), eventId) != events.end();
        }

        [[nodiscard]] GpuAccessPlan beginRead(GpuAccessCoordinator & coordinator,
                                              GpuResourceHandle resource,
                                              GpuQueueId queueId = 1u,
                                              std::string operationName = "read")
        {
            return coordinator.beginAccess(GpuAccessRequest{.resource = resource,
                                                            .queueId = queueId,
                                                            .mode = GpuAccessMode::Read,
                                                            .operationName = std::move(operationName)});
        }

        [[nodiscard]] GpuAccessPlan beginWrite(GpuAccessCoordinator & coordinator,
                                               GpuResourceHandle resource,
                                               GpuQueueId queueId = 1u,
                                               std::string operationName = "write")
        {
            return coordinator.beginAccess(GpuAccessRequest{.resource = resource,
                                                            .queueId = queueId,
                                                            .mode = GpuAccessMode::Write,
                                                            .operationName = std::move(operationName)});
        }

        void complete(GpuAccessCoordinator & coordinator,
                      GpuAccessPlan const & plan,
                      GpuEventId eventId)
        {
            ASSERT_TRUE(plan.granted());
            EXPECT_EQ(coordinator.completeAccess(plan.token, eventId), GpuAccessStatus::Granted);
        }
    }

    TEST(GpuAccessCoordinator, BeginAccess_ReadAfterWrite_ReturnsWriterDependency)
    {
        GpuAccessCoordinator coordinator;
        auto const resource = coordinator.registerResource(GpuResourceKind::Buffer, "parameter buffer");
        auto const write = beginWrite(coordinator, resource, 1u, "upload parameters");
        complete(coordinator, write, 10u);

        auto const read = beginRead(coordinator, resource, 2u, "render reads parameters");

        ASSERT_TRUE(read.granted());
        ASSERT_EQ(read.waitEvents.size(), 1u);
        EXPECT_EQ(read.waitEvents.front(), 10u);
    }

    TEST(GpuAccessCoordinator, BeginAccess_WriteAfterRead_ReturnsAllReaderDependencies)
    {
        GpuAccessCoordinator coordinator;
        auto const resource = coordinator.registerResource(GpuResourceKind::Image3D, "precomputed sdf");
        auto const previewRead = beginRead(coordinator, resource, 1u, "preview samples sdf");
        complete(coordinator, previewRead, 11u);
        auto const hqRead = beginRead(coordinator, resource, 2u, "hq render samples sdf");
        complete(coordinator, hqRead, 12u);

        auto const sdfWrite = beginWrite(coordinator, resource, 3u, "recompute sdf");

        ASSERT_TRUE(sdfWrite.granted());
        ASSERT_EQ(sdfWrite.waitEvents.size(), 2u);
        EXPECT_TRUE(containsEvent(sdfWrite.waitEvents, 11u));
        EXPECT_TRUE(containsEvent(sdfWrite.waitEvents, 12u));
    }

    TEST(GpuAccessCoordinator, BeginAccess_ReadAfterRead_ReturnsNoDependencies)
    {
        GpuAccessCoordinator coordinator;
        auto const resource = coordinator.registerResource(GpuResourceKind::Buffer, "primitive payload");
        auto const firstRead = beginRead(coordinator, resource, 1u, "preview render");
        complete(coordinator, firstRead, 21u);

        auto const secondRead = beginRead(coordinator, resource, 2u, "hq render");

        ASSERT_TRUE(secondRead.granted());
        EXPECT_TRUE(secondRead.waitEvents.empty());
    }

    TEST(GpuAccessCoordinator, BeginAccess_WriteAfterWrite_ReturnsPreviousWriterDependency)
    {
        GpuAccessCoordinator coordinator;
        auto const resource = coordinator.registerResource(GpuResourceKind::Buffer, "command buffer");
        auto const firstWrite = beginWrite(coordinator, resource, 1u, "build command stream");
        complete(coordinator, firstWrite, 31u);

        auto const secondWrite = beginWrite(coordinator, resource, 2u, "rebuild command stream");

        ASSERT_TRUE(secondWrite.granted());
        ASSERT_EQ(secondWrite.waitEvents.size(), 1u);
        EXPECT_EQ(secondWrite.waitEvents.front(), 31u);
    }

    TEST(GpuAccessCoordinator, BeginAccess_AfterEventCompleted_ReturnsNoDependency)
    {
        GpuAccessCoordinator coordinator;
        auto const resource = coordinator.registerResource(GpuResourceKind::Buffer, "metrics buffer");
        auto const write = beginWrite(coordinator, resource, 1u, "clear metrics");
        complete(coordinator, write, 41u);
        coordinator.markEventCompleted(41u);

        auto const read = beginRead(coordinator, resource, 2u, "read metrics");

        ASSERT_TRUE(read.granted());
        EXPECT_TRUE(read.waitEvents.empty());
    }

    TEST(GpuAccessCoordinator, BeginAccess_WithUnknownResource_ReturnsUnknownResource)
    {
        GpuAccessCoordinator coordinator;

        auto const read = beginRead(coordinator,
                                    GpuResourceHandle{.resourceId = 404u, .generation = 1u},
                                    1u,
                                    "read missing resource");

        EXPECT_EQ(read.status, GpuAccessStatus::UnknownResource);
        EXPECT_FALSE(read.granted());
    }

    TEST(GpuAccessCoordinator, CompleteAccess_WithSameTokenTwice_ReturnsUnknownAccessToken)
    {
        GpuAccessCoordinator coordinator;
        auto const resource = coordinator.registerResource(GpuResourceKind::Buffer, "parameter buffer");
        auto const write = beginWrite(coordinator, resource, 1u, "upload parameters");
        ASSERT_TRUE(write.granted());

        EXPECT_EQ(coordinator.completeAccess(write.token, 42u), GpuAccessStatus::Granted);
        EXPECT_EQ(coordinator.completeAccess(write.token, 43u), GpuAccessStatus::UnknownAccessToken);
    }

    TEST(GpuAccessCoordinator, IsIdle_WithOutstandingThenCompletedEvent_TracksState)
    {
        GpuAccessCoordinator coordinator;
        auto const resource = coordinator.registerResource(GpuResourceKind::Buffer, "command buffer");
        auto const write = beginWrite(coordinator, resource, 1u, "write commands");
        complete(coordinator, write, 44u);

        EXPECT_FALSE(coordinator.isIdle(resource));

        coordinator.markEventCompleted(44u);

        EXPECT_TRUE(coordinator.isIdle(resource));
        EXPECT_TRUE(coordinator.outstandingEvents(resource).empty());
    }

    TEST(GpuAccessCoordinator, MarkEventsCompleted_WithHostWait_ClearsOutstandingAccesses)
    {
        GpuAccessCoordinator coordinator;
        auto const resource = coordinator.registerResource(GpuResourceKind::Image2D, "distance init");
        auto const preview = beginWrite(coordinator, resource, 1u, "preview distance output");
        complete(coordinator, preview, 45u);
        ASSERT_EQ(coordinator.outstandingEvents(resource).size(), 1u);

        coordinator.markEventsCompleted({45u});

        auto const hqRead = beginRead(coordinator, resource, 2u, "hq distance init render");
        ASSERT_TRUE(hqRead.granted());
        EXPECT_TRUE(hqRead.waitEvents.empty());
    }

    TEST(GpuAccessCoordinator, BeginAccess_WriteWhileReadTokenPending_ReturnsPendingAccessWithoutEvent)
    {
        GpuAccessCoordinator coordinator;
        auto const resource = coordinator.registerResource(GpuResourceKind::Buffer, "primitive payload");
        auto const read = beginRead(coordinator, resource, 1u, "render reads payload");
        ASSERT_TRUE(read.granted());

        auto const write = beginWrite(coordinator, resource, 2u, "mesh cache build writes payload");

        EXPECT_EQ(write.status, GpuAccessStatus::PendingAccessWithoutEvent);
        EXPECT_FALSE(write.granted());
    }

    TEST(GpuAccessCoordinator, RetireCurrentGeneration_WithOutstandingWriter_KeepsGenerationUntilEventCompletes)
    {
        GpuAccessCoordinator coordinator;
        auto const resource = coordinator.registerResource(GpuResourceKind::Image3D, "precomputed sdf");
        auto const write = beginWrite(coordinator, resource, 1u, "write sdf generation");
        complete(coordinator, write, 51u);

        auto const retirement = coordinator.retireCurrentGeneration(resource.resourceId);

        ASSERT_TRUE(retirement.granted());
        EXPECT_FALSE(retirement.canReleaseImmediately());
        ASSERT_EQ(retirement.waitEvents.size(), 1u);
        EXPECT_EQ(retirement.waitEvents.front(), 51u);
        EXPECT_EQ(coordinator.retiredGenerationCount(resource.resourceId), 1u);

        coordinator.collectCompletedRetirements();
        EXPECT_EQ(coordinator.retiredGenerationCount(resource.resourceId), 1u);

        coordinator.markEventCompleted(51u);
        coordinator.collectCompletedRetirements();
        EXPECT_EQ(coordinator.retiredGenerationCount(resource.resourceId), 0u);
    }

    TEST(GpuAccessCoordinator, BeginAccess_WithRetiredGeneration_ReturnsStaleGeneration)
    {
        GpuAccessCoordinator coordinator;
        auto const oldGeneration = coordinator.registerResource(GpuResourceKind::Image2D, "low res preview");
        auto const retirement = coordinator.retireCurrentGeneration(oldGeneration.resourceId);
        ASSERT_TRUE(retirement.granted());

        auto const staleRead = beginRead(coordinator, oldGeneration, 1u, "stale preview resample");

        EXPECT_EQ(staleRead.status, GpuAccessStatus::StaleGeneration);
        EXPECT_FALSE(staleRead.granted());
    }

    TEST(GpuAccessCoordinator, Workflow_SdfPrecomputeBeforePreviewRender_AddsSdfDependency)
    {
        GpuAccessCoordinator coordinator;
        auto const sdf = coordinator.registerResource(GpuResourceKind::Image3D, "precomputed sdf");
        auto const precompute = beginWrite(coordinator, sdf, 1u, "preComputeSdf");
        complete(coordinator, precompute, 61u);

        auto const preview = beginRead(coordinator, sdf, 2u, "renderLowResPreview");

        ASSERT_TRUE(preview.granted());
        ASSERT_EQ(preview.waitEvents.size(), 1u);
        EXPECT_EQ(preview.waitEvents.front(), 61u);
    }

    TEST(GpuAccessCoordinator, Workflow_MeshCacheBuildBeforeRender_AddsPrimitivePayloadDependency)
    {
        GpuAccessCoordinator coordinator;
        auto const primitivePayload =
          coordinator.registerResource(GpuResourceKind::Buffer, "Primitives::data");
        auto const cacheBuild = beginWrite(coordinator, primitivePayload, 1u, "buildMeshSignCache");
        complete(coordinator, cacheBuild, 71u);

        auto const render = beginRead(coordinator, primitivePayload, 2u, "renderScene");

        ASSERT_TRUE(render.granted());
        ASSERT_EQ(render.waitEvents.size(), 1u);
        EXPECT_EQ(render.waitEvents.front(), 71u);
    }

    TEST(GpuAccessCoordinator, Workflow_DistanceInitPreviewBeforeHqRender_AddsPreviewDependency)
    {
        GpuAccessCoordinator coordinator;
        auto const distanceInit =
          coordinator.registerResource(GpuResourceKind::Image2D, "distance init buffer");
        auto const preview = beginWrite(coordinator, distanceInit, 1u, "preview distance output");
        complete(coordinator, preview, 81u);

        auto const hqRender = beginRead(coordinator, distanceInit, 2u, "hq distance init render");

        ASSERT_TRUE(hqRender.granted());
        ASSERT_EQ(hqRender.waitEvents.size(), 1u);
        EXPECT_EQ(hqRender.waitEvents.front(), 81u);
    }
}
