#include "MeshSimplification.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace gladius::compute
{
    // ============================================================================
    // Quadric Implementation
    // ============================================================================

    Quadric Quadric::fromPlane(Eigen::Vector3f const & normal, float d)
    {
        // Plane equation: n.x * x + n.y * y + n.z * z + d = 0
        // Quadric matrix: p * p^T where p = [n.x, n.y, n.z, d]
        Quadric q;
        float const a = normal.x();
        float const b = normal.y();
        float const c = normal.z();

        // Row 0: [a*a, a*b, a*c, a*d]
        q.data[0] = a * a;
        q.data[1] = a * b;
        q.data[2] = a * c;
        q.data[3] = a * d;

        // Row 1: [    b*b, b*c, b*d]
        q.data[4] = b * b;
        q.data[5] = b * c;
        q.data[6] = b * d;

        // Row 2: [        c*c, c*d]
        q.data[7] = c * c;
        q.data[8] = c * d;

        // Row 3: [            d*d]
        q.data[9] = d * d;

        return q;
    }

    Quadric Quadric::fromTriangle(Eigen::Vector3f const & v0,
                                   Eigen::Vector3f const & v1,
                                   Eigen::Vector3f const & v2)
    {
        Eigen::Vector3f const edge1 = v1 - v0;
        Eigen::Vector3f const edge2 = v2 - v0;
        Eigen::Vector3f normal = edge1.cross(edge2);

        float const length = normal.norm();
        if (length < 1e-10F)
        {
            // Degenerate triangle - return zero quadric
            return Quadric{};
        }

        normal /= length;
        float const d = -normal.dot(v0);

        return fromPlane(normal, d);
    }

    Quadric & Quadric::operator+=(Quadric const & other)
    {
        for (int i = 0; i < 10; ++i)
        {
            data[i] += other.data[i];
        }
        return *this;
    }

    Quadric Quadric::operator+(Quadric const & other) const
    {
        Quadric result = *this;
        result += other;
        return result;
    }

    float Quadric::evaluate(Eigen::Vector3f const & v) const
    {
        // Compute v^T * Q * v for homogeneous coordinates [v.x, v.y, v.z, 1]
        // Q is symmetric 4x4 matrix stored as upper triangle
        //
        // Full expansion:
        // data[0]*x*x + 2*data[1]*x*y + 2*data[2]*x*z + 2*data[3]*x +
        // data[4]*y*y + 2*data[5]*y*z + 2*data[6]*y +
        // data[7]*z*z + 2*data[8]*z +
        // data[9]

        float const x = v.x();
        float const y = v.y();
        float const z = v.z();

        return data[0] * x * x + 2.0F * data[1] * x * y + 2.0F * data[2] * x * z +
               2.0F * data[3] * x + data[4] * y * y + 2.0F * data[5] * y * z +
               2.0F * data[6] * y + data[7] * z * z + 2.0F * data[8] * z + data[9];
    }

    std::optional<Eigen::Vector3f> Quadric::optimalVertex() const
    {
        // Find v that minimizes v^T * Q * v
        // Taking derivative and setting to zero: A * v = -b
        // where A is 3x3 top-left block and b is top-right 3x1 column

        Eigen::Matrix3f A = getA();
        Eigen::Vector3f b = getB();

        // Use SVD for numerical stability with potentially singular matrices
        Eigen::JacobiSVD<Eigen::Matrix3f> svd(A, Eigen::ComputeFullU | Eigen::ComputeFullV);

        // Check condition number
        auto const & singularValues = svd.singularValues();
        if (singularValues(0) < 1e-10F)
        {
            // Matrix is essentially zero - no valid solution
            return std::nullopt;
        }

        float const conditionNumber = singularValues(0) / singularValues(2);
        if (conditionNumber > 1e6F)
        {
            // Matrix is too ill-conditioned
            return std::nullopt;
        }

        Eigen::Vector3f const solution = svd.solve(-b);
        return solution;
    }

    Eigen::Matrix3f Quadric::getA() const
    {
        Eigen::Matrix3f A;
        A(0, 0) = data[0];
        A(0, 1) = data[1];
        A(0, 2) = data[2];
        A(1, 0) = data[1];
        A(1, 1) = data[4];
        A(1, 2) = data[5];
        A(2, 0) = data[2];
        A(2, 1) = data[5];
        A(2, 2) = data[7];
        return A;
    }

    Eigen::Vector3f Quadric::getB() const
    {
        return Eigen::Vector3f(data[3], data[6], data[8]);
    }

    float Quadric::getC() const
    {
        return data[9];
    }

    void Quadric::reset()
    {
        for (int i = 0; i < 10; ++i)
        {
            data[i] = 0.0F;
        }
    }

    // ============================================================================
    // QemMeshSimplifier Implementation
    // ============================================================================

    void QemMeshSimplifier::setConfig(QemSimplificationConfig const & config)
    {
        m_config = config;
    }

    void QemMeshSimplifier::setGpuSdfEvaluator(GpuSdfEvaluator evaluator)
    {
        m_gpuSdfEvaluator = std::move(evaluator);
    }

    void QemMeshSimplifier::setGpuSdfGradientEvaluator(GpuSdfGradientEvaluator evaluator)
    {
        m_gpuSdfGradientEvaluator = std::move(evaluator);
    }

    void QemMeshSimplifier::setProgressCallback(SimplificationProgressCallback callback)
    {
        m_progressCallback = std::move(callback);
    }

    void QemMeshSimplifier::buildVertexQuadrics(std::vector<Eigen::Vector3f> const & positions,
                                                 std::vector<std::uint32_t> const & indices)
    {
        m_vertexQuadrics.clear();
        m_vertexQuadrics.resize(positions.size());

        std::size_t const numTriangles = indices.size() / 3;
        for (std::size_t t = 0; t < numTriangles; ++t)
        {
            std::uint32_t const i0 = indices[t * 3 + 0];
            std::uint32_t const i1 = indices[t * 3 + 1];
            std::uint32_t const i2 = indices[t * 3 + 2];

            // Skip degenerate triangles
            if (i0 == i1 || i1 == i2 || i2 == i0)
            {
                continue;
            }

            Quadric const triQuadric =
                Quadric::fromTriangle(positions[i0], positions[i1], positions[i2]);

            m_vertexQuadrics[i0] += triQuadric;
            m_vertexQuadrics[i1] += triQuadric;
            m_vertexQuadrics[i2] += triQuadric;
        }
    }

    void QemMeshSimplifier::recalculateVertexQuadric(
        std::uint32_t vertexIndex,
        std::vector<Eigen::Vector3f> const & positions,
        std::vector<std::uint32_t> const & indices,
        std::vector<std::vector<std::size_t>> const & vertexToTriangles)
    {
        m_vertexQuadrics[vertexIndex].reset();

        for (std::size_t triIndex : vertexToTriangles[vertexIndex])
        {
            std::uint32_t const i0 = indices[triIndex * 3 + 0];
            std::uint32_t const i1 = indices[triIndex * 3 + 1];
            std::uint32_t const i2 = indices[triIndex * 3 + 2];

            // Skip degenerate triangles
            if (i0 == i1 || i1 == i2 || i2 == i0)
            {
                continue;
            }

            Quadric const triQuadric =
                Quadric::fromTriangle(positions[i0], positions[i1], positions[i2]);

            m_vertexQuadrics[vertexIndex] += triQuadric;
        }
    }

    std::vector<CollapseCandidate> QemMeshSimplifier::collectCandidates(
        std::vector<Eigen::Vector3f> const & positions,
        std::vector<Eigen::Vector3f> const & normals,
        std::vector<std::uint32_t> const & indices) const
    {
        // Build edge map: edge -> list of triangles that use it
        struct EdgeHash
        {
            std::size_t operator()(std::pair<std::uint32_t, std::uint32_t> const & e) const
            {
                return std::hash<std::uint64_t>{}(
                    static_cast<std::uint64_t>(e.first) << 32U | e.second);
            }
        };

        std::unordered_map<std::pair<std::uint32_t, std::uint32_t>,
                           std::vector<std::size_t>,
                           EdgeHash>
            edgeToTriangles;

        std::size_t const numTriangles = indices.size() / 3;
        for (std::size_t t = 0; t < numTriangles; ++t)
        {
            std::uint32_t const i0 = indices[t * 3 + 0];
            std::uint32_t const i1 = indices[t * 3 + 1];
            std::uint32_t const i2 = indices[t * 3 + 2];

            if (i0 == i1 || i1 == i2 || i2 == i0)
            {
                continue;
            }

            auto addEdge = [&](std::uint32_t a, std::uint32_t b)
            {
                auto const edge = std::minmax(a, b);
                edgeToTriangles[edge].push_back(t);
            };

            addEdge(i0, i1);
            addEdge(i1, i2);
            addEdge(i2, i0);
        }

        std::vector<CollapseCandidate> candidates;
        candidates.reserve(edgeToTriangles.size());

        for (auto const & [edge, triangles] : edgeToTriangles)
        {
            CollapseCandidate candidate;
            candidate.vertexA = edge.first;
            candidate.vertexB = edge.second;

            // Compute edge length
            candidate.edgeLength = (positions[candidate.vertexA] - positions[candidate.vertexB]).norm();

            // Boundary edge: only 1 adjacent triangle
            candidate.isBoundaryEdge = (triangles.size() == 1);

            // Sharp feature detection: check angle between adjacent triangle normals
            // Also collect neighbor vertices to compute neighbor edge lengths
            std::unordered_set<std::uint32_t> neighborVertices;
            if (triangles.size() == 2)
            {
                auto computeTriangleNormal = [&](std::size_t triIdx) -> Eigen::Vector3f
                {
                    std::uint32_t const ti0 = indices[triIdx * 3 + 0];
                    std::uint32_t const ti1 = indices[triIdx * 3 + 1];
                    std::uint32_t const ti2 = indices[triIdx * 3 + 2];
                    Eigen::Vector3f const e1 = positions[ti1] - positions[ti0];
                    Eigen::Vector3f const e2 = positions[ti2] - positions[ti0];
                    return e1.cross(e2).normalized();
                };

                Eigen::Vector3f const n0 = computeTriangleNormal(triangles[0]);
                Eigen::Vector3f const n1 = computeTriangleNormal(triangles[1]);
                float const normalDot = n0.dot(n1);

                candidate.isSharpFeatureEdge = (normalDot < m_config.sharpEdgeAngleThreshold);
                
                // Collect vertices from adjacent triangles (excluding edge vertices)
                for (std::size_t triIdx : triangles)
                {
                    for (std::size_t vi = 0; vi < 3; ++vi)
                    {
                        std::uint32_t const v = indices[triIdx * 3 + vi];
                        if (v != candidate.vertexA && v != candidate.vertexB)
                        {
                            neighborVertices.insert(v);
                        }
                    }
                }
            }
            
            // Compute max neighbor edge length (edges from target position to neighbor vertices)
            // This helps detect if collapsing would create unusually long edges
            candidate.maxNeighborEdgeLength = candidate.edgeLength;
            for (std::uint32_t neighborV : neighborVertices)
            {
                float const neighborEdgeLen = (positions[neighborV] - positions[candidate.vertexA]).norm();
                candidate.maxNeighborEdgeLength = std::max(candidate.maxNeighborEdgeLength, neighborEdgeLen);
                float const neighborEdgeLen2 = (positions[neighborV] - positions[candidate.vertexB]).norm();
                candidate.maxNeighborEdgeLength = std::max(candidate.maxNeighborEdgeLength, neighborEdgeLen2);
            }

            // Compute combined quadric for edge
            Quadric const combinedQuadric =
                m_vertexQuadrics[candidate.vertexA] + m_vertexQuadrics[candidate.vertexB];

            // Compute edge midpoint as fallback/baseline
            Eigen::Vector3f const edgeMidpoint =
                (positions[candidate.vertexA] + positions[candidate.vertexB]) * 0.5F;
            
            // Find optimal target position from QEM
            auto optimalPos = combinedQuadric.optimalVertex();
            if (optimalPos.has_value())
            {
                Eigen::Vector3f const qemPos = optimalPos.value();
                
                // Check if QEM position is too far from edge midpoint
                // This prevents placing vertices far from the original surface
                float const qemDistance = (qemPos - edgeMidpoint).norm();
                
                // If QEM position is more than half the edge length away from midpoint,
                // use midpoint instead (QEM is likely ill-conditioned or placing vertex off-surface)
                if (qemDistance < candidate.edgeLength * 0.5F)
                {
                    candidate.targetPosition = qemPos;
                }
                else
                {
                    // QEM is pulling vertex too far - use midpoint for stability
                    candidate.targetPosition = edgeMidpoint;
                }
            }
            else
            {
                // Fall back to edge midpoint
                candidate.targetPosition = edgeMidpoint;
            }

            // Compute QEM error at target position
            candidate.qemError = combinedQuadric.evaluate(candidate.targetPosition);

            // SDF error and normal deviation will be filled in by GPU evaluation
            candidate.sdfError = 0.0F;
            candidate.edgeSdfError = 0.0F;
            candidate.normalDeviation = 0.0F;

            candidates.push_back(candidate);
        }

        return candidates;
    }

    void QemMeshSimplifier::evaluateSdfErrorsGpu(
        std::vector<CollapseCandidate> & candidates,
        std::vector<Eigen::Vector3f> const & meshPositions,
        std::vector<std::uint32_t> const & indices,
        std::vector<std::vector<std::size_t>> const & vertexToTriangles)
    {
        if (!m_gpuSdfEvaluator || candidates.empty())
        {
            return;
        }

        // Multi-point SDF sampling along edges for better curved surface handling
        std::size_t const sampleCount = std::max(std::size_t{1}, m_config.edgeSdfSampleCount);
        
        // Process in batches, accounting for multiple samples per edge
        for (std::size_t batchStart = 0; batchStart < candidates.size();
             batchStart += m_config.batchSize)
        {
            std::size_t const batchEnd =
                std::min(batchStart + m_config.batchSize, candidates.size());
            std::size_t const batchSize = batchEnd - batchStart;

            // Collect all sample positions for this batch
            // For each edge: target position + (sampleCount-1) points along edge
            std::vector<Eigen::Vector3f> positions;
            positions.reserve(batchSize * sampleCount);
            
            for (std::size_t i = batchStart; i < batchEnd; ++i)
            {
                auto const & candidate = candidates[i];
                
                // Always include target position first
                positions.push_back(candidate.targetPosition);
                
                // Sample points along the original edge (before collapse)
                // These detect if the edge cuts through high curvature areas
                if (sampleCount > 1)
                {
                    Eigen::Vector3f const & posA = meshPositions[candidate.vertexA];
                    Eigen::Vector3f const & posB = meshPositions[candidate.vertexB];
                    
                    for (std::size_t s = 1; s < sampleCount; ++s)
                    {
                        float const t = static_cast<float>(s) / static_cast<float>(sampleCount);
                        Eigen::Vector3f const samplePos = posA + t * (posB - posA);
                        positions.push_back(samplePos);
                    }
                }
            }

            // Evaluate SDF on GPU for all sample points
            std::vector<float> sdfValues = m_gpuSdfEvaluator(positions);

            // Process results: target SDF and max edge deviation
            std::size_t sampleIdx = 0;
            for (std::size_t i = batchStart; i < batchEnd; ++i)
            {
                // First sample is target position
                candidates[i].sdfError = std::abs(sdfValues[sampleIdx++]);
                
                // Remaining samples are along the edge - find max deviation
                float maxEdgeSdf = 0.0F;
                for (std::size_t s = 1; s < sampleCount; ++s)
                {
                    maxEdgeSdf = std::max(maxEdgeSdf, std::abs(sdfValues[sampleIdx++]));
                }
                candidates[i].edgeSdfError = maxEdgeSdf;
            }
        }

        // Evaluate normal deviation if gradient evaluator is available
        if (m_gpuSdfGradientEvaluator && m_config.normalDeviationWeight > 0.0F)
        {
            // For each candidate, compute the maximum normal deviation for affected triangles
            // after the collapse. We evaluate SDF gradients at triangle centroids.
            
            for (auto & candidate : candidates)
            {
                std::uint32_t const vA = candidate.vertexA;
                std::uint32_t const vB = candidate.vertexB;
                Eigen::Vector3f const & newPos = candidate.targetPosition;
                
                // Collect triangles that will be modified (not removed) by this collapse
                std::vector<std::size_t> affectedTriangles;
                std::unordered_set<std::size_t> allTriangles;
                
                for (std::size_t t : vertexToTriangles[vA])
                {
                    allTriangles.insert(t);
                }
                for (std::size_t t : vertexToTriangles[vB])
                {
                    allTriangles.insert(t);
                }
                
                for (std::size_t triIdx : allTriangles)
                {
                    std::uint32_t const i0 = indices[triIdx * 3 + 0];
                    std::uint32_t const i1 = indices[triIdx * 3 + 1];
                    std::uint32_t const i2 = indices[triIdx * 3 + 2];
                    
                    // Check if triangle will be removed (contains both vertices)
                    bool const hasA = (i0 == vA || i1 == vA || i2 == vA);
                    bool const hasB = (i0 == vB || i1 == vB || i2 == vB);
                    
                    if (hasA && hasB)
                    {
                        continue;  // Triangle will be removed
                    }
                    
                    if (hasA || hasB)
                    {
                        affectedTriangles.push_back(triIdx);
                    }
                }
                
                if (affectedTriangles.empty())
                {
                    candidate.normalDeviation = 0.0F;
                    continue;
                }
                
                // Compute centroids of affected triangles after collapse
                std::vector<Eigen::Vector3f> centroids;
                std::vector<Eigen::Vector3f> triangleNormals;
                centroids.reserve(affectedTriangles.size());
                triangleNormals.reserve(affectedTriangles.size());
                
                for (std::size_t triIdx : affectedTriangles)
                {
                    std::uint32_t const i0 = indices[triIdx * 3 + 0];
                    std::uint32_t const i1 = indices[triIdx * 3 + 1];
                    std::uint32_t const i2 = indices[triIdx * 3 + 2];
                    
                    // Get positions after collapse
                    auto getPos = [&](std::uint32_t idx) -> Eigen::Vector3f
                    {
                        if (idx == vA || idx == vB)
                        {
                            return newPos;
                        }
                        return meshPositions[idx];
                    };
                    
                    Eigen::Vector3f const p0 = getPos(i0);
                    Eigen::Vector3f const p1 = getPos(i1);
                    Eigen::Vector3f const p2 = getPos(i2);
                    
                    // Compute centroid
                    Eigen::Vector3f const centroid = (p0 + p1 + p2) / 3.0F;
                    centroids.push_back(centroid);
                    
                    // Compute triangle normal
                    Eigen::Vector3f const triNormal = (p1 - p0).cross(p2 - p0).normalized();
                    triangleNormals.push_back(triNormal);
                }
                
                // Evaluate SDF gradients at centroids
                std::vector<Eigen::Vector3f> sdfNormals = m_gpuSdfGradientEvaluator(centroids);
                
                // Compute maximum normal deviation
                float maxDeviation = 0.0F;
                for (std::size_t i = 0; i < centroids.size(); ++i)
                {
                    float const dotProduct = std::abs(triangleNormals[i].dot(sdfNormals[i]));
                    float const deviation = 1.0F - dotProduct;  // 0 = perfect alignment, 1 = perpendicular
                    maxDeviation = std::max(maxDeviation, deviation);
                }
                
                candidate.normalDeviation = maxDeviation;
            }
        }

        // Compute combined error for all candidates
        // Also collect statistics for debugging
        std::size_t rejectedBySdf = 0;
        std::size_t rejectedByEdgeSdf = 0;
        std::size_t rejectedByQem = 0;
        std::size_t rejectedByNormal = 0;
        std::size_t rejectedByBoundary = 0;
        std::size_t rejectedByEdgeLength = 0;
        
        for (auto & candidate : candidates)
        {
            // Count rejection reasons (for debugging)
            if (candidate.sdfError > m_config.maxSdfError)
            {
                ++rejectedBySdf;
            }
            if (candidate.edgeSdfError > m_config.maxSdfError)
            {
                ++rejectedByEdgeSdf;
            }
            if (candidate.qemError > m_config.maxQemError)
            {
                ++rejectedByQem;
            }
            if (candidate.normalDeviation > m_config.maxNormalDeviation)
            {
                ++rejectedByNormal;
            }
            if (candidate.isBoundaryEdge)
            {
                ++rejectedByBoundary;
            }
            
            // Check edge length constraint: would collapse create edges much longer than neighbors?
            // After collapse, edges from target to neighbor vertices shouldn't be too long
            float edgeLengthPenalty = 0.0F;
            if (candidate.maxNeighborEdgeLength > 0.0F)
            {
                // Compute average neighbor edge length as reference
                float const avgNeighborLen = candidate.maxNeighborEdgeLength;
                
                // New edges will be from targetPosition to each neighbor vertex
                // The max such length is approximately: distance(target, farthest neighbor)
                // For now, use a heuristic: if edge being collapsed is very short compared to neighbors,
                // collapsing is good. If it's already long, be more careful.
                float const edgeLengthRatio = candidate.edgeLength / avgNeighborLen;
                
                // Penalize collapsing already-long edges (they span larger areas)
                if (edgeLengthRatio > m_config.maxEdgeLengthRatio)
                {
                    edgeLengthPenalty = (edgeLengthRatio - m_config.maxEdgeLengthRatio) * 0.5F;
                    ++rejectedByEdgeLength;
                }
            }
            
            float error = candidate.sdfError * m_config.sdfErrorWeight +
                          candidate.edgeSdfError * m_config.edgeSdfErrorWeight +
                          candidate.qemError * m_config.qemErrorWeight +
                          candidate.normalDeviation * m_config.normalDeviationWeight +
                          edgeLengthPenalty;

            // Apply penalties for special edges
            if (candidate.isBoundaryEdge)
            {
                error *= m_config.boundaryEdgeLockFactor;
            }
            if (candidate.isSharpFeatureEdge)
            {
                error *= 10.0F; // Significant penalty but not absolute lock
            }

            candidate.combinedError = error;
        }
        
        std::cout << "Candidate rejection stats (of " << candidates.size() << " total):" << std::endl;
        std::cout << "  SDF error > " << m_config.maxSdfError << ": " << rejectedBySdf << std::endl;
        std::cout << "  Edge SDF error > " << m_config.maxSdfError << ": " << rejectedByEdgeSdf << std::endl;
        std::cout << "  Edge length ratio > " << m_config.maxEdgeLengthRatio << ": " << rejectedByEdgeLength << std::endl;
        std::cout << "  QEM error > " << m_config.maxQemError << ": " << rejectedByQem << std::endl;
        std::cout << "  Normal deviation > " << m_config.maxNormalDeviation << ": " << rejectedByNormal << std::endl;
        std::cout << "  Boundary edges: " << rejectedByBoundary << std::endl;
    }

    bool QemMeshSimplifier::wouldCreateDegenerateTriangles(
        CollapseCandidate const & candidate,
        std::vector<Eigen::Vector3f> const & positions,
        std::vector<std::uint32_t> const & indices,
        std::vector<std::vector<std::size_t>> const & vertexToTriangles) const
    {
        std::uint32_t const vA = candidate.vertexA;
        std::uint32_t const vB = candidate.vertexB;
        Eigen::Vector3f const & newPos = candidate.targetPosition;

        // Check all triangles incident to either vertex
        std::unordered_set<std::size_t> trianglesToCheck;
        for (std::size_t t : vertexToTriangles[vA])
        {
            trianglesToCheck.insert(t);
        }
        for (std::size_t t : vertexToTriangles[vB])
        {
            trianglesToCheck.insert(t);
        }

        // Minimum normal preservation threshold (cos of max allowed angle deviation)
        // PrusaSlicer uses 0.2 (~80 degrees), but we use a more conservative value
        // to prevent visible normal discontinuities on smooth surfaces.
        // 0.2 = ~80 degrees, 0.5 = 60 degrees, 0.7 = ~45 degrees
        float constexpr kMinNormalDotProduct = 0.5F;  // ~60 degrees max (relaxed from 0.9)
        
        // Minimum triangle quality (aspect ratio) threshold
        // Ratio of shortest edge to longest edge, below this is too thin
        float constexpr kMinAspectRatio = 0.15F;
        
        // Minimum area relative to original triangle
        float constexpr kMinAreaRatio = 0.05F;

        for (std::size_t triIdx : trianglesToCheck)
        {
            std::uint32_t i0 = indices[triIdx * 3 + 0];
            std::uint32_t i1 = indices[triIdx * 3 + 1];
            std::uint32_t i2 = indices[triIdx * 3 + 2];

            // Skip triangles that will be removed (contain both vA and vB)
            bool const hasA = (i0 == vA || i1 == vA || i2 == vA);
            bool const hasB = (i0 == vB || i1 == vB || i2 == vB);
            if (hasA && hasB)
            {
                continue;
            }

            // Get original vertex positions
            Eigen::Vector3f const oldP0 = positions[i0];
            Eigen::Vector3f const oldP1 = positions[i1];
            Eigen::Vector3f const oldP2 = positions[i2];

            // Get new vertex positions after collapse
            auto getNewPos = [&](std::uint32_t idx) -> Eigen::Vector3f
            {
                if (idx == vA || idx == vB)
                {
                    return newPos;
                }
                return positions[idx];
            };

            Eigen::Vector3f const p0 = getNewPos(i0);
            Eigen::Vector3f const p1 = getNewPos(i1);
            Eigen::Vector3f const p2 = getNewPos(i2);

            // Compute new triangle properties
            Eigen::Vector3f const e1 = p1 - p0;
            Eigen::Vector3f const e2 = p2 - p0;
            Eigen::Vector3f const e3 = p2 - p1;
            Eigen::Vector3f const newNormalUnnorm = e1.cross(e2);
            float const newArea = newNormalUnnorm.norm();

            // Check for degenerate triangle (near-zero area)
            if (newArea < 1e-10F)
            {
                return true; // Degenerate
            }

            Eigen::Vector3f const newNormal = newNormalUnnorm / newArea;

            // Check aspect ratio (triangle quality)
            float const len1 = e1.norm();
            float const len2 = e2.norm();
            float const len3 = e3.norm();
            float const maxLen = std::max({len1, len2, len3});
            float const minLen = std::min({len1, len2, len3});
            
            if (maxLen > 1e-10F && (minLen / maxLen) < kMinAspectRatio)
            {
                return true; // Too thin/elongated
            }
            
            // PrusaSlicer "triangle beauty" check:
            // Check for very thin triangles by looking at edge angles.
            // If normalized edge vectors are nearly parallel (high dot product),
            // the triangle is degenerate/very thin.
            // threshold of 0.999 corresponds to ~2.5 degree angle between edges
            float constexpr kTriangleBeautyThreshold = 0.999F;
            
            if (len1 > 1e-10F && len2 > 1e-10F)
            {
                float const edgeDot = std::abs(e1.normalized().dot(e2.normalized()));
                if (edgeDot > kTriangleBeautyThreshold)
                {
                    return true; // Nearly degenerate thin triangle
                }
            }
            if (len1 > 1e-10F && len3 > 1e-10F)
            {
                float const edgeDot = std::abs(e1.normalized().dot(e3.normalized()));
                if (edgeDot > kTriangleBeautyThreshold)
                {
                    return true; // Nearly degenerate thin triangle
                }
            }
            if (len2 > 1e-10F && len3 > 1e-10F)
            {
                float const edgeDot = std::abs(e2.normalized().dot(e3.normalized()));
                if (edgeDot > kTriangleBeautyThreshold)
                {
                    return true; // Nearly degenerate thin triangle
                }
            }

            // For triangles that will be modified (contain one of the collapsed vertices)
            if (hasA || hasB)
            {
                Eigen::Vector3f const oldNormalUnnorm = (oldP1 - oldP0).cross(oldP2 - oldP0);
                float const oldArea = oldNormalUnnorm.norm();
                
                if (oldArea > 1e-10F)
                {
                    Eigen::Vector3f const oldNormal = oldNormalUnnorm / oldArea;
                    float const normalDot = oldNormal.dot(newNormal);

                    // Check for inverted triangle (normal flipped)
                    if (normalDot < 0.0F)
                    {
                        return true; // Inverted
                    }

                    // Check for excessive normal deviation
                    // This is the key check that prevents "lumpy" surfaces!
                    if (normalDot < kMinNormalDotProduct)
                    {
                        return true; // Normal changed too much
                    }

                    // Check for excessive area reduction
                    if ((newArea / oldArea) < kMinAreaRatio)
                    {
                        return true; // Triangle shrunk too much
                    }
                }
            }
        }

        // PrusaSlicer-inspired "create_no_volume" check:
        // After collapse, check that no two triangles that share an edge would form
        // a zero-volume fold (i.e., triangles with opposite orientations sharing an edge).
        // This prevents the mesh from folding onto itself.
        
        // Build a map of edges to triangles that will exist after collapse
        std::unordered_map<std::uint64_t, std::vector<std::pair<std::size_t, Eigen::Vector3f>>> edgeToTriangleNormals;
        
        auto makeEdgeKey = [](std::uint32_t a, std::uint32_t b) -> std::uint64_t
        {
            if (a > b) std::swap(a, b);
            return (static_cast<std::uint64_t>(a) << 32) | b;
        };
        
        for (std::size_t triIdx : trianglesToCheck)
        {
            std::uint32_t i0 = indices[triIdx * 3 + 0];
            std::uint32_t i1 = indices[triIdx * 3 + 1];
            std::uint32_t i2 = indices[triIdx * 3 + 2];
            
            // Map collapsed vertices
            if (i0 == vB) i0 = vA;
            if (i1 == vB) i1 = vA;
            if (i2 == vB) i2 = vA;
            
            // Skip degenerate triangles (those that collapse because they contain both vA and vB)
            if (i0 == i1 || i1 == i2 || i2 == i0)
            {
                continue;
            }
            
            // Get positions after collapse
            auto getPosAfterCollapse = [&](std::uint32_t idx) -> Eigen::Vector3f
            {
                if (idx == vA || idx == vB)
                {
                    return newPos;
                }
                return positions[idx];
            };
            
            Eigen::Vector3f const p0 = getPosAfterCollapse(i0);
            Eigen::Vector3f const p1 = getPosAfterCollapse(i1);
            Eigen::Vector3f const p2 = getPosAfterCollapse(i2);
            
            Eigen::Vector3f const normal = (p1 - p0).cross(p2 - p0).normalized();
            
            // Add each edge of this triangle to the map
            std::uint64_t const edge01 = makeEdgeKey(i0, i1);
            std::uint64_t const edge12 = makeEdgeKey(i1, i2);
            std::uint64_t const edge20 = makeEdgeKey(i2, i0);
            
            edgeToTriangleNormals[edge01].emplace_back(triIdx, normal);
            edgeToTriangleNormals[edge12].emplace_back(triIdx, normal);
            edgeToTriangleNormals[edge20].emplace_back(triIdx, normal);
        }
        
        // Check for zero-volume folds: if two triangles share an edge and have
        // nearly opposite normals, they form a fold
        float constexpr kNoVolumeDotThreshold = -0.5F; // normals more opposite than ~120 degrees
        
        for (auto const & [edge, triangles] : edgeToTriangleNormals)
        {
            if (triangles.size() >= 2)
            {
                // Check all pairs of triangles sharing this edge
                for (std::size_t i = 0; i < triangles.size(); ++i)
                {
                    for (std::size_t j = i + 1; j < triangles.size(); ++j)
                    {
                        float const dot = triangles[i].second.dot(triangles[j].second);
                        if (dot < kNoVolumeDotThreshold)
                        {
                            // These triangles would form a fold
                            return true;
                        }
                    }
                }
            }
        }

        return false;
    }

    bool QemMeshSimplifier::performCollapse(
        CollapseCandidate const & candidate,
        std::vector<Eigen::Vector3f> & positions,
        std::vector<Eigen::Vector3f> & normals,
        std::vector<std::uint32_t> & indices,
        std::vector<std::vector<std::size_t>> & vertexToTriangles,
        std::vector<bool> & vertexRemoved)
    {
        std::uint32_t const vA = candidate.vertexA;
        std::uint32_t const vB = candidate.vertexB;

        // Check if already removed
        if (vertexRemoved[vA] || vertexRemoved[vB])
        {
            return false;
        }

        // Check for degenerate results
        if (wouldCreateDegenerateTriangles(candidate, positions, indices, vertexToTriangles))
        {
            return false;
        }

        // Move vA to target position
        positions[vA] = candidate.targetPosition;

        // Compute new normal as average of incident faces (will be recalculated properly later)
        normals[vA] = (normals[vA] + normals[vB]).normalized();

        // Mark vB as removed
        vertexRemoved[vB] = true;

        // Update all references from vB to vA in index buffer
        // and track which triangles become degenerate
        std::vector<std::size_t> trianglesToRemove;

        for (std::size_t triIdx : vertexToTriangles[vB])
        {
            std::uint32_t & i0 = indices[triIdx * 3 + 0];
            std::uint32_t & i1 = indices[triIdx * 3 + 1];
            std::uint32_t & i2 = indices[triIdx * 3 + 2];

            // Replace vB with vA
            if (i0 == vB)
                i0 = vA;
            if (i1 == vB)
                i1 = vA;
            if (i2 == vB)
                i2 = vA;

            // Check if triangle became degenerate
            if (i0 == i1 || i1 == i2 || i2 == i0)
            {
                trianglesToRemove.push_back(triIdx);
            }
            else
            {
                // Add this triangle to vA's list
                vertexToTriangles[vA].push_back(triIdx);
            }
        }

        // Clear vB's triangle list
        vertexToTriangles[vB].clear();

        // Remove degenerate triangles from vA's list
        auto & vATriangles = vertexToTriangles[vA];
        vATriangles.erase(
            std::remove_if(vATriangles.begin(),
                           vATriangles.end(),
                           [&](std::size_t t)
                           {
                               return std::find(trianglesToRemove.begin(),
                                                trianglesToRemove.end(),
                                                t) != trianglesToRemove.end();
                           }),
            vATriangles.end());

        // Recalculate quadric for vA from its new incident triangles
        if (m_config.recalculateQuadricsAfterCollapse)
        {
            recalculateVertexQuadric(vA, positions, indices, vertexToTriangles);

            // Also recalculate for neighbors
            std::unordered_set<std::uint32_t> neighbors;
            for (std::size_t triIdx : vertexToTriangles[vA])
            {
                neighbors.insert(indices[triIdx * 3 + 0]);
                neighbors.insert(indices[triIdx * 3 + 1]);
                neighbors.insert(indices[triIdx * 3 + 2]);
            }
            neighbors.erase(vA);

            for (std::uint32_t neighbor : neighbors)
            {
                if (!vertexRemoved[neighbor])
                {
                    recalculateVertexQuadric(neighbor, positions, indices, vertexToTriangles);
                }
            }
        }

        return true;
    }

    void QemMeshSimplifier::compactMesh(std::vector<Eigen::Vector3f> & positions,
                                         std::vector<Eigen::Vector3f> & normals,
                                         std::vector<std::uint32_t> & indices,
                                         std::vector<bool> const & vertexRemoved)
    {
        // Build vertex remapping
        std::vector<std::uint32_t> remap(positions.size(), UINT32_MAX);
        std::vector<Eigen::Vector3f> newPositions;
        std::vector<Eigen::Vector3f> newNormals;
        newPositions.reserve(positions.size());
        newNormals.reserve(normals.size());

        for (std::size_t i = 0; i < positions.size(); ++i)
        {
            if (!vertexRemoved[i])
            {
                remap[i] = static_cast<std::uint32_t>(newPositions.size());
                newPositions.push_back(positions[i]);
                newNormals.push_back(normals[i]);
            }
        }

        // Compact index buffer, removing degenerate triangles
        std::vector<std::uint32_t> newIndices;
        newIndices.reserve(indices.size());

        std::size_t const numTriangles = indices.size() / 3;
        for (std::size_t t = 0; t < numTriangles; ++t)
        {
            std::uint32_t const i0 = indices[t * 3 + 0];
            std::uint32_t const i1 = indices[t * 3 + 1];
            std::uint32_t const i2 = indices[t * 3 + 2];

            // Skip degenerate triangles
            if (i0 == i1 || i1 == i2 || i2 == i0)
            {
                continue;
            }

            std::uint32_t const r0 = remap[i0];
            std::uint32_t const r1 = remap[i1];
            std::uint32_t const r2 = remap[i2];

            // Skip if any vertex was removed
            if (r0 == UINT32_MAX || r1 == UINT32_MAX || r2 == UINT32_MAX)
            {
                continue;
            }

            newIndices.push_back(r0);
            newIndices.push_back(r1);
            newIndices.push_back(r2);
        }

        positions = std::move(newPositions);
        normals = std::move(newNormals);
        indices = std::move(newIndices);
    }

    std::size_t QemMeshSimplifier::simplify(std::vector<Eigen::Vector3f> & positions,
                                             std::vector<Eigen::Vector3f> & normals,
                                             std::vector<std::uint32_t> & indices)
    {
        if (positions.empty() || indices.size() < 3)
        {
            return 0;
        }

        std::size_t const initialTriangles = indices.size() / 3;
        std::size_t totalCollapsed = 0;

        std::cout << "QEM Simplification starting with " << initialTriangles << " triangles"
                  << std::endl;

        // Build initial vertex quadrics
        buildVertexQuadrics(positions, indices);

        for (std::size_t pass = 0; pass < m_config.maxPasses; ++pass)
        {
            std::size_t const currentTriangles = indices.size() / 3;

            // Check termination conditions
            if (m_config.targetTriangleCount.has_value() &&
                currentTriangles <= m_config.targetTriangleCount.value())
            {
                std::cout << "Target triangle count reached" << std::endl;
                break;
            }

            if (m_config.targetReductionPercent.has_value())
            {
                float const reduction =
                    100.0F * static_cast<float>(initialTriangles - currentTriangles) /
                    static_cast<float>(initialTriangles);
                if (reduction >= m_config.targetReductionPercent.value())
                {
                    std::cout << "Target reduction percentage reached" << std::endl;
                    break;
                }
            }

            // Build vertex-to-triangle adjacency
            std::vector<std::vector<std::size_t>> vertexToTriangles(positions.size());
            std::size_t const numTriangles = indices.size() / 3;
            for (std::size_t t = 0; t < numTriangles; ++t)
            {
                std::uint32_t const i0 = indices[t * 3 + 0];
                std::uint32_t const i1 = indices[t * 3 + 1];
                std::uint32_t const i2 = indices[t * 3 + 2];
                if (i0 != i1 && i1 != i2 && i2 != i0)
                {
                    vertexToTriangles[i0].push_back(t);
                    vertexToTriangles[i1].push_back(t);
                    vertexToTriangles[i2].push_back(t);
                }
            }

            // Collect edge collapse candidates
            auto candidates = collectCandidates(positions, normals, indices);

            if (candidates.empty())
            {
                std::cout << "No more collapse candidates" << std::endl;
                break;
            }

            // Evaluate SDF errors and normal deviations on GPU
            evaluateSdfErrorsGpu(candidates, positions, indices, vertexToTriangles);

            // Sort by combined error (lowest first)
            std::sort(candidates.begin(),
                      candidates.end(),
                      [](CollapseCandidate const & a, CollapseCandidate const & b)
                      { return a.combinedError < b.combinedError; });

            // Track removed vertices
            std::vector<bool> vertexRemoved(positions.size(), false);
            std::vector<bool> vertexTouched(positions.size(), false);

            std::size_t passCollapsed = 0;

            for (auto const & candidate : candidates)
            {
                // Skip if error exceeds bounds
                if (candidate.sdfError > m_config.maxSdfError)
                {
                    continue;
                }
                if (candidate.qemError > m_config.maxQemError)
                {
                    continue;
                }
                // Temporarily disabled: normal deviation check needs debugging
                // The gradient evaluator may not be returning correct values
                // if (candidate.normalDeviation > m_config.maxNormalDeviation)
                // {
                //     continue;
                // }

                // Skip boundary edges entirely (watertight preservation)
                if (candidate.isBoundaryEdge)
                {
                    continue;
                }

                // Skip if either vertex was already touched this pass
                if (vertexTouched[candidate.vertexA] || vertexTouched[candidate.vertexB])
                {
                    continue;
                }

                // Attempt collapse
                if (performCollapse(
                        candidate, positions, normals, indices, vertexToTriangles, vertexRemoved))
                {
                    vertexTouched[candidate.vertexA] = true;
                    vertexTouched[candidate.vertexB] = true;
                    ++passCollapsed;
                    ++totalCollapsed;
                }
            }

            std::cout << "Pass " << (pass + 1) << ": collapsed " << passCollapsed << " edges"
                      << std::endl;

            if (passCollapsed == 0)
            {
                std::cout << "No edges collapsed in this pass, stopping" << std::endl;
                break;
            }

            // Compact mesh
            compactMesh(positions, normals, indices, vertexRemoved);

            // Rebuild quadrics for next pass
            buildVertexQuadrics(positions, indices);

            // Progress callback
            if (m_progressCallback)
            {
                std::size_t const targetTris =
                    m_config.targetTriangleCount.value_or(initialTriangles / 2);
                m_progressCallback(indices.size() / 3, targetTris, totalCollapsed);
            }
        }

        std::size_t const finalTriangles = indices.size() / 3;
        float const reductionPercent =
            100.0F * static_cast<float>(initialTriangles - finalTriangles) /
            static_cast<float>(initialTriangles);

        std::cout << "QEM Simplification complete:" << std::endl;
        std::cout << "  Triangles: " << initialTriangles << " -> " << finalTriangles << " ("
                  << std::fixed << std::setprecision(1) << reductionPercent << "% reduction)"
                  << std::endl;
        std::cout << "  Vertices: " << positions.size() << std::endl;
        std::cout << "  Total edges collapsed: " << totalCollapsed << std::endl;

        return totalCollapsed;
    }

    // ============================================================================
    // Fast Greedy QEM Simplification (single-pass, CPU-only)
    // ============================================================================

    namespace
    {
        /// Per-triangle metadata
        struct TriInfo
        {
            Eigen::Vector3f normal{0.F, 0.F, 0.F};
            std::uint8_t minEdgeIdx{0U};
            bool deleted{false};
        };

        /// Priority queue element — one per non-deleted triangle
        struct TriError
        {
            float value{std::numeric_limits<float>::max()};
            std::uint32_t triIdx{};
            std::size_t heapIdx{std::numeric_limits<std::size_t>::max()};
        };

        Eigen::Vector3f calcNormal(Eigen::Vector3f const & v0,
                                   Eigen::Vector3f const & v1,
                                   Eigen::Vector3f const & v2)
        {
            Eigen::Vector3f n = (v1 - v0).cross(v2 - v0);
            float const len = n.norm();
            if (len < 1e-20F)
            {
                return {0.F, 0.F, 0.F};
            }
            return n / len;
        }

        /// Compute error for collapsing edge (vi, vj), and find optimal position
        float edgeError(SymMat const & qi,
                        SymMat const & qj,
                        Eigen::Vector3f const & pi,
                        Eigen::Vector3f const & pj,
                        Eigen::Vector3f & outPos)
        {
            SymMat const combined = qi + qj;

            auto opt = combined.optimalVertex();
            if (opt.has_value())
            {
                outPos = *opt;
                return static_cast<float>(combined.evaluate(outPos));
            }

            // Fallback: best of both endpoints and midpoint
            float const errI = static_cast<float>(combined.evaluate(pi));
            float const errJ = static_cast<float>(combined.evaluate(pj));
            Eigen::Vector3f const mid = (pi + pj) * 0.5F;
            float const errM = static_cast<float>(combined.evaluate(mid));

            if (errI <= errJ && errI <= errM)
            {
                outPos = pi;
                return errI;
            }
            if (errJ <= errI && errJ <= errM)
            {
                outPos = pj;
                return errJ;
            }
            outPos = mid;
            return errM;
        }

        /// Compute the minimum-error edge for a triangle and store minEdgeIdx
        float evalTriMinEdge(std::uint32_t t,
                             std::vector<std::uint32_t> const & indices,
                             std::vector<Eigen::Vector3f> const & positions,
                             std::vector<SymMat> const & quadrics,
                             std::vector<TriInfo> & tris)
        {
            auto const i0 = indices[t * 3U + 0U];
            auto const i1 = indices[t * 3U + 1U];
            auto const i2 = indices[t * 3U + 2U];

            Eigen::Vector3f dummy;
            float const err0 = edgeError(quadrics[i0], quadrics[i1],
                                         positions[i0], positions[i1], dummy);
            float const err1 = edgeError(quadrics[i1], quadrics[i2],
                                         positions[i1], positions[i2], dummy);
            float const err2 = edgeError(quadrics[i2], quadrics[i0],
                                         positions[i2], positions[i0], dummy);

            float minErr = err0;
            std::uint8_t minIdx = 0U;
            if (err1 < minErr) { minErr = err1; minIdx = 1U; }
            if (err2 < minErr) { minErr = err2; minIdx = 2U; }
            tris[t].minEdgeIdx = minIdx;
            return minErr;
        }

        /// Get two vertex indices for edge within a triangle
        std::pair<std::uint32_t, std::uint32_t> triEdgeVerts(
            std::uint32_t t, std::uint8_t e, std::vector<std::uint32_t> const & indices)
        {
            static constexpr std::uint8_t E[3][2] = {{0, 1}, {1, 2}, {2, 0}};
            return {indices[t * 3U + E[e][0]], indices[t * 3U + E[e][1]]};
        }

        /// Check if moving id0 to newPos would flip any surrounding triangle
        bool wouldFlip(std::uint32_t id0,
                       std::uint32_t id1,
                       Eigen::Vector3f const & newPos,
                       std::vector<Eigen::Vector3f> const & positions,
                       std::vector<std::uint32_t> const & indices,
                       std::vector<std::vector<std::uint32_t>> const & vtx2tri,
                       std::vector<TriInfo> const & tris,
                       float flipThreshold)
        {
            for (auto const t : vtx2tri[id0])
            {
                if (tris[t].deleted)
                {
                    continue;
                }
                auto const ti0 = indices[t * 3U + 0U];
                auto const ti1 = indices[t * 3U + 1U];
                auto const ti2 = indices[t * 3U + 2U];

                // Skip triangles shared by both — they will be deleted
                if (ti0 == id1 || ti1 == id1 || ti2 == id1)
                {
                    continue;
                }

                // Substitute id0 → newPos
                Eigen::Vector3f p[3] = {positions[ti0], positions[ti1], positions[ti2]};
                if (ti0 == id0) p[0] = newPos;
                else if (ti1 == id0) p[1] = newPos;
                else if (ti2 == id0) p[2] = newPos;

                Eigen::Vector3f const newN = calcNormal(p[0], p[1], p[2]);
                if (newN.squaredNorm() < 1e-10F)
                {
                    return true; // degenerate
                }
                if (newN.dot(tris[t].normal) < flipThreshold)
                {
                    return true;
                }
            }
            return false;
        }

        /// Count triangles sharing edge (id0, id1)
        std::uint32_t sharedTriCount(std::uint32_t id0,
                                     std::uint32_t id1,
                                     std::vector<std::uint32_t> const & indices,
                                     std::vector<std::vector<std::uint32_t>> const & vtx2tri,
                                     std::vector<TriInfo> const & tris)
        {
            std::uint32_t count = 0U;
            for (auto const t : vtx2tri[id0])
            {
                if (tris[t].deleted) continue;
                auto const ti0 = indices[t * 3U + 0U];
                auto const ti1 = indices[t * 3U + 1U];
                auto const ti2 = indices[t * 3U + 2U];
                if (ti0 == id1 || ti1 == id1 || ti2 == id1)
                {
                    ++count;
                }
            }
            return count;
        }

        /// Check vertex link condition: the 1-ring neighborhoods of id0 and id1
        /// must share exactly 2 vertices (the two "wing" vertices of the edge's
        /// adjacent triangles). Violating this condition creates non-manifold edges.
        bool linkConditionSatisfied(std::uint32_t id0,
                                    std::uint32_t id1,
                                    std::vector<std::uint32_t> const & indices,
                                    std::vector<std::vector<std::uint32_t>> const & vtx2tri,
                                    std::vector<TriInfo> const & tris)
        {
            // Collect unique 1-ring vertices of id0 (excluding id0, id1)
            std::vector<std::uint32_t> ring0;
            ring0.reserve(12U);
            for (auto const t : vtx2tri[id0])
            {
                if (tris[t].deleted) continue;
                for (std::uint8_t e = 0U; e < 3U; ++e)
                {
                    auto const v = indices[t * 3U + e];
                    if (v != id0 && v != id1)
                    {
                        ring0.push_back(v);
                    }
                }
            }
            std::sort(ring0.begin(), ring0.end());
            ring0.erase(std::unique(ring0.begin(), ring0.end()), ring0.end());

            // Collect unique 1-ring vertices of id1, count intersection with ring0
            std::vector<std::uint32_t> ring1;
            ring1.reserve(12U);
            for (auto const t : vtx2tri[id1])
            {
                if (tris[t].deleted) continue;
                for (std::uint8_t e = 0U; e < 3U; ++e)
                {
                    auto const v = indices[t * 3U + e];
                    if (v != id0 && v != id1)
                    {
                        ring1.push_back(v);
                    }
                }
            }
            std::sort(ring1.begin(), ring1.end());
            ring1.erase(std::unique(ring1.begin(), ring1.end()), ring1.end());

            // Sorted set intersection count
            std::uint32_t commonCount = 0U;
            std::size_t i = 0U;
            std::size_t j = 0U;
            while (i < ring0.size() && j < ring1.size())
            {
                if (ring0[i] < ring1[j])
                {
                    ++i;
                }
                else if (ring0[i] > ring1[j])
                {
                    ++j;
                }
                else
                {
                    ++commonCount;
                    ++i;
                    ++j;
                }
            }
            return commonCount == 2U;
        }

        /// Check if collapsing edge (id0, id1) would create duplicate overlapping
        /// triangles. This happens when a non-shared triangle around id1 (after
        /// rewriting id1→id0) would have the same vertex set as an existing
        /// non-shared triangle around id0. The classic case is the "double-diagonal
        /// diamond": T_a=(id0,W1,W2) and T_b=(id1,W1,W2) both exist alongside
        /// shared triangles (id0,id1,W1) and (id0,id1,W2). After collapse, T_b
        /// becomes (id0,W1,W2), overlapping T_a.
        bool wouldCreateDuplicateFaces(std::uint32_t id0,
                                       std::uint32_t id1,
                                       std::vector<std::uint32_t> const & indices,
                                       std::vector<std::vector<std::uint32_t>> const & vtx2tri,
                                       std::vector<TriInfo> const & tris)
        {
            // Collect non-id0 vertex pairs from non-shared triangles around id0
            std::vector<std::pair<std::uint32_t, std::uint32_t>> id0Pairs;
            id0Pairs.reserve(8U);
            for (auto const t : vtx2tri[id0])
            {
                if (tris[t].deleted) continue;
                auto const ti0 = indices[t * 3U + 0U];
                auto const ti1 = indices[t * 3U + 1U];
                auto const ti2 = indices[t * 3U + 2U];
                // Skip shared triangles (contain both id0 and id1)
                if (ti0 == id1 || ti1 == id1 || ti2 == id1) continue;

                std::uint32_t a = UINT32_MAX;
                std::uint32_t b = UINT32_MAX;
                for (std::uint8_t e = 0U; e < 3U; ++e)
                {
                    auto const v = indices[t * 3U + e];
                    if (v != id0)
                    {
                        if (a == UINT32_MAX) a = v;
                        else b = v;
                    }
                }
                if (a > b) std::swap(a, b);
                id0Pairs.emplace_back(a, b);
            }

            // Check non-shared triangles around id1 for matching pairs
            for (auto const t : vtx2tri[id1])
            {
                if (tris[t].deleted) continue;
                auto const ti0 = indices[t * 3U + 0U];
                auto const ti1 = indices[t * 3U + 1U];
                auto const ti2 = indices[t * 3U + 2U];
                // Skip shared triangles
                if (ti0 == id0 || ti1 == id0 || ti2 == id0) continue;

                std::uint32_t a = UINT32_MAX;
                std::uint32_t b = UINT32_MAX;
                for (std::uint8_t e = 0U; e < 3U; ++e)
                {
                    auto const v = indices[t * 3U + e];
                    if (v != id1)
                    {
                        if (a == UINT32_MAX) a = v;
                        else b = v;
                    }
                }
                if (a > b) std::swap(a, b);

                for (auto const & p : id0Pairs)
                {
                    if (p.first == a && p.second == b)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        /// Compact the mesh: remove deleted triangles and unreferenced vertices
        void compactMeshFast(std::vector<Eigen::Vector3f> & positions,
                             std::vector<std::uint32_t> & indices,
                             std::vector<TriInfo> const & tris)
        {
            std::size_t const triCount = indices.size() / 3U;

            std::vector<std::uint32_t> newIndices;
            newIndices.reserve(indices.size());
            for (std::size_t t = 0U; t < triCount; ++t)
            {
                if (tris[t].deleted) continue;
                auto const i0 = indices[t * 3U + 0U];
                auto const i1 = indices[t * 3U + 1U];
                auto const i2 = indices[t * 3U + 2U];
                if (i0 == i1 || i1 == i2 || i2 == i0) continue;
                newIndices.push_back(i0);
                newIndices.push_back(i1);
                newIndices.push_back(i2);
            }

            std::vector<std::uint32_t> remap(positions.size(), UINT32_MAX);
            std::uint32_t newVertCount = 0U;
            for (auto idx : newIndices)
            {
                if (remap[idx] == UINT32_MAX)
                {
                    remap[idx] = newVertCount++;
                }
            }

            std::vector<Eigen::Vector3f> newPositions(newVertCount);
            for (std::size_t i = 0U; i < positions.size(); ++i)
            {
                if (remap[i] != UINT32_MAX)
                {
                    newPositions[remap[i]] = positions[i];
                }
            }

            for (auto & idx : newIndices)
            {
                idx = remap[idx];
            }

            positions = std::move(newPositions);
            indices = std::move(newIndices);
        }
    } // anonymous namespace

    std::size_t fastQemSimplify(
        std::vector<Eigen::Vector3f> & positions,
        std::vector<std::uint32_t> & indices,
        FastQemConfig const & config,
        std::function<void()> throwOnCancel,
        std::function<void(int)> progressFn)
    {
        if (positions.empty() || indices.size() < 3U)
        {
            return 0U;
        }

        std::size_t const initialTriCount = indices.size() / 3U;

        // Determine target triangle count
        std::size_t targetTriCount = 0U;
        switch (config.terminationMode)
        {
        case SimplificationTerminationMode::TargetTriangleCount:
            targetTriCount = config.targetTriangleCount;
            break;
        case SimplificationTerminationMode::TargetReductionPercent:
            targetTriCount = static_cast<std::size_t>(
                static_cast<float>(initialTriCount) * (1.0F - config.targetReductionPercent / 100.0F));
            break;
        case SimplificationTerminationMode::ErrorBounded:
            targetTriCount = 0U;
            break;
        }
        if (targetTriCount >= initialTriCount)
        {
            return 0U;
        }

        std::size_t const collapsesNeeded = initialTriCount - targetTriCount;

        // Weld duplicate vertices: merge vertices at the same position into a
        // single canonical index.  This fixes cracks from dual contouring where
        // different octree cells emit separate vertices at the same location.
        {
            struct Float3Hash
            {
                std::size_t operator()(Eigen::Vector3f const & v) const
                {
                    std::uint32_t hx, hy, hz;
                    std::memcpy(&hx, &v.x(), sizeof(float));
                    std::memcpy(&hy, &v.y(), sizeof(float));
                    std::memcpy(&hz, &v.z(), sizeof(float));
                    // splitmix64-style mixing
                    auto mix = [](std::size_t x)
                    {
                        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
                        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
                        return x ^ (x >> 31);
                    };
                    return mix(hx) ^ (mix(hy) * 2654435761ULL) ^ (mix(hz) * 40503ULL);
                }
            };
            struct Float3Eq
            {
                bool operator()(Eigen::Vector3f const & a, Eigen::Vector3f const & b) const
                {
                    return std::memcmp(&a, &b, sizeof(float) * 3) == 0;
                }
            };

            std::unordered_map<Eigen::Vector3f, std::uint32_t, Float3Hash, Float3Eq> posToIdx;
            posToIdx.reserve(positions.size());
            std::vector<std::uint32_t> remap(positions.size());
            std::size_t mergedCount = 0U;

            for (std::uint32_t i = 0U; i < static_cast<std::uint32_t>(positions.size()); ++i)
            {
                auto const [it, inserted] = posToIdx.emplace(positions[i], i);
                remap[i] = it->second;
                if (!inserted && it->second != i) ++mergedCount;
            }

            if (mergedCount > 0U)
            {
                for (auto & idx : indices)
                {
                    idx = remap[idx];
                }
                // Degenerate triangles (with two or more identical indices after
                // welding) are handled downstream: the quadric init marks zero-area
                // triangles as deleted, and compactMeshFast skips them.
            }
        }

        // Build vertex-to-triangle adjacency (vector of vectors for dynamic updates)
        std::vector<std::vector<std::uint32_t>> vtx2tri(positions.size());
        {
            std::size_t const triCount = indices.size() / 3U;
            for (std::uint32_t t = 0U; t < static_cast<std::uint32_t>(triCount); ++t)
            {
                vtx2tri[indices[t * 3U + 0U]].push_back(t);
                vtx2tri[indices[t * 3U + 1U]].push_back(t);
                vtx2tri[indices[t * 3U + 2U]].push_back(t);
            }
        }

        // Initialize per-vertex quadrics
        std::vector<SymMat> quadrics(positions.size());
        std::vector<TriInfo> tris(initialTriCount);

        for (std::uint32_t t = 0U; t < static_cast<std::uint32_t>(initialTriCount); ++t)
        {
            auto const i0 = indices[t * 3U + 0U];
            auto const i1 = indices[t * 3U + 1U];
            auto const i2 = indices[t * 3U + 2U];
            Eigen::Vector3f const n = calcNormal(positions[i0], positions[i1], positions[i2]);
            tris[t].normal = n;
            if (n.squaredNorm() < 1e-10F)
            {
                tris[t].deleted = true;
                continue;
            }
            double const a = n.x(), b = n.y(), c = n.z();
            double const d = -static_cast<double>(n.dot(positions[i0]));
            SymMat const q = SymMat::fromPlane(a, b, c, d);
            quadrics[i0] += q;
            quadrics[i1] += q;
            quadrics[i2] += q;
        }

        // Per-triangle heap index tracking (indexed by triangle index)
        std::vector<std::size_t> triHeapIdx(initialTriCount, std::numeric_limits<std::size_t>::max());

        auto indexSetter = [&triHeapIdx](TriError & te, std::size_t idx)
        {
            te.heapIdx = idx;
            triHeapIdx[te.triIdx] = idx;
        };
        auto lessPred = [](TriError const & a, TriError const & b) { return a.value < b.value; };

        MutablePriorityQueue<TriError, decltype(indexSetter), decltype(lessPred)> pq(indexSetter, lessPred);
        pq.reserve(initialTriCount);

        for (std::uint32_t t = 0U; t < static_cast<std::uint32_t>(initialTriCount); ++t)
        {
            if (tris[t].deleted) continue;
            TriError te;
            te.triIdx = t;
            te.value = evalTriMinEdge(t, indices, positions, quadrics, tris);
            pq.push(te);
        }

        // Greedy collapse loop
        std::size_t totalCollapsed = 0U;
        std::size_t currentTriCount = initialTriCount;
        int lastProgress = -1;

        // Starvation detection: after a successful collapse, re-evaluation may push
        // entries back into the PQ.  If we pop all entries without any successful
        // collapse, the remaining edges are topologically non-collapsible and we
        // should stop.
        std::size_t popsSinceLastCollapse = 0U;
        std::size_t pqSizeAfterLastCollapse = pq.size();

        while (!pq.empty() && currentTriCount > targetTriCount)
        {
            if (throwOnCancel && (totalCollapsed % config.cancelCheckPeriod == 0U))
            {
                throwOnCancel();
            }
            if (progressFn && collapsesNeeded > 0U)
            {
                int const progress = std::min(
                    static_cast<int>((totalCollapsed * 100U) / collapsesNeeded), 100);
                if (progress != lastProgress)
                {
                    progressFn(progress);
                    lastProgress = progress;
                }
            }

            auto const best = pq.top();
            pq.pop();

            // If we have popped more entries than existed after the last
            // successful collapse, all current candidates have been checked
            // and none could be collapsed.  Break to avoid spinning.
            if (++popsSinceLastCollapse > pqSizeAfterLastCollapse)
            {
                break;
            }

            auto const triIdx = best.triIdx;
            if (tris[triIdx].deleted) continue;

            if (best.value > config.maxError) break;

            // Try all three edges of the triangle, starting with the minimum-error
            // edge, then falling back to the other two.  This drastically reduces
            // the number of PQ pops needed because wouldFlip rejects many edges
            // that are optimal by error but geometrically problematic; alternative
            // edges in the same triangle often succeed.
            bool collapsed = false;

            // Sort edges by error for this triangle
            auto const ti0 = indices[triIdx * 3U + 0U];
            auto const ti1 = indices[triIdx * 3U + 1U];
            auto const ti2 = indices[triIdx * 3U + 2U];
            Eigen::Vector3f dummy;
            float const err0 = edgeError(quadrics[ti0], quadrics[ti1],
                                         positions[ti0], positions[ti1], dummy);
            float const err1 = edgeError(quadrics[ti1], quadrics[ti2],
                                         positions[ti1], positions[ti2], dummy);
            float const err2 = edgeError(quadrics[ti2], quadrics[ti0],
                                         positions[ti2], positions[ti0], dummy);

            std::array<std::uint8_t, 3> edgeOrder = {0U, 1U, 2U};
            std::array<float, 3> const edgeErr = {err0, err1, err2};
            std::sort(edgeOrder.begin(), edgeOrder.end(),
                      [&edgeErr](std::uint8_t a, std::uint8_t b)
                      { return edgeErr[a] < edgeErr[b]; });

            for (auto const edgeIdx : edgeOrder)
            {
                if (edgeErr[edgeIdx] > config.maxError) break;

                auto [id0, id1] = triEdgeVerts(triIdx, edgeIdx, indices);

                // Topology guard: only collapse manifold interior edges with valid vertex link
                if (sharedTriCount(id0, id1, indices, vtx2tri, tris) != 2U) continue;
                if (!linkConditionSatisfied(id0, id1, indices, vtx2tri, tris)) continue;
                if (wouldCreateDuplicateFaces(id0, id1, indices, vtx2tri, tris)) continue;

                // Valence guard: the non-surviving vertex (id1) must have at least 3
                // non-deleted triangles. With sharedTriCount == 2, this guarantees at least
                // 1 non-shared triangle exists to fill the gap left by the deleted shared
                // triangles. Without this, collapsing a "tip" vertex whose entire fan
                // consists of only the 2 shared triangles would create boundary edges (a hole).
                // Additionally, the post-collapse valence of the surviving vertex (id0)
                // must be at least 3 to form a valid closed fan.
                {
                    std::uint32_t id0LiveCount = 0U;
                    std::uint32_t id1LiveCount = 0U;
                    for (auto const t : vtx2tri[id0])
                    {
                        if (!tris[t].deleted) ++id0LiveCount;
                    }
                    for (auto const t : vtx2tri[id1])
                    {
                        if (!tris[t].deleted) ++id1LiveCount;
                    }
                    if (id1LiveCount <= 2U) continue;
                    if (id0LiveCount + id1LiveCount < 7U) continue;
                }

                // Wing vertex connectivity guard: for each wing vertex (the third vertex
                // of each shared triangle), verify that the post-collapse edge count at
                // (id0, wing) will be >= 2. The count is: (triangles of id0 with wing - 1
                // for the deleted shared triangle) + (non-shared triangles of id1 with wing).
                // If id0's side has lost triangles from prior collapses, even having a
                // non-shared triangle from id1 may not suffice.
                {
                    bool wingOk = true;
                    for (auto const st : vtx2tri[id1])
                    {
                        if (tris[st].deleted) continue;
                        auto const sv0 = indices[st * 3U + 0U];
                        auto const sv1 = indices[st * 3U + 1U];
                        auto const sv2 = indices[st * 3U + 2U];
                        if (!(sv0 == id0 || sv1 == id0 || sv2 == id0)) continue;
                        // Shared triangle: find the wing vertex
                        std::uint32_t w = UINT32_MAX;
                        for (auto const v : {sv0, sv1, sv2})
                        {
                            if (v != id0 && v != id1) { w = v; break; }
                        }
                        if (w == UINT32_MAX) continue;
                        // Count triangles of id0 containing the wing vertex
                        std::uint32_t id0wCount = 0U;
                        for (auto const t : vtx2tri[id0])
                        {
                            if (tris[t].deleted) continue;
                            auto const x0 = indices[t * 3U + 0U];
                            auto const x1 = indices[t * 3U + 1U];
                            auto const x2 = indices[t * 3U + 2U];
                            if (x0 == w || x1 == w || x2 == w) ++id0wCount;
                        }
                        // Count non-shared triangles of id1 containing the wing vertex
                        std::uint32_t id1wCount = 0U;
                        for (auto const t : vtx2tri[id1])
                        {
                            if (tris[t].deleted) continue;
                            auto const x0 = indices[t * 3U + 0U];
                            auto const x1 = indices[t * 3U + 1U];
                            auto const x2 = indices[t * 3U + 2U];
                            if (x0 == id0 || x1 == id0 || x2 == id0) continue;
                            if (x0 == w || x1 == w || x2 == w) ++id1wCount;
                        }
                        // Post-collapse count for edge (id0, w)
                        if (id0wCount + id1wCount < 3U)
                        {
                            wingOk = false;
                            break;
                        }
                    }
                    if (!wingOk) continue;
                }

                // Compute optimal position
                Eigen::Vector3f newPos;
                edgeError(quadrics[id0], quadrics[id1], positions[id0], positions[id1], newPos);

                // Flip detection
                if (wouldFlip(id0, id1, newPos, positions, indices, vtx2tri, tris, config.flipThreshold))
                {
                    continue;
                }
                if (wouldFlip(id1, id0, newPos, positions, indices, vtx2tri, tris, config.flipThreshold))
                {
                    continue;
                }

                // === Perform collapse: merge id1 into id0 ===
                collapsed = true;
                positions[id0] = newPos;
                quadrics[id0] += quadrics[id1];

                // Mark shared triangles as deleted
                for (auto const t : vtx2tri[id1])
                {
                    if (tris[t].deleted) continue;
                    auto const tti0 = indices[t * 3U + 0U];
                    auto const tti1 = indices[t * 3U + 1U];
                    auto const tti2 = indices[t * 3U + 2U];
                    if (tti0 == id0 || tti1 == id0 || tti2 == id0)
                    {
                        tris[t].deleted = true;
                        --currentTriCount;
                        // Remove from pq (verify the entry still belongs to this triangle)
                        if (triHeapIdx[t] < pq.size() &&
                            pq[triHeapIdx[t]].triIdx == static_cast<std::uint32_t>(t))
                        {
                            pq.remove(triHeapIdx[t]);
                        }
                    }
                }

                // Rewrite id1 → id0 in remaining triangles around id1
                for (auto const t : vtx2tri[id1])
                {
                    if (tris[t].deleted) continue;
                    for (std::uint8_t e2 = 0U; e2 < 3U; ++e2)
                    {
                        if (indices[t * 3U + e2] == id1)
                        {
                            indices[t * 3U + e2] = id0;
                        }
                    }
                    // Add triangle to id0's adjacency
                    vtx2tri[id0].push_back(t);
                }
                vtx2tri[id1].clear();

                // Prune deleted entries from vtx2tri[id0]
                {
                    auto & adj = vtx2tri[id0];
                    adj.erase(std::remove_if(adj.begin(),
                                             adj.end(),
                                             [&tris](std::uint32_t t)
                                             { return tris[t].deleted; }),
                              adj.end());
                }

                // Update normals for triangles around id0
                for (auto const t : vtx2tri[id0])
                {
                    if (tris[t].deleted) continue;
                    auto const ni0 = indices[t * 3U + 0U];
                    auto const ni1 = indices[t * 3U + 1U];
                    auto const ni2 = indices[t * 3U + 2U];
                    tris[t].normal = calcNormal(positions[ni0], positions[ni1], positions[ni2]);
                }

                // Re-evaluate affected triangles in the priority queue
                for (auto const t : vtx2tri[id0])
                {
                    if (tris[t].deleted) continue;
                    float const newErr = evalTriMinEdge(t, indices, positions, quadrics, tris);
                    if (triHeapIdx[t] < pq.size() &&
                        pq[triHeapIdx[t]].triIdx == static_cast<std::uint32_t>(t))
                    {
                        pq[triHeapIdx[t]].value = newErr;
                        pq.update(triHeapIdx[t]);
                    }
                    else
                    {
                        TriError te;
                        te.triIdx = static_cast<std::uint32_t>(t);
                        te.value = newErr;
                        pq.push(te);
                    }
                }

                break; // Edge collapsed successfully, exit inner edge loop
            } // end for (edgeIdx)

            if (!collapsed) continue;

            ++totalCollapsed;

            // Reset starvation counter and record new PQ size
            // (re-evaluation may have pushed entries back).
            popsSinceLastCollapse = 0U;
            pqSizeAfterLastCollapse = pq.size();
        }

        if (progressFn)
        {
            progressFn(100);
        }

        compactMeshFast(positions, indices, tris);

        return totalCollapsed;
    }

} // namespace gladius::compute
