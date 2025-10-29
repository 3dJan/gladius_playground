#pragma once

#include "kernel/types.h"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace gladius
{
    class ComputeCore;
}

namespace gladius::dual_contouring
{
    struct AxisAlignedBoundingBox
    {
        Eigen::Vector3f min{Eigen::Vector3f::Zero()};
        Eigen::Vector3f max{Eigen::Vector3f::Zero()};

        [[nodiscard]] Eigen::Vector3f center() const;
        [[nodiscard]] Eigen::Vector3f extent() const;
    };

    struct OctreeNode
    {
        AxisAlignedBoundingBox bounds{};
        std::array<float, 8> cornerValues{};
        std::array<std::unique_ptr<OctreeNode>, 8> children{};
        struct HermiteSample
        {
            Eigen::Vector3f position{Eigen::Vector3f::Zero()};
            Eigen::Vector3f normal{Eigen::Vector3f::Zero()};
        };
        std::vector<HermiteSample> hermiteSamples{};
        Eigen::Vector3f vertexPosition{Eigen::Vector3f::Zero()};
        float vertexResidual{0.0F};
        bool isLeaf{true};
        bool isIntersecting{false};
        bool hasVertex{false};
        std::uint8_t childMask{0U};
        std::uint8_t depth{0U};
    };

    struct OctreeMetrics
    {
        size_t nodeCount{0U};
        size_t leafCount{0U};
        size_t maxDepthReached{0U};
    };

    struct OctreeBuildConfig
    {
        size_t sdfResolution{64U};
        size_t maxDepth{4U};
        float isoValue{0.0F};
    };

    class OctreeBuilder
    {
      public:
        OctreeBuilder(gladius::ComputeCore & core,
                      BoundingBox const & targetBounds,
                      OctreeBuildConfig config);

        [[nodiscard]] std::unique_ptr<OctreeNode> build(OctreeMetrics & metrics);

      private:
        struct SdfGrid
        {
            Eigen::Vector3f min{Eigen::Vector3f::Zero()};
            Eigen::Vector3f max{Eigen::Vector3f::Zero()};
                        Eigen::Vector3f spacing{Eigen::Vector3f::Ones()};
            size_t width{1U};
            size_t height{1U};
            size_t depth{1U};
            std::vector<float> values{0.0F};

            [[nodiscard]] float sample(Eigen::Vector3f const & position) const;
                        [[nodiscard]] float valueAt(size_t x, size_t y, size_t z) const;

          private:
            [[nodiscard]] size_t index(size_t x, size_t y, size_t z) const;
        };

        std::unique_ptr<OctreeNode> buildNode(AxisAlignedBoundingBox const & bounds,
                                              std::uint8_t depth,
                                              OctreeMetrics & metrics);
        void evaluateCorners(OctreeNode & node) const;
                void gatherHermiteSamples(OctreeNode & node) const;
                void computeVertex(OctreeNode & node) const;
                [[nodiscard]] Eigen::Vector3f evaluateGradient(Eigen::Vector3f const & position) const;
                [[nodiscard]] Eigen::Vector3f clampToGrid(Eigen::Vector3f const & position) const;
        [[nodiscard]] bool shouldSubdivide(OctreeNode const & node, std::uint8_t depth) const;

        SdfGrid m_grid{};
        OctreeBuildConfig m_config{};
        AxisAlignedBoundingBox m_rootBounds{};
    };
}
