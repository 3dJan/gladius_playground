#include "MeshSimplification.h"

#include <algorithm>
#include <cmath>
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

            // Boundary edge: only 1 adjacent triangle
            candidate.isBoundaryEdge = (triangles.size() == 1);

            // Sharp feature detection: check angle between adjacent triangle normals
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
            }

            // Compute combined quadric for edge
            Quadric const combinedQuadric =
                m_vertexQuadrics[candidate.vertexA] + m_vertexQuadrics[candidate.vertexB];

            // Find optimal target position
            auto optimalPos = combinedQuadric.optimalVertex();
            if (optimalPos.has_value())
            {
                candidate.targetPosition = optimalPos.value();
            }
            else
            {
                // Fall back to edge midpoint
                candidate.targetPosition =
                    (positions[candidate.vertexA] + positions[candidate.vertexB]) * 0.5F;
            }

            // Compute QEM error at target position
            candidate.qemError = combinedQuadric.evaluate(candidate.targetPosition);

            // SDF error will be filled in by GPU evaluation
            candidate.sdfError = 0.0F;

            candidates.push_back(candidate);
        }

        return candidates;
    }

    void QemMeshSimplifier::evaluateSdfErrorsGpu(std::vector<CollapseCandidate> & candidates)
    {
        if (!m_gpuSdfEvaluator || candidates.empty())
        {
            return;
        }

        // Process in batches
        for (std::size_t batchStart = 0; batchStart < candidates.size();
             batchStart += m_config.batchSize)
        {
            std::size_t const batchEnd =
                std::min(batchStart + m_config.batchSize, candidates.size());
            std::size_t const batchSize = batchEnd - batchStart;

            // Collect positions for this batch
            std::vector<Eigen::Vector3f> positions;
            positions.reserve(batchSize);
            for (std::size_t i = batchStart; i < batchEnd; ++i)
            {
                positions.push_back(candidates[i].targetPosition);
            }

            // Evaluate SDF on GPU
            std::vector<float> sdfValues = m_gpuSdfEvaluator(positions);

            // Store results
            for (std::size_t i = 0; i < batchSize; ++i)
            {
                candidates[batchStart + i].sdfError = std::abs(sdfValues[i]);
            }
        }

        // Compute combined error for all candidates
        for (auto & candidate : candidates)
        {
            float error = candidate.sdfError * m_config.sdfErrorWeight +
                          candidate.qemError * m_config.qemErrorWeight;

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

            // Check for degenerate triangle
            Eigen::Vector3f const e1 = p1 - p0;
            Eigen::Vector3f const e2 = p2 - p0;
            Eigen::Vector3f const newNormal = e1.cross(e2);
            float const area = newNormal.norm();

            if (area < 1e-10F)
            {
                return true; // Degenerate
            }

            // Check for inverted triangle
            if (hasA || hasB)
            {
                Eigen::Vector3f const oldP0 = positions[i0];
                Eigen::Vector3f const oldP1 = positions[i1];
                Eigen::Vector3f const oldP2 = positions[i2];
                Eigen::Vector3f const oldNormal = (oldP1 - oldP0).cross(oldP2 - oldP0);

                if (oldNormal.dot(newNormal) < 0.0F)
                {
                    return true; // Inverted
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

            // Evaluate SDF errors on GPU
            evaluateSdfErrorsGpu(candidates);

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

} // namespace gladius::compute
