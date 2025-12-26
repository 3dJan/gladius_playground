#include "DualContouringPrototype.h"

#include "DualContouringOctree.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <stdexcept>

#include <Eigen/Geometry>

namespace gladius::dual_contouring
{
    namespace
    {
        [[nodiscard]] float sdfUnitCube(Eigen::Vector3f const & position)
        {
            Eigen::Vector3f const halfExtents = Eigen::Vector3f::Constant(0.5F);
            Eigen::Vector3f const q = position.cwiseAbs() - halfExtents;
            Eigen::Vector3f const outside = q.cwiseMax(Eigen::Vector3f::Zero());
            float const outsideLength = outside.norm();
            float const insideDistance = std::min(std::max({q.x(), q.y(), q.z()}), 0.0F);
            return outsideLength + insideDistance;
        }

        [[nodiscard]] float sdfSphere(Eigen::Vector3f const & position)
        {
            constexpr float radius = 0.6F;
            return position.norm() - radius;
        }

        [[nodiscard]] float sdfCylinderZ(Eigen::Vector3f const & position)
        {
            constexpr float radius = 0.7F;
            constexpr float halfHeight = 0.7F;
            Eigen::Vector2f const radial{position.x(), position.y()};
            float const radialDistance = radial.norm() - radius;
            float const axialDistance = std::abs(position.z()) - halfHeight;
            float const outsideRadial = std::max(radialDistance, 0.0F);
            float const outsideAxial = std::max(axialDistance, 0.0F);
            float const outside = std::sqrt(outsideRadial * outsideRadial + outsideAxial * outsideAxial);
            float const inside = std::min(std::max(radialDistance, axialDistance), 0.0F);
            return outside + inside;
        }

        [[nodiscard]] float smoothMin(float a, float b, float k)
        {
            float const h = std::clamp(0.5F + 0.5F * (b - a) / k, 0.0F, 1.0F);
            return std::lerp(b, a, h) - k * h * (1.0F - h);
        }

        [[nodiscard]] float sdfCylinderBlend(Eigen::Vector3f const & position)
        {
            constexpr float blendFactor = 0.25F;
            float const sphere = sdfSphere(position);
            float const cylinder = sdfCylinderZ(position);
            return smoothMin(sphere, cylinder, blendFactor);
        }

        [[nodiscard]] AxisAlignedBoundingBox defaultBounds()
        {
            AxisAlignedBoundingBox bounds{};
            bounds.min = Eigen::Vector3f::Constant(-1.0F);
            bounds.max = Eigen::Vector3f::Constant(1.0F);
            return bounds;
        }

        [[nodiscard]] std::function<float(Eigen::Vector3f const &)> selectField(PrototypeShape shape)
        {
            switch (shape)
            {
            case PrototypeShape::UnitCube:
                return &sdfUnitCube;
            case PrototypeShape::Sphere:
                return &sdfSphere;
            case PrototypeShape::CylinderBlend:
                return &sdfCylinderBlend;
            }
            throw std::invalid_argument("Unsupported prototype shape");
        }

        [[nodiscard]] std::vector<float>
          sampleScalarField(std::function<float(Eigen::Vector3f const &)> const & field,
                            AxisAlignedBoundingBox const & bounds,
                            std::uint32_t resolution)
        {
            if (resolution < 2U)
            {
                throw std::invalid_argument("Resolution must be at least 2");
            }

            std::vector<float> samples;
            samples.resize(static_cast<size_t>(resolution) * static_cast<size_t>(resolution) *
                           static_cast<size_t>(resolution));

            Eigen::Vector3f const spacing = (bounds.max - bounds.min) /
                                             static_cast<float>(resolution - 1U);

            size_t index = 0U;
            for (std::uint32_t z = 0U; z < resolution; ++z)
            {
                float const zPos = bounds.min.z() + spacing.z() * static_cast<float>(z);
                for (std::uint32_t y = 0U; y < resolution; ++y)
                {
                    float const yPos = bounds.min.y() + spacing.y() * static_cast<float>(y);
                    for (std::uint32_t x = 0U; x < resolution; ++x)
                    {
                        float const xPos = bounds.min.x() + spacing.x() * static_cast<float>(x);
                        samples[index++] = field({xPos, yPos, zPos});
                    }
                }
            }

            return samples;
        }

    } // namespace

    PrototypeMesh generatePrototypeMesh(PrototypeShape shape,
                                        std::uint32_t resolution,
                                        PrototypeDiagnostics * diagnostics)
    {
        if (resolution < 2U)
        {
            throw std::invalid_argument("Prototype resolution must be at least 2");
        }

        std::uint32_t const cellCount = resolution - 1U;
        if (cellCount < 1U || !std::has_single_bit(cellCount))
        {
            throw std::invalid_argument("Resolution - 1 must be a power of two for uniform octree meshing");
        }

        OctreeBuildConfig config{};
        config.sdfResolution = resolution;
        config.maxDepth = static_cast<size_t>(std::countr_zero(cellCount));
        config.isoValue = 0.0F;
        config.forceUniform = true;

        AxisAlignedBoundingBox const bounds = defaultBounds();
        auto const field = selectField(shape);
        std::vector<float> samples = sampleScalarField(field, bounds, resolution);

        OctreeBuilder builder(bounds, config, resolution, resolution, resolution, std::move(samples));
        OctreeMetrics metrics{};
        auto root = builder.build(metrics);

        return buildDualContouringMesh(builder, *root, config, diagnostics);
    }
}
