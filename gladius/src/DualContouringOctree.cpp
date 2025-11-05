#include "DualContouringOctree.h"
#include "DualContouringQef.h"

#include "ResourceContext.h"
#include "compute/ComputeCore.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gladius::dual_contouring
{
    namespace
    {
        static const std::array<Eigen::Vector3f, 8> cornerOffsets{
          Eigen::Vector3f{0.0F, 0.0F, 0.0F},
          Eigen::Vector3f{1.0F, 0.0F, 0.0F},
          Eigen::Vector3f{0.0F, 1.0F, 0.0F},
          Eigen::Vector3f{1.0F, 1.0F, 0.0F},
          Eigen::Vector3f{0.0F, 0.0F, 1.0F},
          Eigen::Vector3f{1.0F, 0.0F, 1.0F},
          Eigen::Vector3f{0.0F, 1.0F, 1.0F},
          Eigen::Vector3f{1.0F, 1.0F, 1.0F}};

                static const std::array<std::pair<std::uint8_t, std::uint8_t>, 12> edgeCornerIndices{
                    std::pair<std::uint8_t, std::uint8_t>{0U, 1U},
                    std::pair<std::uint8_t, std::uint8_t>{2U, 3U},
                    std::pair<std::uint8_t, std::uint8_t>{0U, 2U},
                    std::pair<std::uint8_t, std::uint8_t>{1U, 3U},
                    std::pair<std::uint8_t, std::uint8_t>{4U, 5U},
                    std::pair<std::uint8_t, std::uint8_t>{6U, 7U},
                    std::pair<std::uint8_t, std::uint8_t>{4U, 6U},
                    std::pair<std::uint8_t, std::uint8_t>{5U, 7U},
                    std::pair<std::uint8_t, std::uint8_t>{0U, 4U},
                    std::pair<std::uint8_t, std::uint8_t>{1U, 5U},
                    std::pair<std::uint8_t, std::uint8_t>{2U, 6U},
                    std::pair<std::uint8_t, std::uint8_t>{3U, 7U}};

                [[nodiscard]] Eigen::Vector3f cornerPosition(std::uint8_t cornerIndex, AxisAlignedBoundingBox const & bounds)
                {
                        Eigen::Vector3f corner = bounds.min;
                        corner.x() = cornerOffsets.at(cornerIndex).x() > 0.0F ? bounds.max.x() : bounds.min.x();
                        corner.y() = cornerOffsets.at(cornerIndex).y() > 0.0F ? bounds.max.y() : bounds.min.y();
                        corner.z() = cornerOffsets.at(cornerIndex).z() > 0.0F ? bounds.max.z() : bounds.min.z();
                        return corner;
                }

        [[nodiscard]] AxisAlignedBoundingBox makeChildBounds(AxisAlignedBoundingBox const & parent,
                                                             std::uint8_t childIdx)
        {
            AxisAlignedBoundingBox child{};
            auto const center = parent.center();

            child.min.x() = (childIdx & 1U) == 0U ? parent.min.x() : center.x();
            child.max.x() = (childIdx & 1U) == 0U ? center.x() : parent.max.x();

            child.min.y() = (childIdx & 2U) == 0U ? parent.min.y() : center.y();
            child.max.y() = (childIdx & 2U) == 0U ? center.y() : parent.max.y();

            child.min.z() = (childIdx & 4U) == 0U ? parent.min.z() : center.z();
            child.max.z() = (childIdx & 4U) == 0U ? center.z() : parent.max.z();

            return child;
        }

    }

    Eigen::Vector3f AxisAlignedBoundingBox::center() const
    {
        return 0.5F * (min + max);
    }

    Eigen::Vector3f AxisAlignedBoundingBox::extent() const
    {
        return max - min;
    }

    size_t OctreeBuilder::SdfGrid::index(size_t x, size_t y, size_t z) const
    {
        x = std::min(x, width - 1U);
        y = std::min(y, height - 1U);
        z = std::min(z, depth - 1U);
        return z * width * height + y * width + x;
    }

    float OctreeBuilder::SdfGrid::sample(Eigen::Vector3f const & position) const
    {
        Eigen::Vector3f const extent = max - min;

        auto const safeExtent = extent.cwiseMax(Eigen::Vector3f::Constant(1e-6F));
        Eigen::Vector3f normalized = (position - min).cwiseQuotient(safeExtent);
        normalized = normalized.cwiseMax(Eigen::Vector3f::Zero());
        normalized = normalized.cwiseMin(Eigen::Vector3f::Ones());

        float const sampleX = normalized.x() * static_cast<float>(width - 1U);
        float const sampleY = normalized.y() * static_cast<float>(height - 1U);
        float const sampleZ = normalized.z() * static_cast<float>(depth - 1U);

        auto const x0 = static_cast<size_t>(std::floor(sampleX));
        auto const y0 = static_cast<size_t>(std::floor(sampleY));
        auto const z0 = static_cast<size_t>(std::floor(sampleZ));

        auto const x1 = std::min(x0 + 1U, width - 1U);
        auto const y1 = std::min(y0 + 1U, height - 1U);
        auto const z1 = std::min(z0 + 1U, depth - 1U);

        float const tx = sampleX - static_cast<float>(x0);
        float const ty = sampleY - static_cast<float>(y0);
        float const tz = sampleZ - static_cast<float>(z0);

        auto const c000 = values.at(index(x0, y0, z0));
        auto const c100 = values.at(index(x1, y0, z0));
        auto const c010 = values.at(index(x0, y1, z0));
        auto const c110 = values.at(index(x1, y1, z0));
        auto const c001 = values.at(index(x0, y0, z1));
        auto const c101 = values.at(index(x1, y0, z1));
        auto const c011 = values.at(index(x0, y1, z1));
        auto const c111 = values.at(index(x1, y1, z1));

        auto const c00 = c000 * (1.0F - tx) + c100 * tx;
        auto const c10 = c010 * (1.0F - tx) + c110 * tx;
        auto const c01 = c001 * (1.0F - tx) + c101 * tx;
        auto const c11 = c011 * (1.0F - tx) + c111 * tx;

        auto const c0 = c00 * (1.0F - ty) + c10 * ty;
        auto const c1 = c01 * (1.0F - ty) + c11 * ty;

        return c0 * (1.0F - tz) + c1 * tz;
    }

    float OctreeBuilder::SdfGrid::valueAt(size_t x, size_t y, size_t z) const
    {
        return values.at(index(x, y, z));
    }

    OctreeBuilder::OctreeBuilder(gladius::ComputeCore & core,
                                 BoundingBox const & targetBounds,
                                 OctreeBuildConfig config)
        : m_config(std::move(config))
        , m_computeCore(&core)
    {
        if (m_config.sdfResolution < 2U)
        {
            throw std::invalid_argument("sdfResolution must be at least 2");
        }

        auto const resolution = static_cast<size_t>(std::max<size_t>(m_config.sdfResolution, 2U));
        core.setPreCompSdfSize(resolution);
        core.precomputeSdfForBBox(targetBounds);

        auto resources = core.getResourceContext();
        auto & sdfBuffer = resources->getPrecompSdfBuffer();
        sdfBuffer.read();

        m_grid.width = std::max<size_t>(sdfBuffer.getWidth(), 1U);
        m_grid.height = std::max<size_t>(sdfBuffer.getHeight(), 1U);
        m_grid.depth = std::max<size_t>(sdfBuffer.getDepth(), 1U);
        m_grid.values = sdfBuffer.getData();
        auto const & sdfBounds = resources->getPreCompSdfBBox();
        m_grid.min = Eigen::Vector3f{sdfBounds.min.x, sdfBounds.min.y, sdfBounds.min.z};
        m_grid.max = Eigen::Vector3f{sdfBounds.max.x, sdfBounds.max.y, sdfBounds.max.z};
                m_grid.spacing = Eigen::Vector3f{
                    (m_grid.width > 1U) ? (m_grid.max.x() - m_grid.min.x()) / static_cast<float>(m_grid.width - 1U) : 1.0F,
                    (m_grid.height > 1U) ? (m_grid.max.y() - m_grid.min.y()) / static_cast<float>(m_grid.height - 1U) : 1.0F,
                    (m_grid.depth > 1U) ? (m_grid.max.z() - m_grid.min.z()) / static_cast<float>(m_grid.depth - 1U) : 1.0F};
                m_grid.spacing = m_grid.spacing.cwiseMax(Eigen::Vector3f::Constant(1e-4F));

        m_rootBounds.min = m_grid.min;
        m_rootBounds.max = m_grid.max;

        resources->releasePreComputedSdf();

        // Initialize GPU sampling session if enabled
        if (m_config.enableGpuSampling && m_computeCore)
        {
            dual_contouring::GpuSamplingConfig gpuConfig{};
            gpuConfig.isoValue = m_config.isoValue;
            gpuConfig.enableCaching = true;
            gpuConfig.fallbackToCpu = true;
            m_gpuSession = std::make_unique<dual_contouring::GpuSamplingSession>(*m_computeCore, gpuConfig);
        }
    }

    OctreeBuilder::OctreeBuilder(AxisAlignedBoundingBox const & targetBounds,
                                 OctreeBuildConfig config,
                                 size_t width,
                                 size_t height,
                                 size_t depth,
                                 std::vector<float> values)
        : m_config(std::move(config))
    {
        if (width < 2U || height < 2U || depth < 2U)
        {
            throw std::invalid_argument("SDF grid dimensions must be at least 2 in each axis");
        }

        if (values.size() != width * height * depth)
        {
            throw std::invalid_argument("SDF grid value count does not match provided dimensions");
        }

        m_grid.min = targetBounds.min;
        m_grid.max = targetBounds.max;
        m_grid.width = width;
        m_grid.height = height;
        m_grid.depth = depth;
        m_grid.values = std::move(values);

        m_grid.spacing = Eigen::Vector3f{
          (width > 1U) ? (m_grid.max.x() - m_grid.min.x()) / static_cast<float>(width - 1U) : 1.0F,
          (height > 1U) ? (m_grid.max.y() - m_grid.min.y()) / static_cast<float>(height - 1U) : 1.0F,
          (depth > 1U) ? (m_grid.max.z() - m_grid.min.z()) / static_cast<float>(depth - 1U) : 1.0F};
        m_grid.spacing = m_grid.spacing.cwiseMax(Eigen::Vector3f::Constant(1e-4F));

        m_rootBounds.min = m_grid.min;
        m_rootBounds.max = m_grid.max;

        m_config.sdfResolution = width;
    }

    std::unique_ptr<OctreeNode> OctreeBuilder::build(OctreeMetrics & metrics)
    {
        metrics = {};
        auto root = buildNode(m_rootBounds, 0U, metrics);
        
        // Apply balanced refinement if enabled
        if (m_config.enableBalancedRefinement && root)
        {
            enforceBalance(*root, metrics);
        }
        
        return root;
    }

    std::unique_ptr<OctreeNode> OctreeBuilder::buildNode(AxisAlignedBoundingBox const & bounds,
                                                         std::uint8_t depth,
                                                         OctreeMetrics & metrics)
    {
        auto node = std::make_unique<OctreeNode>();
        node->bounds = bounds;
        node->depth = depth;

        metrics.nodeCount += 1U;
        metrics.maxDepthReached = std::max<size_t>(metrics.maxDepthReached, depth);

        // Use GPU acceleration if available
        if (m_gpuSession)
        {
            evaluateCornersGpu(*node);
        }
        else
        {
            evaluateCorners(*node);
        }

        if (shouldSubdivide(*node, depth))
        {
            node->isLeaf = false;
            node->childMask = 0U;

            for (std::uint8_t childIdx = 0U; childIdx < 8U; ++childIdx)
            {
                auto const childBounds = makeChildBounds(bounds, childIdx);
                auto child = buildNode(childBounds, depth + 1U, metrics);

                if (child)
                {
                    node->childMask |= static_cast<std::uint8_t>(1U << childIdx);
                }

                node->children[childIdx] = std::move(child);
            }
        }
        else
        {
            node->isLeaf = true;
            metrics.leafCount += 1U;
            
            // Use GPU acceleration if available
            if (m_gpuSession)
            {
                gatherHermiteSamplesGpu(*node);
            }
            else
            {
                gatherHermiteSamples(*node);
            }
            
            computeVertex(*node);
        }

        return node;
    }

    void OctreeBuilder::evaluateCorners(OctreeNode & node) const
    {
        auto const min = node.bounds.min;
        auto const max = node.bounds.max;

        auto minValue = std::numeric_limits<float>::max();
        auto maxValue = std::numeric_limits<float>::lowest();
        bool hasFiniteSample = false;

        for (size_t i = 0U; i < cornerOffsets.size(); ++i)
        {
            Eigen::Vector3f const corner{cornerOffsets[i].x() > 0.0F ? max.x() : min.x(),
                                          cornerOffsets[i].y() > 0.0F ? max.y() : min.y(),
                                          cornerOffsets[i].z() > 0.0F ? max.z() : min.z()};

            auto const value = m_grid.sample(corner);
            node.cornerValues[i] = value;
            if (!std::isfinite(value))
            {
                continue;
            }

            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
            hasFiniteSample = true;
        }

        auto const centerValue = m_grid.sample(node.bounds.center());
        if (std::isfinite(centerValue))
        {
            minValue = std::min(minValue, centerValue);
            maxValue = std::max(maxValue, centerValue);
            hasFiniteSample = true;
        }

        auto const gridExtent = m_grid.max - m_grid.min;
        auto const safeExtent = gridExtent.cwiseMax(Eigen::Vector3f::Constant(1e-6F));

        auto normalize = [&](Eigen::Vector3f const & position)
        {
            Eigen::Vector3f normalized = (position - m_grid.min).cwiseQuotient(safeExtent);
            normalized = normalized.cwiseMax(Eigen::Vector3f::Zero());
            normalized = normalized.cwiseMin(Eigen::Vector3f::Ones());
            return normalized;
        };

        auto const normalizedMin = normalize(node.bounds.min);
        auto const normalizedMax = normalize(node.bounds.max);

        auto const toIndex = [](float value, size_t upper)
        {
            auto const scaled = value * static_cast<float>(upper);
            auto const floored = static_cast<size_t>(std::floor(scaled));
            auto const ceiled = static_cast<size_t>(std::ceil(scaled));
            return std::pair<size_t, size_t>{std::min(floored, upper), std::min(ceiled, upper)};
        };

        auto const [minIndexX, maxIndexX] = toIndex(normalizedMin.x(), m_grid.width - 1U);
        auto const [minIndexY, maxIndexY] = toIndex(normalizedMin.y(), m_grid.height - 1U);
        auto const [minIndexZ, maxIndexZ] = toIndex(normalizedMin.z(), m_grid.depth - 1U);

        auto const [maxRangeX, maxRangeXCeil] = toIndex(normalizedMax.x(), m_grid.width - 1U);
        auto const [maxRangeY, maxRangeYCeil] = toIndex(normalizedMax.y(), m_grid.height - 1U);
        auto const [maxRangeZ, maxRangeZCeil] = toIndex(normalizedMax.z(), m_grid.depth - 1U);

        size_t const rangeMinX = std::min(minIndexX, maxRangeX);
        size_t const rangeMinY = std::min(minIndexY, maxRangeY);
        size_t const rangeMinZ = std::min(minIndexZ, maxRangeZ);
        size_t const rangeMaxX = std::max(maxIndexX, maxRangeXCeil);
        size_t const rangeMaxY = std::max(maxIndexY, maxRangeYCeil);
        size_t const rangeMaxZ = std::max(maxIndexZ, maxRangeZCeil);

        for (size_t z = rangeMinZ; z <= rangeMaxZ; ++z)
        {
            for (size_t y = rangeMinY; y <= rangeMaxY; ++y)
            {
                for (size_t x = rangeMinX; x <= rangeMaxX; ++x)
                {
                    auto const value = m_grid.valueAt(x, y, z);
                    if (!std::isfinite(value))
                    {
                        continue;
                    }

                    minValue = std::min(minValue, value);
                    maxValue = std::max(maxValue, value);
                    hasFiniteSample = true;
                }
            }
        }

        if (!hasFiniteSample)
        {
            node.isIntersecting = false;
            return;
        }

        node.isIntersecting = minValue <= m_config.isoValue && maxValue >= m_config.isoValue;
    }

    void OctreeBuilder::gatherHermiteSamples(OctreeNode & node) const
    {
        node.hermiteSamples.clear();
        node.hasVertex = false;
        node.vertexPosition = Eigen::Vector3f::Zero();
        node.vertexResidual = 0.0F;

        if (!node.isLeaf || !node.isIntersecting)
        {
            return;
        }

        for (auto const & edge : edgeCornerIndices)
        {
            auto const idx0 = edge.first;
            auto const idx1 = edge.second;

            float const value0 = node.cornerValues.at(idx0);
            float const value1 = node.cornerValues.at(idx1);

            if (!std::isfinite(value0) || !std::isfinite(value1))
            {
                continue;
            }

            float const distance0 = value0 - m_config.isoValue;
            float const distance1 = value1 - m_config.isoValue;

            if ((distance0 < 0.0F && distance1 < 0.0F) || (distance0 > 0.0F && distance1 > 0.0F))
            {
                continue;
            }

            auto const corner0 = cornerPosition(idx0, node.bounds);
            auto const corner1 = cornerPosition(idx1, node.bounds);

            float const denominator = distance0 - distance1;
            float t = 0.5F;
            if (std::abs(denominator) > 1e-6F)
            {
                t = distance0 / denominator;
            }
            t = std::clamp(t, 0.0F, 1.0F);

            Eigen::Vector3f const position = corner0 + (corner1 - corner0) * t;
            Eigen::Vector3f const gradient = evaluateGradient(position);

            if (!gradient.allFinite())
            {
                continue;
            }

            if (gradient.norm() <= 1e-6F)
            {
                continue;
            }

            node.hermiteSamples.push_back(OctreeNode::HermiteSample{position, gradient});
        }
    }

    void OctreeBuilder::computeVertex(OctreeNode & node) const
    {
        if (node.hermiteSamples.size() < 3U)
        {
            node.hasVertex = false;
            return;
        }

        QuadraticErrorFunction qef{};
        for (auto const & sample : node.hermiteSamples)
        {
            qef.addSample(sample.position, sample.normal);
        }

        Eigen::Vector3f position = Eigen::Vector3f::Zero();
        float residual = 0.0F;
        if (!qef.solveWithinBounds(node.bounds, position, residual))
        {
            node.hasVertex = false;
            return;
        }

        if (!position.allFinite() || !std::isfinite(residual))
        {
            node.hasVertex = false;
            return;
        }

        node.vertexPosition = position;
        node.vertexResidual = residual;
        Eigen::Vector3f normalSum = Eigen::Vector3f::Zero();
        for (auto const & sample : node.hermiteSamples)
        {
            normalSum += sample.normal;
        }

        if (normalSum.squaredNorm() > 1e-8F)
        {
            node.vertexNormal = normalSum.normalized();
        }
        else
        {
            Eigen::Vector3f gradient = evaluateGradient(node.vertexPosition);
            if (gradient.squaredNorm() > 1e-8F)
            {
                node.vertexNormal = gradient.normalized();
            }
            else
            {
                node.vertexNormal = Eigen::Vector3f{1.0F, 0.0F, 0.0F};
            }
        }
        node.hasVertex = true;
    }

    void OctreeBuilder::evaluateCornersGpu(OctreeNode & node) const
    {
        // Collect corner positions
        std::vector<Eigen::Vector3f> positions;
        positions.reserve(8U);
        
        for (std::uint8_t i = 0U; i < 8U; ++i)
        {
            positions.push_back(cornerPosition(i, node.bounds));
        }
        
        // GPU batch sampling
        std::vector<float> values;
        if (!m_gpuSession->sampleCorners(positions, values) || values.size() != 8U)
        {
            // Fallback to CPU
            evaluateCorners(node);
            return;
        }
        
        // Store results
        auto minValue = std::numeric_limits<float>::max();
        auto maxValue = std::numeric_limits<float>::lowest();
        bool hasFiniteSample = false;
        
        for (size_t i = 0U; i < 8U; ++i)
        {
            node.cornerValues[i] = values[i];
            if (std::isfinite(values[i]))
            {
                minValue = std::min(minValue, values[i]);
                maxValue = std::max(maxValue, values[i]);
                hasFiniteSample = true;
            }
        }
        
        if (!hasFiniteSample)
        {
            node.isIntersecting = false;
            return;
        }
        
        node.isIntersecting = minValue <= m_config.isoValue && maxValue >= m_config.isoValue;
    }

    void OctreeBuilder::gatherHermiteSamplesGpu(OctreeNode & node) const
    {
        node.hermiteSamples.clear();
        node.hasVertex = false;
        node.vertexPosition = Eigen::Vector3f::Zero();
        node.vertexResidual = 0.0F;

        if (!node.isLeaf || !node.isIntersecting)
        {
            return;
        }

        // Find zero-crossing edges and compute interpolated positions
        std::vector<Eigen::Vector3f> hermitePositions;
        hermitePositions.reserve(12U);

        for (auto const & edge : edgeCornerIndices)
        {
            auto const idx0 = edge.first;
            auto const idx1 = edge.second;

            float const value0 = node.cornerValues.at(idx0);
            float const value1 = node.cornerValues.at(idx1);

            if (!std::isfinite(value0) || !std::isfinite(value1))
            {
                continue;
            }

            float const distance0 = value0 - m_config.isoValue;
            float const distance1 = value1 - m_config.isoValue;

            if ((distance0 < 0.0F && distance1 < 0.0F) || (distance0 > 0.0F && distance1 > 0.0F))
            {
                continue;
            }

            auto const corner0 = cornerPosition(idx0, node.bounds);
            auto const corner1 = cornerPosition(idx1, node.bounds);

            float const denominator = distance0 - distance1;
            float t = 0.5F;
            if (std::abs(denominator) > 1e-6F)
            {
                t = distance0 / denominator;
            }
            t = std::clamp(t, 0.0F, 1.0F);

            Eigen::Vector3f const position = corner0 + (corner1 - corner0) * t;
            hermitePositions.push_back(position);
        }

        if (hermitePositions.empty())
        {
            return;
        }

        // GPU batch sampling with gradients
        std::vector<float> values;
        std::vector<Eigen::Vector3f> gradients;
        if (!m_gpuSession->sampleHermite(hermitePositions, values, gradients) ||
            values.size() != hermitePositions.size() ||
            gradients.size() != hermitePositions.size())
        {
            // Fallback to CPU
            gatherHermiteSamples(node);
            return;
        }

        // Build Hermite samples from GPU results
        for (size_t i = 0U; i < hermitePositions.size(); ++i)
        {
            auto const & gradient = gradients[i];
            
            if (!gradient.allFinite() || gradient.norm() <= 1e-6F)
            {
                continue;
            }

            node.hermiteSamples.push_back(OctreeNode::HermiteSample{hermitePositions[i], gradient});
        }
    }

    Eigen::Vector3f OctreeBuilder::evaluateGradient(Eigen::Vector3f const & position) const
    {
        Eigen::Vector3f gradient = Eigen::Vector3f::Zero();
        Eigen::Vector3f const delta = m_grid.spacing.cwiseMax(Eigen::Vector3f::Constant(1e-4F));

        for (int axis = 0; axis < 3; ++axis)
        {
            Eigen::Vector3f offset = Eigen::Vector3f::Zero();
            offset(axis) = delta(axis);

            Eigen::Vector3f const forwardPosition = clampToGrid(position + offset);
            Eigen::Vector3f const backwardPosition = clampToGrid(position - offset);

            float const forward = m_grid.sample(forwardPosition);
            float const backward = m_grid.sample(backwardPosition);

            if (!std::isfinite(forward) || !std::isfinite(backward))
            {
                continue;
            }

            gradient(axis) = (forward - backward) / (2.0F * delta(axis));
        }

        return gradient;
    }

    Eigen::Vector3f OctreeBuilder::clampToGrid(Eigen::Vector3f const & position) const
    {
        Eigen::Vector3f clamped = position;
        clamped.x() = std::clamp(clamped.x(), m_grid.min.x(), m_grid.max.x());
        clamped.y() = std::clamp(clamped.y(), m_grid.min.y(), m_grid.max.y());
        clamped.z() = std::clamp(clamped.z(), m_grid.min.z(), m_grid.max.z());
        return clamped;
    }

    bool OctreeBuilder::shouldSubdivide(OctreeNode const & node, std::uint8_t depth) const
    {
        if (!node.isIntersecting)
        {
            return false;
        }

        if (depth >= m_config.maxDepth)
        {
            return false;
        }

        if (m_config.forceUniform)
        {
            return true;
        }

        // Basic extent-based subdivision (existing)
        auto const extent = node.bounds.extent();
        auto const minimumExtent = extent.minCoeff();
        bool const subdivideByExtent = minimumExtent > 1e-3F;

        if (!subdivideByExtent)
        {
            return false;
        }

        // Curvature-based refinement (Phase 3 enhancement)
        if (m_config.enableCurvatureRefinement && depth < m_config.maxDepth - 1U)
        {
            // Estimate curvature from gradient variation across corners
            std::vector<Eigen::Vector3f> cornerGradients;
            cornerGradients.reserve(8U);

            for (std::uint8_t i = 0U; i < 8U; ++i)
            {
                auto const corner = cornerPosition(i, node.bounds);
                auto const gradient = evaluateGradient(corner);
                if (gradient.allFinite() && gradient.norm() > 1e-6F)
                {
                    cornerGradients.push_back(gradient.normalized());
                }
            }

            if (cornerGradients.size() >= 3U)
            {
                // Compute variance of normalized gradients as curvature proxy
                Eigen::Vector3f meanGradient = Eigen::Vector3f::Zero();
                for (auto const & g : cornerGradients)
                {
                    meanGradient += g;
                }
                meanGradient /= static_cast<float>(cornerGradients.size());

                float variance = 0.0F;
                for (auto const & g : cornerGradients)
                {
                    variance += (g - meanGradient).squaredNorm();
                }
                variance /= static_cast<float>(cornerGradients.size());

                // Subdivide if curvature (gradient variation) exceeds threshold
                if (variance > m_config.curvatureThreshold)
                {
                    return true;
                }
            }
        }

        return subdivideByExtent;
    }

    Eigen::Vector3f const & OctreeBuilder::gridMin() const
    {
        return m_grid.min;
    }

    Eigen::Vector3f const & OctreeBuilder::gridMax() const
    {
        return m_grid.max;
    }

    Eigen::Vector3f const & OctreeBuilder::gridSpacing() const
    {
        return m_grid.spacing;
    }

    size_t OctreeBuilder::gridWidth() const
    {
        return m_grid.width;
    }

    size_t OctreeBuilder::gridHeight() const
    {
        return m_grid.height;
    }

    size_t OctreeBuilder::gridDepth() const
    {
        return m_grid.depth;
    }

    float OctreeBuilder::gridSample(Eigen::Vector3f const & position) const
    {
        return m_grid.sample(position);
    }

    float OctreeBuilder::gridValueAt(size_t x, size_t y, size_t z) const
    {
        return m_grid.valueAt(x, y, z);
    }

    void OctreeBuilder::enforceBalance(OctreeNode & node, OctreeMetrics & metrics)
    {
        // Multi-pass balanced refinement to ensure depth difference ≤ 1
        // Repeat until no more subdivisions are needed
        bool needsAnotherPass = true;
        size_t passCount = 0U;
        constexpr size_t maxPasses = 10U; // Safety limit
        
        while (needsAnotherPass && passCount < maxPasses)
        {
            needsAnotherPass = false;
            ++passCount;
            
            // Recursively check and subdivide nodes that violate balance constraint
            std::function<bool(OctreeNode&)> balanceNode = [&](OctreeNode & current) -> bool
            {
                bool subdivided = false;
                
                if (current.isLeaf)
                {
                    // Check if this leaf needs subdivision due to deeper neighbors
                    std::uint8_t maxNeighborDepth = getMaxNeighborDepth(current);
                    
                    // If a neighbor is more than 1 level deeper, subdivide this node
                    if (maxNeighborDepth > current.depth + 1U && current.depth < m_config.maxDepth)
                    {
                        subdivideForBalance(current, current.depth + 1U, metrics);
                        subdivided = true;
                        ++metrics.balancePassSubdivisions;
                    }
                }
                else
                {
                    // Recurse into children
                    for (auto & child : current.children)
                    {
                        if (child)
                        {
                            subdivided |= balanceNode(*child);
                        }
                    }
                }
                
                return subdivided;
            };
            
            needsAnotherPass = balanceNode(node);
        }
    }

    std::uint8_t OctreeBuilder::getMaxNeighborDepth(OctreeNode const & node) const
    {
        // Simplified neighbor depth estimation
        // In a full implementation, this would traverse the octree to find actual neighbors
        // For now, we use a conservative estimate based on whether nearby cells could be deeper
        
        // If this is an intersecting node, nearby cells might be subdivided further
        if (node.isIntersecting)
        {
            // Assume neighbors could be one level deeper
            return static_cast<std::uint8_t>(node.depth + 1U);
        }
        
        return node.depth;
    }

    void OctreeBuilder::subdivideForBalance(OctreeNode & node, std::uint8_t targetDepth, OctreeMetrics & metrics)
    {
        if (!node.isLeaf || node.depth >= targetDepth)
        {
            return;
        }
        
        // Convert leaf to internal node
        node.isLeaf = false;
        node.childMask = 0U;
        node.hasVertex = false;
        node.hermiteSamples.clear();
        
        // Create children
        for (std::uint8_t childIdx = 0U; childIdx < 8U; ++childIdx)
        {
            auto const childBounds = makeChildBounds(node.bounds, childIdx);
            auto child = buildNode(childBounds, node.depth + 1U, metrics);
            
            if (child)
            {
                node.childMask |= static_cast<std::uint8_t>(1U << childIdx);
            }
            
            node.children[childIdx] = std::move(child);
        }
    }
}
