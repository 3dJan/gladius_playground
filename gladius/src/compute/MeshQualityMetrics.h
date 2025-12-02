#pragma once

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <random>
#include <unordered_map>
#include <unordered_set>
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
    
    /// Triangle quality statistics
    struct TriangleQualityStats
    {
        float minAngle{0.0F};           ///< Minimum angle across all triangles (degrees)
        float maxAngle{0.0F};           ///< Maximum angle across all triangles (degrees)
        float avgMinAngle{0.0F};        ///< Average of minimum angle per triangle
        float minAspectRatio{0.0F};     ///< Worst aspect ratio (0=degenerate, 1=equilateral)
        float avgAspectRatio{0.0F};     ///< Average aspect ratio
        std::size_t degenerateCount{0}; ///< Number of near-degenerate triangles
        std::size_t totalTriangles{0};
    };

    /// Mesh quality improvement utilities
    class MeshQualityImprover
    {
      public:
        /// Configuration for mesh improvement
        struct Config
        {
            float minAngleThreshold{15.0F};     ///< Triangles with angles below this are candidates for improvement (degrees)
            std::size_t maxEdgeFlipPasses{5U};  ///< Maximum edge flip optimization passes
            bool enableEdgeFlipping{true};      ///< Enable edge flipping to improve triangle quality
            float aspectRatioThreshold{0.1F};   ///< Triangles with aspect ratio below this are considered poor
        };
        
        MeshQualityImprover() = default;
        explicit MeshQualityImprover(Config const & config) : m_config(config) {}
        
        /// Compute triangle quality statistics
        [[nodiscard]] static TriangleQualityStats computeQualityStats(
            std::vector<Eigen::Vector3f> const & positions,
            std::vector<std::uint32_t> const & indices)
        {
            TriangleQualityStats stats{};
            stats.minAngle = 180.0F;
            stats.maxAngle = 0.0F;
            stats.minAspectRatio = 1.0F;
            
            std::size_t const numTriangles = indices.size() / 3U;
            stats.totalTriangles = numTriangles;
            
            if (numTriangles == 0U)
            {
                return stats;
            }
            
            float sumMinAngles = 0.0F;
            float sumAspectRatios = 0.0F;
            
            for (std::size_t t = 0; t < numTriangles; ++t)
            {
                Eigen::Vector3f const & v0 = positions[indices[t * 3 + 0]];
                Eigen::Vector3f const & v1 = positions[indices[t * 3 + 1]];
                Eigen::Vector3f const & v2 = positions[indices[t * 3 + 2]];
                
                auto const [minAng, maxAng] = computeTriangleAngles(v0, v1, v2);
                float const aspectRatio = computeAspectRatio(v0, v1, v2);
                
                stats.minAngle = std::min(stats.minAngle, minAng);
                stats.maxAngle = std::max(stats.maxAngle, maxAng);
                stats.minAspectRatio = std::min(stats.minAspectRatio, aspectRatio);
                
                sumMinAngles += minAng;
                sumAspectRatios += aspectRatio;
                
                if (aspectRatio < 0.01F || minAng < 1.0F)
                {
                    ++stats.degenerateCount;
                }
            }
            
            stats.avgMinAngle = sumMinAngles / static_cast<float>(numTriangles);
            stats.avgAspectRatio = sumAspectRatios / static_cast<float>(numTriangles);
            
            return stats;
        }
        
        /// Improve mesh quality using edge flipping (Delaunay-like optimization).
        /// Returns the number of edges flipped.
        [[nodiscard]] std::size_t improveQuality(
            std::vector<Eigen::Vector3f> & positions,
            std::vector<std::uint32_t> & indices) const
        {
            if (!m_config.enableEdgeFlipping || indices.size() < 6U)
            {
                return 0U;
            }
            
            std::size_t totalFlips = 0U;
            
            for (std::size_t pass = 0; pass < m_config.maxEdgeFlipPasses; ++pass)
            {
                std::size_t const passFlips = edgeFlipPass(positions, indices);
                totalFlips += passFlips;
                
                if (passFlips == 0U)
                {
                    break; // Converged
                }
            }
            
            return totalFlips;
        }
        
        /// Improve mesh quality with optional SDF projection to keep vertices on surface.
        /// sdfEvaluator takes a position and returns the signed distance.
        /// sdfGradientEvaluator takes a position and returns the normalized gradient.
        [[nodiscard]] std::size_t improveQualityWithSdf(
            std::vector<Eigen::Vector3f> & positions,
            std::vector<Eigen::Vector3f> & normals,
            std::vector<std::uint32_t> & indices,
            std::function<float(Eigen::Vector3f const &)> const & sdfEvaluator,
            std::function<Eigen::Vector3f(Eigen::Vector3f const &)> const & sdfGradientEvaluator,
            std::size_t projectionIterations = 3U) const
        {
            std::size_t const flips = improveQuality(positions, indices);
            
            // Project vertices back to SDF surface
            if (sdfEvaluator && sdfGradientEvaluator)
            {
                for (std::size_t iter = 0; iter < projectionIterations; ++iter)
                {
                    for (std::size_t i = 0; i < positions.size(); ++i)
                    {
                        float const sdf = sdfEvaluator(positions[i]);
                        if (std::abs(sdf) > 1e-6F)
                        {
                            Eigen::Vector3f const grad = sdfGradientEvaluator(positions[i]);
                            positions[i] -= sdf * grad;
                            normals[i] = -grad; // Update normal
                        }
                    }
                }
            }
            
            return flips;
        }
        
      private:
        Config m_config{};
        
        /// Compute min and max angles of a triangle in degrees
        [[nodiscard]] static std::pair<float, float> computeTriangleAngles(
            Eigen::Vector3f const & v0,
            Eigen::Vector3f const & v1,
            Eigen::Vector3f const & v2)
        {
            Eigen::Vector3f const e0 = (v1 - v0).normalized();
            Eigen::Vector3f const e1 = (v2 - v1).normalized();
            Eigen::Vector3f const e2 = (v0 - v2).normalized();
            
            // Angles at each vertex
            float const a0 = std::acos(std::clamp(-e2.dot(e0), -1.0F, 1.0F)) * 180.0F / 3.14159265F;
            float const a1 = std::acos(std::clamp(-e0.dot(e1), -1.0F, 1.0F)) * 180.0F / 3.14159265F;
            float const a2 = std::acos(std::clamp(-e1.dot(e2), -1.0F, 1.0F)) * 180.0F / 3.14159265F;
            
            return {std::min({a0, a1, a2}), std::max({a0, a1, a2})};
        }
        
        /// Compute aspect ratio of a triangle (0 = degenerate, 1 = equilateral)
        [[nodiscard]] static float computeAspectRatio(
            Eigen::Vector3f const & v0,
            Eigen::Vector3f const & v1,
            Eigen::Vector3f const & v2)
        {
            float const l0 = (v1 - v0).norm();
            float const l1 = (v2 - v1).norm();
            float const l2 = (v0 - v2).norm();
            
            float const maxLen = std::max({l0, l1, l2});
            float const minLen = std::min({l0, l1, l2});
            
            if (maxLen < 1e-10F)
            {
                return 0.0F;
            }
            
            return minLen / maxLen;
        }
        
        /// Compute minimum angle of a triangle (higher is better, 60° is ideal)
        [[nodiscard]] static float computeMinAngle(
            Eigen::Vector3f const & v0,
            Eigen::Vector3f const & v1,
            Eigen::Vector3f const & v2)
        {
            return computeTriangleAngles(v0, v1, v2).first;
        }
        
        /// One pass of edge flipping optimization
        [[nodiscard]] std::size_t edgeFlipPass(
            std::vector<Eigen::Vector3f> const & positions,
            std::vector<std::uint32_t> & indices) const
        {
            std::size_t const numTriangles = indices.size() / 3U;
            
            // Build edge to triangle adjacency map
            // Key: edge (min_idx << 32 | max_idx)
            // Value: list of triangle indices
            std::unordered_map<std::uint64_t, std::vector<std::size_t>> edgeToTriangles;
            
            auto makeEdgeKey = [](std::uint32_t a, std::uint32_t b) -> std::uint64_t
            {
                if (a > b) std::swap(a, b);
                return (static_cast<std::uint64_t>(a) << 32) | b;
            };
            
            for (std::size_t t = 0; t < numTriangles; ++t)
            {
                std::uint32_t const i0 = indices[t * 3 + 0];
                std::uint32_t const i1 = indices[t * 3 + 1];
                std::uint32_t const i2 = indices[t * 3 + 2];
                
                edgeToTriangles[makeEdgeKey(i0, i1)].push_back(t);
                edgeToTriangles[makeEdgeKey(i1, i2)].push_back(t);
                edgeToTriangles[makeEdgeKey(i2, i0)].push_back(t);
            }
            
            std::size_t flips = 0U;
            std::unordered_set<std::uint64_t> processedEdges;
            
            for (auto const & [edgeKey, triangles] : edgeToTriangles)
            {
                // Only process edges shared by exactly 2 triangles
                if (triangles.size() != 2U)
                {
                    continue;
                }
                
                if (processedEdges.count(edgeKey))
                {
                    continue;
                }
                
                std::size_t const t0 = triangles[0];
                std::size_t const t1 = triangles[1];
                
                // Get the shared edge vertices
                std::uint32_t const edgeA = static_cast<std::uint32_t>(edgeKey >> 32);
                std::uint32_t const edgeB = static_cast<std::uint32_t>(edgeKey & 0xFFFFFFFF);
                
                // Find the opposite vertices in each triangle
                std::uint32_t oppA = 0U, oppB = 0U;
                for (int i = 0; i < 3; ++i)
                {
                    std::uint32_t const idx = indices[t0 * 3 + i];
                    if (idx != edgeA && idx != edgeB)
                    {
                        oppA = idx;
                        break;
                    }
                }
                for (int i = 0; i < 3; ++i)
                {
                    std::uint32_t const idx = indices[t1 * 3 + i];
                    if (idx != edgeA && idx != edgeB)
                    {
                        oppB = idx;
                        break;
                    }
                }
                
                // Skip if opposite vertices are the same (shouldn't happen)
                if (oppA == oppB)
                {
                    continue;
                }
                
                // Current triangles: (edgeA, edgeB, oppA) and (edgeA, oppB, edgeB) or similar
                // After flip: (edgeA, oppB, oppA) and (edgeB, oppA, oppB)
                
                // Compute current quality (minimum of minimum angles)
                Eigen::Vector3f const & pA = positions[edgeA];
                Eigen::Vector3f const & pB = positions[edgeB];
                Eigen::Vector3f const & pOppA = positions[oppA];
                Eigen::Vector3f const & pOppB = positions[oppB];
                
                float const currentMinAngle = std::min(
                    computeMinAngle(pA, pB, pOppA),
                    computeMinAngle(pA, pOppB, pB));
                
                // Compute flipped quality
                float const flippedMinAngle = std::min(
                    computeMinAngle(pA, pOppB, pOppA),
                    computeMinAngle(pB, pOppA, pOppB));
                
                // Check if flip would create inverted triangles
                Eigen::Vector3f const oldNormal0 = (pB - pA).cross(pOppA - pA);
                Eigen::Vector3f const newNormal0 = (pOppB - pA).cross(pOppA - pA);
                Eigen::Vector3f const newNormal1 = (pOppA - pB).cross(pOppB - pB);
                
                bool const wouldInvert = oldNormal0.dot(newNormal0) < 0.0F || 
                                         oldNormal0.dot(newNormal1) < 0.0F;
                
                // Flip if it improves quality and doesn't invert
                if (flippedMinAngle > currentMinAngle + 1.0F && !wouldInvert)  // +1° hysteresis
                {
                    // Perform the flip
                    // Find and update the triangles
                    // Triangle 0: replace edge with (edgeA, oppB, oppA)
                    // Triangle 1: replace edge with (edgeB, oppA, oppB)
                    
                    indices[t0 * 3 + 0] = edgeA;
                    indices[t0 * 3 + 1] = oppB;
                    indices[t0 * 3 + 2] = oppA;
                    
                    indices[t1 * 3 + 0] = edgeB;
                    indices[t1 * 3 + 1] = oppA;
                    indices[t1 * 3 + 2] = oppB;
                    
                    ++flips;
                    
                    // Mark all edges of both triangles as needing re-evaluation
                    processedEdges.insert(edgeKey);
                    processedEdges.insert(makeEdgeKey(edgeA, oppA));
                    processedEdges.insert(makeEdgeKey(edgeA, oppB));
                    processedEdges.insert(makeEdgeKey(edgeB, oppA));
                    processedEdges.insert(makeEdgeKey(edgeB, oppB));
                    processedEdges.insert(makeEdgeKey(oppA, oppB));
                }
            }
            
            return flips;
        }
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
