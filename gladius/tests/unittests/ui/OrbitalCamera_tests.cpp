#include "ui/OrbitalCamera.h"

#include <gtest/gtest.h>

#include <cmath>

namespace gladius::ui::tests
{
    namespace
    {
        [[nodiscard]] float distance(cl_float3 const & lhs, cl_float3 const & rhs)
        {
            float const dx = lhs.x - rhs.x;
            float const dy = lhs.y - rhs.y;
            float const dz = lhs.z - rhs.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }

    TEST(OrbitalCamera_Update, WithPendingZoom_InterpolatesTowardTargetDistance)
    {
        OrbitalCamera camera;
        camera.snapToTarget();

        float const initialDistance = distance(camera.getEyePosition(), camera.getLookAt());

        camera.zoom(-0.5f);

        float const distanceBeforeUpdate = distance(camera.getEyePosition(), camera.getLookAt());
        EXPECT_NEAR(distanceBeforeUpdate, initialDistance, 1e-3f);

        EXPECT_TRUE(camera.update(16.f));

        float const distanceAfterUpdate = distance(camera.getEyePosition(), camera.getLookAt());
        EXPECT_LT(distanceAfterUpdate, initialDistance);
        EXPECT_GT(distanceAfterUpdate, initialDistance * 0.5f);
    }

    TEST(OrbitalCamera_SnapToTarget, WithPendingZoom_AppliesTargetDistance)
    {
        OrbitalCamera camera;
        camera.snapToTarget();

        float const initialDistance = distance(camera.getEyePosition(), camera.getLookAt());

        camera.zoom(-0.5f);
        camera.snapToTarget();

        float const snappedDistance = distance(camera.getEyePosition(), camera.getLookAt());
        EXPECT_NEAR(snappedDistance, initialDistance * 0.5f, 1e-3f);
    }

    TEST(OrbitalCamera_Update, WithPendingRotation_InterpolatesTowardTargetAngle)
    {
        OrbitalCamera camera;
        camera.snapToTarget();
        camera.rotate(0.0f, 0.5f);

        float const yawBeforeUpdate =
          std::atan2(camera.getEyePosition().y - camera.getLookAt().y,
                     camera.getEyePosition().x - camera.getLookAt().x);

        EXPECT_TRUE(camera.update(16.f));

        float const yawAfterUpdate =
          std::atan2(camera.getEyePosition().y - camera.getLookAt().y,
                     camera.getEyePosition().x - camera.getLookAt().x);
        EXPECT_GT(yawAfterUpdate, yawBeforeUpdate);
        EXPECT_LT(yawAfterUpdate, yawBeforeUpdate + 0.5f);
    }

    TEST(OrbitalCamera_Update, WithSmallMouseRotation_StillReportsMoving)
    {
        OrbitalCamera camera;
        camera.snapToTarget();

        // A tiny rotation (typical single-frame mouse drag) must not be swallowed
        // by the interpolation tolerance.
        camera.rotate(0.0f, 0.01f);

        EXPECT_TRUE(camera.update(16.f));
    }

    TEST(OrbitalCamera_Update, WithWrappedYawTarget_TakesShortestPath)
    {
        OrbitalCamera camera;
        camera.setAngle(0.0f, 3.0f);
        camera.snapToTarget();

        float const xOffsetBefore = camera.getEyePosition().x - camera.getLookAt().x;

        // Target wraps past +pi; shortest path continues through -pi (yaw keeps increasing),
        // which monotonically decreases the x-offset. The long way round would increase it.
        camera.setAngle(0.0f, -3.0f);
        (void) camera.update(16.f);

        float const xOffsetAfter = camera.getEyePosition().x - camera.getLookAt().x;
        EXPECT_LT(xOffsetAfter, xOffsetBefore);
    }
}