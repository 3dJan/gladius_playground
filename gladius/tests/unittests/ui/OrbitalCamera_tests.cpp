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
}