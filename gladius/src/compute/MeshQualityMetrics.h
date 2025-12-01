#pragma once

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace gladius::compute
{
    /// Result of mesh quality analysis
    struct MeshQualityMetrics
    {
        float hausdorffDistance{0.0F};       ///< Maximum distance between meshes (worst case error)
        float meanDistance{0.0F};            ///< Average distance from simplified to original
        float rmsDistance{0.0F};             ///< Root mean square distance
        float percentile95Distance{0.0F};    ///< 95th percentile distance (robust max)
        std::size_t sampleCount{0U};         ///< Number of samples used
    };

    /// Compute quality metrics between an original mesh and a simplified version.
    /// Uses point sampling to estimate distances efficiently.
    class MeshQualityAnalyzer
    {
      public:
        /// Configuration for quality analysis
        struct Config
        {
            std::size_t samplesPerTriangle{10U};   ///< Random samples per triangle on simplified mesh
            std::size_t maxTotalSamples{100000U};  ///< Maximum total samples (limits computation time)
            unsigned int randomSeed{42U};           ///< Random seed for reproducibility
        };

        MeshQualityAnalyzer() = default;
        explicit MeshQualityAnalyzer(Config const & config)
            : m_config(config)
        {
        }

        /// Compute quality metrics comparing simplified mesh to original.
        /// Measures how far the simplified mesh deviates from the original.
        [[nodiscard]] MeshQualityMetrics analyze(
            std::vector<Eigen::Vector3f> const & originalPositions,
            std::vector<std::uint32_t> const & originalIndices,
            std::vector<Eigen::Vector3f> const & simplifiedPositions,
            std::vector<std::uint32_t> const & simplifiedIndices) const
        {
            MeshQualityMetrics result{};

            if (originalIndices.empty() || simplifiedIndices.empty())
            {
                return result;
            }

            // Sample points on simplified mesh
            std::vector<Eigen::Vector3f> samplePoints = sampleMeshSurface(
                simplifiedPositions, simplifiedIndices);

            if (samplePoints.empty())
            {
                return result;
            }

            // For each sample point, find distance to original mesh
            std::vector<float> distances;
            distances.reserve(samplePoints.size());

            for (auto const & point : samplePoints)
            {
                float const dist = pointToMeshDistance(point, originalPositions, originalIndices);
                distances.push_back(dist);
            }

            // Compute statistics
            result.sampleCount = distances.size();

            // Hausdorff (max)
            result.hausdorffDistance = *std::max_element(distances.begin(), distances.end());

            // Mean
            float sum = 0.0F;
            for (float d : distances)
            {
                sum += d;
            }
            result.meanDistance = sum / static_cast<float>(distances.size());

            // RMS
            float sumSquared = 0.0F;
            for (float d : distances)
            {
                sumSquared += d * d;
            }
            result.rmsDistance = std::sqrt(sumSquared / static_cast<float>(distances.size()));

            // 95th percentile
            std::sort(distances.begin(), distances.end());
            std::size_t const idx95 = static_cast<std::size_t>(
                static_cast<float>(distances.size()) * 0.95F);
            result.percentile95Distance = distances[std::min(idx95, distances.size() - 1)];

            return result;
        }

        /// Compute symmetric Hausdorff distance (max of both directions).
        /// More expensive but gives a complete picture.
        [[nodiscard]] float computeSymmetricHausdorff(
            std::vector<Eigen::Vector3f> const & meshAPositions,
            std::vector<std::uint32_t> const & meshAIndices,
            std::vector<Eigen::Vector3f> const & meshBPositions,
            std::vector<std::uint32_t> const & meshBIndices) const
        {
            auto const metricsAtoB = analyze(meshBPositions, meshBIndices, meshAPositions, meshAIndices);
            auto const metricsBtoA = analyze(meshAPositions, meshAIndices, meshBPositions, meshBIndices);
            return std::max(metricsAtoB.hausdorffDistance, metricsBtoA.hausdorffDistance);
        }

      private:
        Config m_config{};

        /// Sample random points on mesh surface
        [[nodiscard]] std::vector<Eigen::Vector3f> sampleMeshSurface(
            std::vector<Eigen::Vector3f> const & positions,
            std::vector<std::uint32_t> const & indices) const
        {
            std::vector<Eigen::Vector3f> samples;

            std::size_t const numTriangles = indices.size() / 3U;
            if (numTriangles == 0U)
            {
                return samples;
            }

            // Calculate samples per triangle based on limits
            std::size_t samplesPerTri = m_config.samplesPerTriangle;
            std::size_t const totalDesired = numTriangles * samplesPerTri;
            if (totalDesired > m_config.maxTotalSamples)
            {
                samplesPerTri = std::max(1UL, m_config.maxTotalSamples / numTriangles);
            }

            samples.reserve(numTriangles * samplesPerTri);
            std::mt19937 rng(m_config.randomSeed);
            std::uniform_real_distribution<float> dist(0.0F, 1.0F);

            for (std::size_t t = 0; t < numTriangles; ++t)
            {
                Eigen::Vector3f const & v0 = positions[indices[t * 3 + 0]];
                Eigen::Vector3f const & v1 = positions[indices[t * 3 + 1]];
                Eigen::Vector3f const & v2 = positions[indices[t * 3 + 2]];

                for (std::size_t s = 0; s < samplesPerTri; ++s)
                {
                    // Random barycentric coordinates
                    float u = dist(rng);
                    float v = dist(rng);
                    if (u + v > 1.0F)
                    {
                        u = 1.0F - u;
                        v = 1.0F - v;
                    }
                    float const w = 1.0F - u - v;

                    samples.push_back(u * v0 + v * v1 + w * v2);
                }
            }

            return samples;
        }

        /// Compute distance from a point to the closest point on a mesh
        [[nodiscard]] float pointToMeshDistance(
            Eigen::Vector3f const & point,
            std::vector<Eigen::Vector3f> const & positions,
            std::vector<std::uint32_t> const & indices) const
        {
            float minDist = std::numeric_limits<float>::max();

            std::size_t const numTriangles = indices.size() / 3U;
            for (std::size_t t = 0; t < numTriangles; ++t)
            {
                Eigen::Vector3f const & v0 = positions[indices[t * 3 + 0]];
                Eigen::Vector3f const & v1 = positions[indices[t * 3 + 1]];
                Eigen::Vector3f const & v2 = positions[indices[t * 3 + 2]];

                float const dist = pointToTriangleDistance(point, v0, v1, v2);
                minDist = std::min(minDist, dist);
            }

            return minDist;
        }

        /// Compute distance from point to triangle
        [[nodiscard]] static float pointToTriangleDistance(
            Eigen::Vector3f const & p,
            Eigen::Vector3f const & a,
            Eigen::Vector3f const & b,
            Eigen::Vector3f const & c)
        {
            // Based on the algorithm by Christer Ericson in "Real-Time Collision Detection"
            Eigen::Vector3f const ab = b - a;
            Eigen::Vector3f const ac = c - a;
            Eigen::Vector3f const ap = p - a;

            float const d1 = ab.dot(ap);
            float const d2 = ac.dot(ap);
            if (d1 <= 0.0F && d2 <= 0.0F)
            {
                return (p - a).norm(); // Closest to vertex a
            }

            Eigen::Vector3f const bp = p - b;
            float const d3 = ab.dot(bp);
            float const d4 = ac.dot(bp);
            if (d3 >= 0.0F && d4 <= d3)
            {
                return (p - b).norm(); // Closest to vertex b
            }

            float const vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F)
            {
                float const v = d1 / (d1 - d3);
                return (p - (a + v * ab)).norm(); // Closest to edge ab
            }

            Eigen::Vector3f const cp = p - c;
            float const d5 = ab.dot(cp);
            float const d6 = ac.dot(cp);
            if (d6 >= 0.0F && d5 <= d6)
            {
                return (p - c).norm(); // Closest to vertex c
            }

            float const vb = d5 * d2 - d1 * d6;
            if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F)
            {
                float const w = d2 / (d2 - d6);
                return (p - (a + w * ac)).norm(); // Closest to edge ac
            }

            float const va = d3 * d6 - d5 * d4;
            if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F)
            {
                float const w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                return (p - (b + w * (c - b))).norm(); // Closest to edge bc
            }

            // Inside triangle - project onto plane
            float const denom = 1.0F / (va + vb + vc);
            float const v = vb * denom;
            float const w = vc * denom;
            Eigen::Vector3f const closest = a + ab * v + ac * w;
            return (p - closest).norm();
        }
    };

} // namespace gladius::compute
