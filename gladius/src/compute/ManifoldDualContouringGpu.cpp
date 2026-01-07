#include "ManifoldDualContouringGpu.h"
#include "../Buffer.h"
#include "../Primitives.h"
#include "../ResourceContext.h"
#include "../SlicerProgram.h"
#include "ManifoldDualContouringProgram.h"
#include "MeshQualityMetrics.h"
#include "MeshSimplification.h"

#include <algorithm>
#include <cmath>
#include <Eigen/Eigenvalues>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <array>
#include <cstdlib>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gladius::compute
{
    namespace
    {
        struct GpuVertex
        {
            cl_float4 position;
            cl_float4 normal;
        };
    }
        namespace
        {
            struct TriangleKey
            {
                std::uint32_t a{0U};
                std::uint32_t b{0U};
                std::uint32_t c{0U};
                std::size_t triIndex{0U};
            };

            [[nodiscard]] bool triangleKeyLess(TriangleKey const & lhs, TriangleKey const & rhs)
            {
                if (lhs.a != rhs.a)
                {
                    return lhs.a < rhs.a;
                }
                if (lhs.b != rhs.b)
                {
                    return lhs.b < rhs.b;
                }
                return lhs.c < rhs.c;
            }

            [[nodiscard]] bool triangleKeyEqual(TriangleKey const & lhs, TriangleKey const & rhs)
            {
                return lhs.a == rhs.a && lhs.b == rhs.b && lhs.c == rhs.c;
            }

            void removeDegenerateAndDuplicateTriangles(ManifoldDualContouringMesh & mesh)
            {
                if (mesh.indices.size() < 3U || (mesh.indices.size() % 3U) != 0U)
                {
                    return;
                }

                auto const isFiniteVec3 = [](Eigen::Vector3f const & v)
                {
                    return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
                };

                std::size_t const originalTriangleCount = mesh.indices.size() / 3U;
                std::vector<TriangleKey> keys;
                keys.reserve(originalTriangleCount);

                std::size_t degenerateCount = 0U;
                for (std::size_t tri = 0U; tri < originalTriangleCount; ++tri)
                {
                    std::uint32_t const i0 = mesh.indices[tri * 3U + 0U];
                    std::uint32_t const i1 = mesh.indices[tri * 3U + 1U];
                    std::uint32_t const i2 = mesh.indices[tri * 3U + 2U];

                    if (i0 == i1 || i1 == i2 || i2 == i0)
                    {
                        ++degenerateCount;
                        continue;
                    }

                    if (i0 >= mesh.positions.size() || i1 >= mesh.positions.size() ||
                        i2 >= mesh.positions.size())
                    {
                        ++degenerateCount;
                        continue;
                    }

                    Eigen::Vector3f const & p0 = mesh.positions[i0];
                    Eigen::Vector3f const & p1 = mesh.positions[i1];
                    Eigen::Vector3f const & p2 = mesh.positions[i2];
                    if (!isFiniteVec3(p0) || !isFiniteVec3(p1) || !isFiniteVec3(p2))
                    {
                        ++degenerateCount;
                        continue;
                    }

                    // Also cull geometrically degenerate triangles (zero area). Some consumers
                    // discard these at import time, which can make an otherwise "closed" mesh
                    // appear to have holes.
                    Eigen::Vector3f const cross = (p1 - p0).cross(p2 - p0);
                    if (cross.squaredNorm() <= 1e-12F)
                    {
                        ++degenerateCount;
                        continue;
                    }

                    // Canonicalize triangle key independent of winding.
                    std::uint32_t a = i0;
                    std::uint32_t b = i1;
                    std::uint32_t c = i2;
                    if (a > b)
                    {
                        std::swap(a, b);
                    }
                    if (b > c)
                    {
                        std::swap(b, c);
                    }
                    if (a > b)
                    {
                        std::swap(a, b);
                    }

                    keys.push_back({a, b, c, tri});
                }

                if (keys.empty())
                {
                    if (degenerateCount > 0U)
                    {
                        mesh.indices.clear();
                    }
                    return;
                }

                std::sort(keys.begin(), keys.end(), triangleKeyLess);

                std::vector<std::uint32_t> dedupedIndices;
                dedupedIndices.reserve(keys.size() * 3U);

                TriangleKey prevKey = keys.front();
                {
                    std::size_t const tri = prevKey.triIndex;
                    dedupedIndices.push_back(mesh.indices[tri * 3U + 0U]);
                    dedupedIndices.push_back(mesh.indices[tri * 3U + 1U]);
                    dedupedIndices.push_back(mesh.indices[tri * 3U + 2U]);
                }

                std::size_t duplicateCount = 0U;
                for (std::size_t i = 1U; i < keys.size(); ++i)
                {
                    TriangleKey const & key = keys[i];
                    if (triangleKeyEqual(key, prevKey))
                    {
                        ++duplicateCount;
                        continue;
                    }

                    std::size_t const tri = key.triIndex;
                    dedupedIndices.push_back(mesh.indices[tri * 3U + 0U]);
                    dedupedIndices.push_back(mesh.indices[tri * 3U + 1U]);
                    dedupedIndices.push_back(mesh.indices[tri * 3U + 2U]);
                    prevKey = key;
                }

                std::size_t const newTriangleCount = dedupedIndices.size() / 3U;
                if (degenerateCount > 0U || duplicateCount > 0U)
                {
                    std::cout << "  Triangle cleanup: removed " << degenerateCount
                              << " degenerate and " << duplicateCount
                              << " duplicate triangles (" << originalTriangleCount
                              << " -> " << newTriangleCount << ")" << std::endl;
                }

                mesh.indices = std::move(dedupedIndices);
            }

            [[nodiscard]] bool shouldRepairNonManifoldEdges()
            {
                // Keep this opt-in / strict-mode only: building an edge incidence map over
                // millions of triangles is expensive. Tests enable strict enforcement via
                // GLADIUS_REQUIRE_WATERTIGHT=1.
                return (std::getenv("GLADIUS_REQUIRE_WATERTIGHT") != nullptr) ||
                       (std::getenv("GLADIUS_MDC_REPAIR_NONMANIFOLD_EDGES") != nullptr);
            }

            struct EdgeIncidence
            {
                std::uint32_t count{0U};
                std::array<std::uint32_t, 4> triIds{};
            };

            [[nodiscard]] std::uint64_t makeEdgeKey(std::uint32_t a, std::uint32_t b)
            {
                std::uint32_t const lo = std::min(a, b);
                std::uint32_t const hi = std::max(a, b);
                return (static_cast<std::uint64_t>(lo) << 32U) | static_cast<std::uint64_t>(hi);
            }

            [[nodiscard]] Eigen::Vector3f triangleNormal(ManifoldDualContouringMesh const & mesh,
                                                         std::uint32_t i0,
                                                         std::uint32_t i1,
                                                         std::uint32_t i2)
            {
                Eigen::Vector3f const & p0 = mesh.positions[i0];
                Eigen::Vector3f const & p1 = mesh.positions[i1];
                Eigen::Vector3f const & p2 = mesh.positions[i2];
                Eigen::Vector3f const n = (p1 - p0).cross(p2 - p0);
                float const len = n.norm();
                if (len <= 1e-12F)
                {
                    return Eigen::Vector3f::Zero();
                }
                return n / len;
            }

            void orientTriangleToNormal(ManifoldDualContouringMesh const & mesh,
                                        Eigen::Vector3f const & targetNormal,
                                        std::uint32_t & io0,
                                        std::uint32_t & io1,
                                        std::uint32_t & io2)
            {
                Eigen::Vector3f const n = triangleNormal(mesh, io0, io1, io2);
                if (n.isZero(1e-12F) || targetNormal.isZero(1e-12F))
                {
                    return;
                }
                if (n.dot(targetNormal) < 0.0F)
                {
                    std::swap(io1, io2);
                }
            }

            void repairNonManifoldDegree4Edges(ManifoldDualContouringMesh & mesh)
            {
                if (!shouldRepairNonManifoldEdges())
                {
                    return;
                }

                if (mesh.indices.size() < 3U || (mesh.indices.size() % 3U) != 0U)
                {
                    return;
                }

                std::size_t const vertexCount = mesh.positions.size();
                std::size_t const triangleCount = mesh.indices.size() / 3U;

                // Build edge incidence map (undirected).
                std::unordered_map<std::uint64_t, EdgeIncidence> incidence;
                incidence.reserve(triangleCount * 3U);

                auto addEdge = [&incidence](std::uint32_t a, std::uint32_t b, std::uint32_t triId)
                {
                    std::uint64_t const key = makeEdgeKey(a, b);
                    auto & info = incidence[key];
                    if (info.count < info.triIds.size())
                    {
                        info.triIds[info.count] = triId;
                    }
                    ++info.count;
                };

                for (std::size_t tri = 0U; tri < triangleCount; ++tri)
                {
                    std::uint32_t const i0 = mesh.indices[tri * 3U + 0U];
                    std::uint32_t const i1 = mesh.indices[tri * 3U + 1U];
                    std::uint32_t const i2 = mesh.indices[tri * 3U + 2U];

                    if (static_cast<std::size_t>(i0) >= vertexCount ||
                        static_cast<std::size_t>(i1) >= vertexCount ||
                        static_cast<std::size_t>(i2) >= vertexCount)
                    {
                        continue;
                    }

                    addEdge(i0, i1, static_cast<std::uint32_t>(tri));
                    addEdge(i1, i2, static_cast<std::uint32_t>(tri));
                    addEdge(i2, i0, static_cast<std::uint32_t>(tri));
                }

                std::vector<std::uint64_t> candidates;
                candidates.reserve(4096);
                for (auto const & [key, info] : incidence)
                {
                    if (info.count == 4U)
                    {
                        candidates.push_back(key);
                    }
                }

                if (candidates.empty())
                {
                    return;
                }

                std::vector<std::uint32_t> newIndices = mesh.indices;
                std::vector<bool> triTouched(triangleCount, false);

                auto unpackEdgeKey = [](std::uint64_t key) -> std::pair<std::uint32_t, std::uint32_t>
                {
                    std::uint32_t const a = static_cast<std::uint32_t>(key >> 32U);
                    std::uint32_t const b = static_cast<std::uint32_t>(key & 0xFFFFFFFFULL);
                    return {a, b};
                };

                std::size_t repairedEdges = 0U;
                for (std::uint64_t const key : candidates)
                {
                    auto const it = incidence.find(key);
                    if (it == incidence.end() || it->second.count != 4U)
                    {
                        continue;
                    }

                    auto const [a, b] = unpackEdgeKey(key);
                    std::array<std::uint32_t, 4> triIds = it->second.triIds;

                    // Skip if any triangle already changed in this pass.
                    bool anyTouched = false;
                    for (std::uint32_t const t : triIds)
                    {
                        if (t < triTouched.size() && triTouched[t])
                        {
                            anyTouched = true;
                            break;
                        }
                    }
                    if (anyTouched)
                    {
                        continue;
                    }

                    // Gather normals and "third" vertices for each incident triangle.
                    struct TriInfo
                    {
                        std::uint32_t triId{0U};
                        std::uint32_t third{0U};
                        Eigen::Vector3f normal{Eigen::Vector3f::Zero()};
                    };
                    std::array<TriInfo, 4> tris;

                    bool valid = true;
                    for (std::size_t i = 0U; i < 4U; ++i)
                    {
                        std::uint32_t const triId = triIds[i];
                        if (triId >= triangleCount)
                        {
                            valid = false;
                            break;
                        }

                        std::uint32_t const i0 = newIndices[triId * 3U + 0U];
                        std::uint32_t const i1 = newIndices[triId * 3U + 1U];
                        std::uint32_t const i2 = newIndices[triId * 3U + 2U];

                        // Identify the vertex not on the edge (a,b).
                        std::uint32_t third = 0U;
                        if (i0 != a && i0 != b)
                        {
                            third = i0;
                        }
                        else if (i1 != a && i1 != b)
                        {
                            third = i1;
                        }
                        else if (i2 != a && i2 != b)
                        {
                            third = i2;
                        }
                        else
                        {
                            valid = false;
                            break;
                        }

                        Eigen::Vector3f const n = triangleNormal(mesh, i0, i1, i2);
                        if (n.isZero(1e-12F))
                        {
                            valid = false;
                            break;
                        }

                        tris[i] = TriInfo{triId, third, n};
                    }

                    if (!valid)
                    {
                        continue;
                    }

                    // Choose a pairing (perfect matching) that maximizes within-pair normal similarity.
                    auto dotAbs = [](Eigen::Vector3f const & n0, Eigen::Vector3f const & n1)
                    { return std::abs(n0.dot(n1)); };

                    struct Matching
                    {
                        std::array<std::array<int, 2>, 2> pairs{};
                        float score{0.0F};
                    };

                    std::array<Matching, 3> matchings;
                    matchings[0].pairs[0] = {0, 1};
                    matchings[0].pairs[1] = {2, 3};
                    matchings[1].pairs[0] = {0, 2};
                    matchings[1].pairs[1] = {1, 3};
                    matchings[2].pairs[0] = {0, 3};
                    matchings[2].pairs[1] = {1, 2};

                    for (auto & m : matchings)
                    {
                        m.score = dotAbs(tris[m.pairs[0][0]].normal, tris[m.pairs[0][1]].normal) +
                                  dotAbs(tris[m.pairs[1][0]].normal, tris[m.pairs[1][1]].normal);
                    }

                    auto const bestIt = std::max_element(matchings.begin(),
                                                       matchings.end(),
                                                       [](Matching const & lhs, Matching const & rhs)
                                                       { return lhs.score < rhs.score; });
                    Matching const & best = *bestIt;

                    auto attemptFlipPair = [&](std::array<int, 2> const & p) -> bool
                    {
                        TriInfo const & t0 = tris[p[0]];
                        TriInfo const & t1 = tris[p[1]];
                        std::uint32_t const c = t0.third;
                        std::uint32_t const d = t1.third;
                        if (c == d)
                        {
                            return false;
                        }

                        // Avoid creating a new degree-4 edge on the opposite diagonal.
                        std::uint64_t const cdKey = makeEdgeKey(c, d);
                        auto const cdIt = incidence.find(cdKey);
                        if (cdIt != incidence.end() && cdIt->second.count >= 2U)
                        {
                            return false;
                        }

                        Eigen::Vector3f targetNormal = t0.normal + t1.normal;
                        if (targetNormal.isZero(1e-6F))
                        {
                            targetNormal = t0.normal;
                        }
                        else
                        {
                            targetNormal.normalize();
                        }

                        // Replace both triangles with the flipped diagonal (c,d):
                        // (a,c,d) and (b,d,c)
                        std::uint32_t n0_0 = a;
                        std::uint32_t n0_1 = c;
                        std::uint32_t n0_2 = d;
                        std::uint32_t n1_0 = b;
                        std::uint32_t n1_1 = d;
                        std::uint32_t n1_2 = c;

                        orientTriangleToNormal(mesh, targetNormal, n0_0, n0_1, n0_2);
                        orientTriangleToNormal(mesh, targetNormal, n1_0, n1_1, n1_2);

                        newIndices[t0.triId * 3U + 0U] = n0_0;
                        newIndices[t0.triId * 3U + 1U] = n0_1;
                        newIndices[t0.triId * 3U + 2U] = n0_2;

                        newIndices[t1.triId * 3U + 0U] = n1_0;
                        newIndices[t1.triId * 3U + 1U] = n1_1;
                        newIndices[t1.triId * 3U + 2U] = n1_2;

                        triTouched[t0.triId] = true;
                        triTouched[t1.triId] = true;
                        return true;
                    };

                    // Flip exactly one of the two surface sheets using this edge.
                    // Prefer flipping the less-coplanar pair (smaller abs dot), so the remaining
                    // (a,b) edge corresponds to the more coherent surface patch.
                    auto const pair0 = best.pairs[0];
                    auto const pair1 = best.pairs[1];
                    float const sim0 = dotAbs(tris[pair0[0]].normal, tris[pair0[1]].normal);
                    float const sim1 = dotAbs(tris[pair1[0]].normal, tris[pair1[1]].normal);

                    bool flipped = false;
                    if (sim0 < sim1)
                    {
                        flipped = attemptFlipPair(pair0) || attemptFlipPair(pair1);
                    }
                    else
                    {
                        flipped = attemptFlipPair(pair1) || attemptFlipPair(pair0);
                    }

                    if (flipped)
                    {
                        ++repairedEdges;
                    }
                }

                if (repairedEdges > 0U)
                {
                    std::cout << "  Non-manifold repair: flipped diagonals for " << repairedEdges
                              << " degree-4 edges (" << candidates.size() << " candidates)" << std::endl;
                    mesh.indices = std::move(newIndices);
                }
            }

            struct TopologyStats
            {
                std::size_t triangleCount{0U};
                std::size_t uniqueEdgeCount{0U};
                std::size_t openEdgeCount{0U};
                std::size_t nonManifoldEdgeCount{0U};
                std::size_t invalidTriangleCount{0U};
            };

            [[nodiscard]] bool debugMdcTopologyStagesEnabled()
            {
                return std::getenv("GLADIUS_DEBUG_MDC_TOPOLOGY_STAGES") != nullptr;
            }

            [[nodiscard]] TopologyStats computeTopologyStats(ManifoldDualContouringMesh const & mesh)
            {
                TopologyStats stats;

                if (mesh.indices.size() < 3U || (mesh.indices.size() % 3U) != 0U)
                {
                    return stats;
                }

                std::size_t const vertexCount = mesh.positions.size();
                std::size_t const triangleCount = mesh.indices.size() / 3U;
                stats.triangleCount = triangleCount;

                // Count undirected edge usage.
                // key = (minIndex << 32) | maxIndex
                std::unordered_map<std::uint64_t, std::uint32_t> edgeUseCount;
                edgeUseCount.reserve(triangleCount * 3U);

                for (std::size_t tri = 0U; tri < triangleCount; ++tri)
                {
                    std::uint32_t const i0 = mesh.indices[tri * 3U + 0U];
                    std::uint32_t const i1 = mesh.indices[tri * 3U + 1U];
                    std::uint32_t const i2 = mesh.indices[tri * 3U + 2U];

                    if (static_cast<std::size_t>(i0) >= vertexCount ||
                        static_cast<std::size_t>(i1) >= vertexCount ||
                        static_cast<std::size_t>(i2) >= vertexCount)
                    {
                        ++stats.invalidTriangleCount;
                        continue;
                    }

                    auto addEdge = [&edgeUseCount](std::uint32_t a, std::uint32_t b)
                    {
                        std::uint32_t const lo = std::min(a, b);
                        std::uint32_t const hi = std::max(a, b);
                        std::uint64_t const key =
                          (static_cast<std::uint64_t>(lo) << 32U) | static_cast<std::uint64_t>(hi);
                        auto const it = edgeUseCount.find(key);
                        if (it == edgeUseCount.end())
                        {
                            edgeUseCount.emplace(key, 1U);
                        }
                        else
                        {
                            ++it->second;
                        }
                    };

                    addEdge(i0, i1);
                    addEdge(i1, i2);
                    addEdge(i2, i0);
                }

                stats.uniqueEdgeCount = edgeUseCount.size();
                for (auto const & kv : edgeUseCount)
                {
                    std::uint32_t const count = kv.second;
                    if (count == 1U)
                    {
                        ++stats.openEdgeCount;
                    }
                    else if (count > 2U)
                    {
                        ++stats.nonManifoldEdgeCount;
                    }
                }

                return stats;
            }

            void dumpTopologyStats(char const * stageLabel, ManifoldDualContouringMesh const & mesh)
            {
                if (!debugMdcTopologyStagesEnabled())
                {
                    return;
                }

                TopologyStats const stats = computeTopologyStats(mesh);
                std::cout << "[mdc-topology] " << stageLabel
                          << ": verts=" << mesh.positions.size()
                          << ", tris=" << stats.triangleCount
                          << ", uniqueEdges=" << stats.uniqueEdgeCount
                          << ", openEdges=" << stats.openEdgeCount
                          << ", nonManifoldEdges=" << stats.nonManifoldEdgeCount;
                if (stats.invalidTriangleCount > 0U)
                {
                    std::cout << ", invalidTris=" << stats.invalidTriangleCount;
                }
                std::cout << std::endl;
            }
        }


    ManifoldDualContouringGpu::ManifoldDualContouringGpu(ComputeCore & core)
        : m_core(core)
    {
        loadKernels();
    }

    void ManifoldDualContouringGpu::setConfig(ManifoldDualContouringConfig config)
    {
        if (config.initialDepth > config.maxDepth)
        {
            config.initialDepth = config.maxDepth;
        }
        m_config = config;
    }

    void ManifoldDualContouringGpu::setMeshGenerationProgressCallback(MeshGenerationProgressCallback callback)
    {
        m_meshGenerationProgressCallback = std::move(callback);
    }

    void ManifoldDualContouringGpu::setCancellationCheckCallback(CancellationCheckCallback callback)
    {
        m_cancellationCheckCallback = std::move(callback);
    }

    bool ManifoldDualContouringGpu::isCancelled() const
    {
        return m_cancellationCheckCallback && m_cancellationCheckCallback();
    }

    void ManifoldDualContouringGpu::reportProgress(float progress, std::string_view phaseName)
    {
        if (m_meshGenerationProgressCallback)
        {
            m_meshGenerationProgressCallback(progress, phaseName);
        }
    }

    void ManifoldDualContouringGpu::loadKernels()
    {
        // Get the shared program instance from ProgramManager
        // This ensures the program has the correct model source set
        auto & programManager = m_core.getProgramManager();

        auto * program = programManager.getManifoldDualContouringProgram();
        if (!program)
        {
            throw std::runtime_error(
              "ManifoldDualContouringProgram not available in ProgramManager");
        }

        // Store a pointer to the program (we don't own it - ProgramManager does)
        m_program = program;
    }

    void ManifoldDualContouringGpu::generateMesh()
    {
        m_mesh.positions.clear();
        m_mesh.normals.clear();
        m_mesh.indices.clear();
        m_lastVertexCount = 0U;

        reportProgress(0.0F, "Initializing");

        if (!m_program)
        {
            std::cerr << "Program not initialized, cannot generate mesh" << std::endl;
            return;
        }

        // Ensure the program is compiled with the current model's SDF
        // This is important when switching between different models
        m_core.getProgramManager().recompileBlockingForManifoldDC();

        // Pre-fetch bounding box for chunking decision
        auto bbox = m_core.getBoundingBox();
        if (!bbox.has_value())
        {
            std::cerr << "No bounding box available" << std::endl;
            return;
        }
        m_cachedBoundingBox = bbox;
        Eigen::Vector3f originalBboxMin =
          Eigen::Vector3f(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        Eigen::Vector3f originalBboxMax =
          Eigen::Vector3f(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);

        // Add margin to bounding box to ensure surface at boundaries is properly captured.
        // The margin should be at least 2 voxels at the finest level to allow proper
        // sign change detection at the surface boundary.
        Eigen::Vector3f const originalSize = originalBboxMax - originalBboxMin;
        float const maxExtent = originalSize.maxCoeff();
        float const voxelSize = maxExtent / static_cast<float>(1U << m_config.maxDepth);
        float const margin = 2.0f * voxelSize;

        m_cachedBboxMin = originalBboxMin - Eigen::Vector3f(margin, margin, margin);
        m_cachedBboxMax = originalBboxMax + Eigen::Vector3f(margin, margin, margin);
        m_cachedBboxSize = m_cachedBboxMax - m_cachedBboxMin;

        if (std::getenv("GLADIUS_DEBUG_MDC_CONFIG") != nullptr)
        {
            std::cout << "MDC generateMesh config: initialDepth=" << m_config.initialDepth
                      << ", maxDepth=" << m_config.maxDepth
                      << ", hierarchical=" << (m_config.enableHierarchicalOctree ? "true" : "false")
                      << ", chunking=" << (m_config.enableChunking ? "true" : "false")
                      << ", minFeatureSize=" << m_config.minFeatureSize << std::endl;
        }

        reportProgress(0.05F, "Building octree");

        // Check for cancellation before expensive operations
        if (isCancelled())
        {
            return;
        }

        // Use hierarchical octree approach if enabled.
        if (m_config.enableHierarchicalOctree)
        {
            generateMeshHierarchical();
        }
        else
        {
            generateMeshNonHierarchical();
        } // End of else block for non-hierarchical processing

        // Check for cancellation after mesh generation
        if (isCancelled())
        {
            m_mesh.positions.clear();
            m_mesh.normals.clear();
            m_mesh.indices.clear();
            return;
        }

        reportProgress(0.65F, "Post-processing");

        // Post-processing for sharp features
        if (m_config.enableSharpFeaturePostProcess && !m_mesh.indices.empty())
        {
            postProcessSharpFeatures();
        }

        // Check for cancellation after post-processing
        if (isCancelled())
        {
            m_mesh.positions.clear();
            m_mesh.normals.clear();
            m_mesh.indices.clear();
            return;
        }

        reportProgress(0.70F, "Simplifying mesh");

        // Mesh simplification (after sharp feature processing)
        // Support both legacy enableSimplification flag and new simplificationMethod enum
        bool const shouldSimplify = (m_config.simplificationMethod != SimplificationMethod::None) ||
                                    m_config.enableSimplification;

        if (shouldSimplify && !m_mesh.indices.empty())
        {
            simplifyMesh();
            
            // Check for cancellation after simplification
            if (isCancelled())
            {
                m_mesh.positions.clear();
                m_mesh.normals.clear();
                m_mesh.indices.clear();
                return;
            }
        }

        reportProgress(0.75F, "Improving mesh quality");

        // Mesh quality improvement (edge flipping to improve triangle aspect ratios)
        if (m_config.enableQualityImprovement && !m_mesh.indices.empty())
        {
            improveMeshQuality();
            
            // Check for cancellation after quality improvement
            if (isCancelled())
            {
                m_mesh.positions.clear();
                m_mesh.normals.clear();
                m_mesh.indices.clear();
                return;
            }
        }

        reportProgress(0.78F, "Projecting to surface");

        // Project vertices to SDF surface AFTER simplification and quality improvement
        // This snaps simplified vertices back onto the iso-surface, fixing off-surface artifacts
        if (m_config.projectToSurface && !m_mesh.indices.empty())
        {
            projectVerticesToSurface();
        }

        // Final cancellation check
        if (isCancelled())
        {
            m_mesh.positions.clear();
            m_mesh.normals.clear();
            m_mesh.indices.clear();
            return;
        }

        reportProgress(0.80F, "Mesh generation complete");
    }

    void ManifoldDualContouringGpu::generateMeshNonHierarchical()
    {
        // Fallback to chunked or single-pass approach.
        // Note: This helper assumes the bounding box and program compilation were already
        // prepared by generateMesh().
        m_mesh.positions.clear();
        m_mesh.normals.clear();
        m_mesh.indices.clear();
        m_lastVertexCount = 0U;

        // Use a conservative voxel size (used as search radius heuristics).
        // This uses the padded bbox (which is what the kernels/octree see).
        float const voxelSize = m_cachedBboxSize.maxCoeff() / static_cast<float>(1U << m_config.maxDepth);

        // Check if chunking is needed
        std::size_t const chunkDivisor = calculateChunkDivisor();
        bool const useChunking =
          m_config.enableChunking && m_config.minFeatureSize > 0.0F && chunkDivisor > 1U;

        if (useChunking)
        {
            std::cout << "Using chunked processing with " << chunkDivisor
                      << "^3 = " << (chunkDivisor * chunkDivisor * chunkDivisor)
                      << " potential chunks" << std::endl;
            std::cout << "  BBox: [" << m_cachedBboxMin.transpose() << "] to ["
                      << m_cachedBboxMax.transpose() << "]" << std::endl;
            std::cout << "  minFeatureSize: " << m_config.minFeatureSize
                      << ", maxDepth: " << m_config.maxDepth << std::endl;

            // Enable chunked mode - disables maxCoord boundary check in generateIndices()
            m_isChunkedMode = true;

            std::vector<ChunkInfo> chunks = generateChunkGrid();
            std::size_t processedChunks = 0U;
            std::size_t emptyChunks = 0U;
            std::size_t const totalChunks = chunks.size();

            ManifoldDualContouringMesh combinedMesh;

            for (auto const & chunk : chunks)
            {
                if (!isChunkNonEmpty(chunk))
                {
                    continue;
                }

                ++processedChunks;
                
                // Report per-chunk progress (5% to 55% range for chunked mesh generation)
                float const chunkProgress = 0.05F + 0.50F * (static_cast<float>(processedChunks) / static_cast<float>(totalChunks));
                reportProgress(chunkProgress, "Processing chunk");
                
                if (processedChunks <= 5U || processedChunks % 50U == 0U)
                {
                    std::cout << "  Processing chunk " << processedChunks << "/"
                              << chunks.size() << " [" << chunk.indexX << "," << chunk.indexY
                              << "," << chunk.indexZ << "]..." << std::endl;
                }

                ManifoldDualContouringMesh chunkMesh;
                generateMeshForChunk(chunk, chunkMesh);

                // Clip triangles to core region to avoid duplicate geometry in overlap areas
                // Each triangle is kept only if its centroid is within the chunk's core region
                clipMeshToCore(chunkMesh, chunk);

                if (chunkMesh.positions.empty())
                {
                    ++emptyChunks;
                }
                else
                {
                    if (processedChunks <= 5U || processedChunks % 50U == 0U)
                    {
                        std::cout << "    Generated " << chunkMesh.positions.size()
                                  << " vertices, " << chunkMesh.indices.size() / 3U
                                  << " triangles" << std::endl;
                    }
                    mergeMeshes(combinedMesh, chunkMesh);
                }
            }

            m_mesh = std::move(combinedMesh);

            std::cout << "Chunk processing complete: " << processedChunks
                      << " chunks processed, " << emptyChunks << " produced no geometry"
                      << std::endl;
            std::cout << "Combined mesh before welding: " << m_mesh.positions.size()
                      << " vertices, " << m_mesh.indices.size() / 3U << " triangles"
                      << std::endl;

            // Weld boundary vertices to make mesh watertight
            if (!m_mesh.positions.empty())
            {
                reportProgress(0.55F, "Welding boundary vertices");
                
                // Calculate appropriate weld tolerance if not specified
                // Use a fraction of the voxel size at maxDepth as tolerance
                float weldTolerance = m_config.chunkWeldTolerance;
                Eigen::Vector3f const chunkSize =
                  m_cachedBboxSize / static_cast<float>(chunkDivisor);
                float const chunkVoxelSize =
                  chunkSize.maxCoeff() / static_cast<float>(1U << m_config.maxDepth);

                if (weldTolerance <= 0.0F)
                {
                    // Chunk size / 2^maxDepth = voxel size within chunk
                    // Use 0.5 * voxel size as tolerance - conservative to avoid
                    // welding vertices that shouldn't be merged
                    weldTolerance = chunkVoxelSize * 0.5F;
                    std::cout << "  Auto weld tolerance: " << weldTolerance
                              << " (voxel size: " << chunkVoxelSize << ")" << std::endl;
                }
                weldBoundaryVertices(weldTolerance);

                dumpTopologyStats("chunked after weldBoundaryVertices", m_mesh);

                reportProgress(0.58F, "Filling boundary gaps");
                
                // Fill gaps between unconnected boundary edges
                // Use voxel size as search radius - edges from neighboring chunks
                // should be within this distance
                fillBoundaryGaps(chunkVoxelSize * 1.5F);

                dumpTopologyStats("chunked after fillBoundaryGaps", m_mesh);
            }

            // Chunk processing can still produce duplicate triangles at overlap boundaries
            // and occasional degenerate bridging triangles. Clean these up before any
            // downstream analysis/export.
            if (!m_mesh.indices.empty())
            {
                reportProgress(0.60F, "Repairing non-manifold edges");
                repairNonManifoldDegree4Edges(m_mesh);

                dumpTopologyStats("chunked after non-manifold repair", m_mesh);

                reportProgress(0.62F, "Removing degenerate triangles");
                removeDegenerateAndDuplicateTriangles(m_mesh);

                dumpTopologyStats("chunked after triangle cleanup", m_mesh);
            }

            // Reset chunked mode flag
            m_isChunkedMode = false;
        }
        else
        {
            // Original single-pass processing (not chunked)
            m_isChunkedMode = false;
            
            reportProgress(0.08F, "Constructing octree");
            constructOctree();
            
            reportProgress(0.25F, "Generating vertices");
            generateVertices();
            
            reportProgress(0.45F, "Generating indices");
            generateIndices();

            dumpTopologyStats("after generateIndices", m_mesh);

            // Fill gaps between disconnected boundary edges.
            // This helps close holes caused by missing neighbor cells in the octree.
            // The function skips edges on the bbox boundary to avoid creating
            // incorrect geometry at domain boundaries.
            if (!m_mesh.indices.empty())
            {
                fillBoundaryGaps(voxelSize * 1.5F);

                dumpTopologyStats("after fillBoundaryGaps", m_mesh);

                repairNonManifoldDegree4Edges(m_mesh);

                dumpTopologyStats("after non-manifold repair", m_mesh);

                // Non-hierarchical index generation and gap filling can emit duplicate triangles in rare
                // ambiguous configurations (leading to non-manifold edges). Remove exact duplicates and
                // degenerate triangles as a conservative post-process.
                removeDegenerateAndDuplicateTriangles(m_mesh);

                dumpTopologyStats("after triangle cleanup", m_mesh);
            }
        }
    }

    void ManifoldDualContouringGpu::constructOctree()
    {
        m_cpuOctreeNodes.clear();
        m_mortonToIndex.clear();

        // Get bounding box from compute core
        auto bbox = m_core.getBoundingBox();
        if (!bbox.has_value())
        {
            std::cerr << "No bounding box available for octree construction" << std::endl;
            return;
        }
        m_cachedBoundingBox = bbox;

        // Get primitives
        auto primitives = m_core.getPrimitives();
        if (!primitives)
        {
            std::cerr << "No primitives available" << std::endl;
            return;
        }

        Eigen::Vector3f bboxMin(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        Eigen::Vector3f bboxMax(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);

        // Add margin to bounding box to ensure surface at boundaries is properly captured.
        // The margin should be at least 2 voxels at the finest level to allow proper
        // sign change detection at the surface boundary.
        Eigen::Vector3f const originalSize = bboxMax - bboxMin;
        float const maxExtent = originalSize.maxCoeff();
        std::uint32_t const depth = static_cast<std::uint32_t>(m_config.maxDepth);
        float const voxelSize = maxExtent / static_cast<float>(1U << depth);
        float const margin = 2.0f * voxelSize;

        bboxMin -= Eigen::Vector3f(margin, margin, margin);
        bboxMax += Eigen::Vector3f(margin, margin, margin);

        m_cachedBboxMin = bboxMin;
        m_cachedBboxMax = bboxMax;
        m_cachedBboxSize = bboxMax - bboxMin;
        m_octreeDepth = depth;
        if (m_octreeDepth >= 31U)
        {
            m_gridResolution = std::numeric_limits<std::uint32_t>::max();
        }
        else
        {
            m_gridResolution = 1U << m_octreeDepth;
            if (m_gridResolution == 0U)
            {
                m_gridResolution = 1U;
            }
        }

        std::cout << "Constructing Octree. Original BBox: ["
                  << (bboxMin + Eigen::Vector3f(margin, margin, margin)).transpose() << "] to ["
                  << (bboxMax - Eigen::Vector3f(margin, margin, margin)).transpose()
                  << "], Padded BBox: [" << bboxMin.transpose() << "] to [" << bboxMax.transpose()
                  << "], Margin: " << margin << " (voxelSize=" << voxelSize << ")"
                  << ", Extents: " << m_cachedBboxSize.transpose()
                  << ", initialDepth: " << m_config.initialDepth
                  << ", maxDepth: " << m_config.maxDepth << std::endl;

        try
        {
            m_program->constructOctree(m_octreeBuffer,
                                       m_octreeNodeCount,
                                       bboxMin,
                                       bboxMax,
                                       static_cast<std::uint32_t>(m_config.initialDepth),
                                       static_cast<std::uint32_t>(m_config.maxDepth),
                                       *primitives,
                                       m_config.isoValue);

            std::cout << "Octree construction complete. Nodes before halo: " << m_octreeNodeCount
                      << std::endl;

            // Add halo nodes around surface-crossing cells to ensure all neighbors exist for quad
            // generation. This fixes holes in thin structures where boundary cells lack neighbors.
            std::uint32_t const maxCoord = m_gridResolution - 1;
            m_program->addHaloNodes(m_octreeBuffer,
                                    m_octreeNodeCount,
                                    maxCoord,
                                    static_cast<std::uint8_t>(m_config.maxDepth),
                                    bboxMin,
                                    bboxMax,
                                    *primitives,
                                    m_config.isoValue);

            std::cout << "Octree construction complete. Total nodes after halo: "
                      << m_octreeNodeCount << std::endl;
            refreshCpuOctreeCache();
        }
        catch (std::exception & e)
        {
            std::cerr << "Error in octree construction: " << e.what() << std::endl;
        }
    }

    void ManifoldDualContouringGpu::generateVertices()
    {
        auto context = m_core.getComputeContext();
        auto & queue = context->GetQueue();

        if (m_octreeNodeCount == 0)
        {
            std::cerr << "No octree nodes to generate vertices from" << std::endl;
            return;
        }

        size_t numNodes = m_octreeNodeCount;

        // IMPORTANT: Sort octree by Morton code BEFORE generating vertices.
        // This ensures vertexOffsets[i] corresponds to the sorted node at index i,
        // which is required for emit_indices to find correct neighbor vertices.
        m_program->sortOctreeByMorton(m_octreeBuffer, numNodes);

        // Get primitives
        auto primitives = m_core.getPrimitives();
        if (!primitives)
        {
            std::cerr << "No primitives available" << std::endl;
            return;
        }

        Eigen::Vector3f bboxMin = m_cachedBboxMin;
        Eigen::Vector3f bboxMax = m_cachedBboxMax;

        // Calculate voxel size and gradient epsilon based on octree depth
        Eigen::Vector3f const bboxSize = bboxMax - bboxMin;
        float const maxExtent = bboxSize.maxCoeff();
        float const voxelSize = maxExtent / static_cast<float>(1U << m_config.maxDepth);
        // Use 10% of voxel size for gradient computation - balances detail vs noise
        float const gradientEpsilon = voxelSize * 0.1f;

        // 1. Count vertices (1-4 per cell based on discontinuity detection)
        m_countBuffer = context->createBufferChecked(CL_MEM_READ_WRITE, numNodes * sizeof(int));

        try
        {
            m_program->countVertices(*m_octreeBuffer,
                                     *m_countBuffer,
                                     numNodes,
                                     bboxMin,
                                     bboxMax,
                                     *primitives,
                                     m_config.isoValue,
                                     gradientEpsilon);
        }
        catch (std::exception & e)
        {
            std::cerr << "Error running count_vertices: " << e.what() << std::endl;
            return;
        }

        // 2. Scan (Prefix Sum)
        // For this initial implementation, we perform the scan on the CPU.
        // For high performance, this should be replaced with a GPU-based scan (e.g., Blelloch
        // scan).
        std::vector<int> counts(numNodes);
        try
        {
            queue.enqueueReadBuffer(
              *m_countBuffer, CL_TRUE, 0, numNodes * sizeof(int), counts.data());
        }
        catch (std::exception & e)
        {
            std::cerr << "Error reading count buffer: " << e.what() << std::endl;
            return;
        }

        std::vector<int> offsets(numNodes);
        int totalVertices = 0;
        for (size_t i = 0; i < numNodes; ++i)
        {
            offsets[i] = totalVertices;
            totalVertices += counts[i];
        }

        m_cpuVertexOffsets = offsets;

        std::cout << "Generating " << totalVertices << " vertices from " << numNodes
                  << " octree nodes" << std::endl;

        if (totalVertices == 0)
        {
            std::cout << "No vertices to generate" << std::endl;
            return;
        }

        m_lastVertexCount = static_cast<std::size_t>(totalVertices);

        m_offsetBuffer = context->createBufferChecked(
          CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, numNodes * sizeof(int), offsets.data());

        // Allocate vertex buffer (float4 position + float4 normal = 32 bytes)
        m_vertexBuffer = context->createBufferChecked(CL_MEM_READ_WRITE, totalVertices * 32);

                // Per-node (not per-vertex) component map: 12 local edges per node.
                // This is used by the watertight index generation kernel to pick the correct
                // component vertex for each participating cell.
                m_edgeComponentBuffer =
                    context->createBufferChecked(CL_MEM_READ_WRITE, numNodes * 12U * sizeof(cl_uchar));

        try
        {
            m_program->generateVertices(*m_octreeBuffer,
                                        *m_offsetBuffer,
                                        *m_vertexBuffer,
                                        *m_edgeComponentBuffer,
                                        numNodes,
                                        bboxMin,
                                        bboxMax,
                                        *primitives,
                                        m_config.isoValue,
                                        gradientEpsilon);

            std::vector<GpuVertex> hostVertices(static_cast<std::size_t>(totalVertices));
            queue.enqueueReadBuffer(*m_vertexBuffer,
                                    CL_TRUE,
                                    0,
                                    hostVertices.size() * sizeof(GpuVertex),
                                    hostVertices.data());

            m_mesh.positions.clear();
            m_mesh.normals.clear();
            m_mesh.positions.reserve(hostVertices.size());
            m_mesh.normals.reserve(hostVertices.size());

            for (auto const & vertex : hostVertices)
            {
                m_mesh.positions.emplace_back(
                  vertex.position.s[0], vertex.position.s[1], vertex.position.s[2]);
                Eigen::Vector3f normal(vertex.normal.s[0], vertex.normal.s[1], vertex.normal.s[2]);
                if (normal.squaredNorm() > 1e-12F)
                {
                    normal.normalize();
                }
                else
                {
                    normal = Eigen::Vector3f{0.0F, 1.0F, 0.0F};
                }
                m_mesh.normals.emplace_back(normal);
            }

            std::cout << "Generated " << m_mesh.positions.size() << " vertices" << std::endl;
        }
        catch (std::exception const & e)
        {
            std::cerr << "Error running emit_vertices: " << e.what() << std::endl;
            m_mesh.positions.clear();
            m_mesh.normals.clear();
            m_lastVertexCount = 0U;
        }
    }

    void ManifoldDualContouringGpu::generateIndices()
    {
        m_mesh.indices.clear();

        if (m_mesh.positions.empty())
        {
            return;
        }

        if (m_octreeNodeCount == 0U || !m_octreeBuffer)
        {
            std::cerr << "No octree data available for index generation" << std::endl;
            return;
        }

        auto context = m_core.getComputeContext();
        if (!context)
        {
            std::cerr << "Compute context unavailable" << std::endl;
            return;
        }

        auto & queue = context->GetQueue();
        std::size_t const numNodes = m_octreeNodeCount;

        try
        {
            // Note: Octree was already sorted in generateVertices()
            // The sorted order ensures vertexOffsets[i] matches sorted node i

            // Calculate maxCoord for bounds checking in kernels (2^depth - 1)
            std::uint32_t const maxCoord = m_gridResolution - 1U;

            // In chunked mode, disable boundary checks since chunk boundaries
            // are internal and we want to emit quads there
            std::uint32_t const disableBoundaryChecks = m_isChunkedMode ? 1U : 0U;

            // 1. Count quads per cell
            auto quadCountBuffer =
              context->createBufferChecked(CL_MEM_READ_WRITE, numNodes * sizeof(int));
            m_program->countQuads(
              *m_octreeBuffer, *quadCountBuffer, numNodes, maxCoord, disableBoundaryChecks);

            // 2. CPU-side prefix sum for index offsets
            std::vector<int> quadCounts(numNodes);
            queue.enqueueReadBuffer(
              *quadCountBuffer, CL_TRUE, 0, numNodes * sizeof(int), quadCounts.data());

            // Debug: Count total quads and cells with quads
            int totalQuads = 0;
            int cellsWithQuads = 0;
            for (std::size_t i = 0U; i < numNodes; ++i)
            {
                totalQuads += quadCounts[i];
                if (quadCounts[i] > 0)
                    cellsWithQuads++;
            }
            std::cout << "Quad counting: " << totalQuads << " quads from " << cellsWithQuads
                      << " cells (of " << numNodes << " total nodes)" << std::endl;

            // Run diagnostic analysis to understand boundary hole causes
            auto diagnostics = m_program->runQuadDiagnostics(*m_octreeBuffer, numNodes, maxCoord);
            std::cout << "\n=== Boundary Hole Diagnostics (All 12 Edges) ===" << std::endl;

            // Edge axis names for better readability
            static char const * edgeNames[] = {
              "Edge  0 (X at y=0,z=0)", // corners 0-1
              "Edge  1 (Y at x=1,z=0)", // corners 1-3
              "Edge  2 (X at y=1,z=0)", // corners 2-3
              "Edge  3 (Y at x=0,z=0)", // corners 0-2
              "Edge  4 (X at y=0,z=1)", // corners 4-5
              "Edge  5 (Y at x=1,z=1)", // corners 5-7, owner
              "Edge  6 (X at y=1,z=1)", // corners 6-7, owner
              "Edge  7 (Y at x=0,z=1)", // corners 4-6
              "Edge  8 (Z at x=0,y=0)", // corners 0-4
              "Edge  9 (Z at x=1,y=0)", // corners 1-5
              "Edge 10 (Z at x=1,y=1)", // corners 3-7, owner
              "Edge 11 (Z at x=0,y=1)"  // corners 2-6
            };

            int totalEmitted = 0;
            int totalSkipped = 0;
            for (int e = 0; e < 12; ++e)
            {
                int emitted = diagnostics.edgeEmitted[static_cast<std::size_t>(e)];
                int skipped = diagnostics.edgeSkipped[static_cast<std::size_t>(e)];
                if (emitted > 0 || skipped > 0)
                {
                    std::cout << edgeNames[e] << ": emitted=" << emitted << ", skipped=" << skipped
                              << std::endl;
                }
                totalEmitted += emitted;
                totalSkipped += skipped;
            }
            std::cout << "Summary: " << totalEmitted << " edges emitted, " << totalSkipped
                      << " edges skipped" << std::endl;
            std::cout << "=================================================\n" << std::endl;

            // Run discontinuity diagnostic to detect CSG-related gradient issues
            BBox paddedBbox;
            paddedBbox.extend(m_cachedBboxMin);
            paddedBbox.extend(m_cachedBboxMax);
            Eigen::Vector3f const bboxSize = m_cachedBboxMax - m_cachedBboxMin;
            float const maxExtent = bboxSize.maxCoeff();
            float const voxelSize = maxExtent / static_cast<float>(1U << m_config.maxDepth);
            float const gradientEpsilon = voxelSize * 0.1F;

            // Get primitives for discontinuity diagnostic
            auto primitives = m_core.getPrimitives();
            if (primitives)
            {
                auto discDiag = m_program->runDiscontinuityDiagnostics(*m_octreeBuffer,
                                                                       numNodes,
                                                                       paddedBbox,
                                                                       *primitives,
                                                                       m_config.isoValue,
                                                                       gradientEpsilon);

                if (discDiag.cells2Components > 0 || discDiag.cells3Components > 0 ||
                    discDiag.cells4Components > 0)
                {
                    std::cout << "\n=== Gradient Discontinuity Analysis ===" << std::endl;
                    std::cout << "Cells analyzed: " << discDiag.totalCells << std::endl;
                    std::cout << "  1 component (smooth): " << discDiag.cells1Component << " ("
                              << (100.0F * static_cast<float>(discDiag.cells1Component) /
                                  static_cast<float>(discDiag.totalCells))
                              << "%)" << std::endl;
                    std::cout << "  2 components: " << discDiag.cells2Components << " ("
                              << (100.0F * static_cast<float>(discDiag.cells2Components) /
                                  static_cast<float>(discDiag.totalCells))
                              << "%)" << std::endl;
                    if (discDiag.cells3Components > 0)
                    {
                        std::cout << "  3 components: " << discDiag.cells3Components << std::endl;
                    }
                    if (discDiag.cells4Components > 0)
                    {
                        std::cout << "  4 components: " << discDiag.cells4Components << std::endl;
                    }
                    std::cout << "Average discontinuity score: " << discDiag.avgDiscontinuityScore
                              << std::endl;
                    std::cout << "Severe discontinuities (>0.5): " << discDiag.severeDiscontinuities
                              << std::endl;
                    std::cout << "========================================\n" << std::endl;
                }
            }

            std::vector<int> indexOffsets(numNodes);
            int totalIndices = 0;
            for (std::size_t i = 0U; i < numNodes; ++i)
            {
                indexOffsets[i] = totalIndices;
                totalIndices += quadCounts[i];
            }

            if (totalIndices == 0)
            {
                return;
            }

            auto indexOffsetBuffer = context->createBufferChecked(
              CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, numNodes * sizeof(int), indexOffsets.data());

            // 4. Allocate index buffer and emit indices
            m_indexBuffer =
              context->createBufferChecked(CL_MEM_READ_WRITE, totalIndices * sizeof(std::uint32_t));

                        if (!m_edgeComponentBuffer)
                        {
                                // Should have been produced during generateVertices(). Keep a safe default to
                                // avoid undefined reads in the kernel if something goes wrong.
                                std::vector<cl_uchar> zeros(numNodes * 12U, 0);
                                m_edgeComponentBuffer = context->createBufferChecked(
                                    CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                    zeros.size() * sizeof(cl_uchar),
                                    zeros.data());
                        }

                        m_program->generateIndices(*m_octreeBuffer,
                                                                             *m_offsetBuffer,
                                                                             *m_edgeComponentBuffer,
                                                                             *indexOffsetBuffer,
                                                                             *m_indexBuffer,
                                                                             numNodes,
                                                                             maxCoord,
                                                                             disableBoundaryChecks);

            // 5. Read back indices
            m_mesh.indices.resize(static_cast<std::size_t>(totalIndices));
            queue.enqueueReadBuffer(*m_indexBuffer,
                                    CL_TRUE,
                                    0,
                                    totalIndices * sizeof(std::uint32_t),
                                    m_mesh.indices.data());

            std::cout << "Generated " << (m_mesh.indices.size() / 3U) << " triangles via GPU"
                      << std::endl;
        }
        catch (std::exception const & e)
        {
            std::cerr << "GPU index generation failed: " << e.what() << std::endl;
            m_mesh.indices.clear();
        }
    }

    void ManifoldDualContouringGpu::refreshCpuOctreeCache()
    {
        m_cpuOctreeNodes.clear();
        m_mortonToIndex.clear();

        if (m_octreeNodeCount == 0U || !m_octreeBuffer)
        {
            return;
        }

        auto context = m_core.getComputeContext();
        if (!context)
        {
            std::cerr << "Compute context unavailable; cannot read octree buffer" << std::endl;
            return;
        }

        try
        {
            m_cpuOctreeNodes.resize(m_octreeNodeCount);
            context->GetQueue().enqueueReadBuffer(*m_octreeBuffer,
                                                  CL_TRUE,
                                                  0,
                                                  m_cpuOctreeNodes.size() * sizeof(OctreeNode),
                                                  m_cpuOctreeNodes.data());
        }
        catch (std::exception const & e)
        {
            std::cerr << "Failed to read octree buffer: " << e.what() << std::endl;
            m_cpuOctreeNodes.clear();
            return;
        }

        m_mortonToIndex.reserve(m_cpuOctreeNodes.size());
        for (std::size_t i = 0; i < m_cpuOctreeNodes.size(); ++i)
        {
            m_mortonToIndex[m_cpuOctreeNodes[i].mortonCode] = i;
        }
    }

    void ManifoldDualContouringGpu::postProcessSharpFeatures()
    {
        std::cout << "Starting sharp feature post-processing..." << std::endl;

        // Step 1: Detect triangles with sharp features
        auto sharpTriangles = detectSharpTriangles();

        if (sharpTriangles.empty())
        {
            std::cout << "No sharp triangles detected, skipping post-processing" << std::endl;
            return;
        }

        std::cout << "Detected " << sharpTriangles.size() << " sharp triangles for processing"
                  << std::endl;

        // Step 2: Subdivide the flagged triangles
        for (std::size_t iter = 0U; iter < m_config.subdivisionIterations; ++iter)
        {
            subdivideTriangles(sharpTriangles);

            // Re-detect sharp triangles for next iteration
            if (iter + 1U < m_config.subdivisionIterations)
            {
                sharpTriangles = detectSharpTriangles();
            }
        }

        // Note: Vertex projection moved to after simplification for better results

        std::cout << "Sharp feature post-processing complete" << std::endl;
    }

    std::vector<std::size_t> ManifoldDualContouringGpu::detectSharpTriangles()
    {
        std::vector<std::size_t> result;

        if (m_mesh.indices.size() < 3U || m_mesh.normals.empty())
        {
            return result;
        }

        // The threshold is already a cosine value in the config
        float const cosThreshold = m_config.sharpFeatureAngleThreshold;

        std::size_t const numTriangles = m_mesh.indices.size() / 3U;
        result.reserve(numTriangles / 10U); // Assume ~10% might be sharp

        for (std::size_t triIdx = 0U; triIdx < numTriangles; ++triIdx)
        {
            std::size_t const baseIdx = triIdx * 3U;
            auto const i0 = m_mesh.indices[baseIdx + 0U];
            auto const i1 = m_mesh.indices[baseIdx + 1U];
            auto const i2 = m_mesh.indices[baseIdx + 2U];

            // Get vertex normals
            if (i0 >= m_mesh.normals.size() || i1 >= m_mesh.normals.size() ||
                i2 >= m_mesh.normals.size())
            {
                continue;
            }

            auto const & n0 = m_mesh.normals[i0];
            auto const & n1 = m_mesh.normals[i1];
            auto const & n2 = m_mesh.normals[i2];

            // Check dot products between vertex normals (using Eigen Vector3f member functions)
            float const dot01 = n0.x() * n1.x() + n0.y() * n1.y() + n0.z() * n1.z();
            float const dot12 = n1.x() * n2.x() + n1.y() * n2.y() + n1.z() * n2.z();
            float const dot20 = n2.x() * n0.x() + n2.y() * n0.y() + n2.z() * n0.z();

            // If any pair of normals differs significantly, flag this triangle
            if (dot01 < cosThreshold || dot12 < cosThreshold || dot20 < cosThreshold)
            {
                result.push_back(triIdx);
            }
        }

        return result;
    }

    void
    ManifoldDualContouringGpu::subdivideTriangles(std::vector<std::size_t> const & triangleIndices)
    {
        if (triangleIndices.empty())
        {
            return;
        }

        // For manifold subdivision, we need to track edges and their midpoints
        // to ensure shared edges get the same midpoint vertex
        struct Edge
        {
            std::uint32_t v0;
            std::uint32_t v1;

            bool operator<(Edge const & other) const
            {
                auto const minThis = std::min(v0, v1);
                auto const maxThis = std::max(v0, v1);
                auto const minOther = std::min(other.v0, other.v1);
                auto const maxOther = std::max(other.v0, other.v1);
                return (minThis < minOther) || (minThis == minOther && maxThis < maxOther);
            }

            bool operator==(Edge const & other) const
            {
                auto const minThis = std::min(v0, v1);
                auto const maxThis = std::max(v0, v1);
                auto const minOther = std::min(other.v0, other.v1);
                auto const maxOther = std::max(other.v0, other.v1);
                return minThis == minOther && maxThis == maxOther;
            }
        };

        // Map from edge to its midpoint vertex index
        std::map<Edge, std::uint32_t> edgeMidpoints;

        // Helper to get or create midpoint vertex for an edge
        auto getOrCreateMidpoint = [&](std::uint32_t v0, std::uint32_t v1) -> std::uint32_t
        {
            Edge edge{v0, v1};
            auto it = edgeMidpoints.find(edge);
            if (it != edgeMidpoints.end())
            {
                return it->second;
            }

            // Create new midpoint vertex
            auto const & p0 = m_mesh.positions[v0];
            auto const & p1 = m_mesh.positions[v1];
            Eigen::Vector3f midPos{
              (p0.x() + p1.x()) * 0.5F, (p0.y() + p1.y()) * 0.5F, (p0.z() + p1.z()) * 0.5F};

            // Interpolate normal
            auto const & n0 = m_mesh.normals[v0];
            auto const & n1 = m_mesh.normals[v1];
            Eigen::Vector3f midNormal{
              (n0.x() + n1.x()) * 0.5F, (n0.y() + n1.y()) * 0.5F, (n0.z() + n1.z()) * 0.5F};

            // Normalize the interpolated normal
            float const len = midNormal.norm();
            if (len > 1e-6F)
            {
                midNormal /= len;
            }

            auto const newIdx = static_cast<std::uint32_t>(m_mesh.positions.size());
            m_mesh.positions.push_back(midPos);
            m_mesh.normals.push_back(midNormal);

            edgeMidpoints[edge] = newIdx;
            return newIdx;
        };

        // Collect triangles to subdivide into a set for O(1) lookup
        std::set<std::size_t> trianglesToSubdivide(triangleIndices.begin(), triangleIndices.end());

        // First pass: collect all edges that will be split (from triangles to subdivide)
        std::set<Edge> edgesToSplit;
        for (auto triIdx : trianglesToSubdivide)
        {
            std::size_t const baseIdx = triIdx * 3U;
            auto const i0 = m_mesh.indices[baseIdx + 0U];
            auto const i1 = m_mesh.indices[baseIdx + 1U];
            auto const i2 = m_mesh.indices[baseIdx + 2U];
            edgesToSplit.insert(Edge{i0, i1});
            edgesToSplit.insert(Edge{i1, i2});
            edgesToSplit.insert(Edge{i2, i0});
        }

        // Build new index buffer
        std::vector<std::uint32_t> newIndices;
        newIndices.reserve(m_mesh.indices.size() + triangleIndices.size() * 9U);

        std::size_t const numTriangles = m_mesh.indices.size() / 3U;
        for (std::size_t triIdx = 0U; triIdx < numTriangles; ++triIdx)
        {
            std::size_t const baseIdx = triIdx * 3U;
            auto const i0 = m_mesh.indices[baseIdx + 0U];
            auto const i1 = m_mesh.indices[baseIdx + 1U];
            auto const i2 = m_mesh.indices[baseIdx + 2U];

            if (trianglesToSubdivide.count(triIdx) > 0U)
            {
                // Subdivide this triangle into 4 triangles using midpoints
                // Create midpoint on each edge
                auto const m01 = getOrCreateMidpoint(i0, i1);
                auto const m12 = getOrCreateMidpoint(i1, i2);
                auto const m20 = getOrCreateMidpoint(i2, i0);

                // Create 4 new triangles:
                // Triangle 1: i0, m01, m20
                newIndices.push_back(i0);
                newIndices.push_back(m01);
                newIndices.push_back(m20);

                // Triangle 2: m01, i1, m12
                newIndices.push_back(m01);
                newIndices.push_back(i1);
                newIndices.push_back(m12);

                // Triangle 3: m20, m12, i2
                newIndices.push_back(m20);
                newIndices.push_back(m12);
                newIndices.push_back(i2);

                // Triangle 4: m01, m12, m20 (center triangle)
                newIndices.push_back(m01);
                newIndices.push_back(m12);
                newIndices.push_back(m20);
            }
            else
            {
                // Check if any edges of this triangle need to be split
                // to match neighboring subdivided triangles (T-junction fix)
                bool const split01 = edgesToSplit.count(Edge{i0, i1}) > 0U;
                bool const split12 = edgesToSplit.count(Edge{i1, i2}) > 0U;
                bool const split20 = edgesToSplit.count(Edge{i2, i0}) > 0U;

                int const splitCount = (split01 ? 1 : 0) + (split12 ? 1 : 0) + (split20 ? 1 : 0);

                if (splitCount == 0)
                {
                    // No edges to split, keep original triangle
                    newIndices.push_back(i0);
                    newIndices.push_back(i1);
                    newIndices.push_back(i2);
                }
                else if (splitCount == 1)
                {
                    // One edge split: create 2 triangles
                    if (split01)
                    {
                        auto const m = getOrCreateMidpoint(i0, i1);
                        newIndices.push_back(i0);
                        newIndices.push_back(m);
                        newIndices.push_back(i2);

                        newIndices.push_back(m);
                        newIndices.push_back(i1);
                        newIndices.push_back(i2);
                    }
                    else if (split12)
                    {
                        auto const m = getOrCreateMidpoint(i1, i2);
                        newIndices.push_back(i0);
                        newIndices.push_back(i1);
                        newIndices.push_back(m);

                        newIndices.push_back(i0);
                        newIndices.push_back(m);
                        newIndices.push_back(i2);
                    }
                    else // split20
                    {
                        auto const m = getOrCreateMidpoint(i2, i0);
                        newIndices.push_back(i0);
                        newIndices.push_back(i1);
                        newIndices.push_back(m);

                        newIndices.push_back(m);
                        newIndices.push_back(i1);
                        newIndices.push_back(i2);
                    }
                }
                else if (splitCount == 2)
                {
                    // Two edges split: create 3 triangles
                    if (!split01) // split12 and split20
                    {
                        auto const m12 = getOrCreateMidpoint(i1, i2);
                        auto const m20 = getOrCreateMidpoint(i2, i0);
                        newIndices.push_back(i0);
                        newIndices.push_back(i1);
                        newIndices.push_back(m12);

                        newIndices.push_back(i0);
                        newIndices.push_back(m12);
                        newIndices.push_back(m20);

                        newIndices.push_back(m20);
                        newIndices.push_back(m12);
                        newIndices.push_back(i2);
                    }
                    else if (!split12) // split01 and split20
                    {
                        auto const m01 = getOrCreateMidpoint(i0, i1);
                        auto const m20 = getOrCreateMidpoint(i2, i0);
                        newIndices.push_back(i0);
                        newIndices.push_back(m01);
                        newIndices.push_back(m20);

                        newIndices.push_back(m01);
                        newIndices.push_back(i1);
                        newIndices.push_back(m20);

                        newIndices.push_back(m20);
                        newIndices.push_back(i1);
                        newIndices.push_back(i2);
                    }
                    else // split01 and split12
                    {
                        auto const m01 = getOrCreateMidpoint(i0, i1);
                        auto const m12 = getOrCreateMidpoint(i1, i2);
                        newIndices.push_back(i0);
                        newIndices.push_back(m01);
                        newIndices.push_back(i2);

                        newIndices.push_back(m01);
                        newIndices.push_back(i1);
                        newIndices.push_back(m12);

                        newIndices.push_back(m01);
                        newIndices.push_back(m12);
                        newIndices.push_back(i2);
                    }
                }
                else // splitCount == 3
                {
                    // All three edges split: create 4 triangles (same as full subdivide)
                    auto const m01 = getOrCreateMidpoint(i0, i1);
                    auto const m12 = getOrCreateMidpoint(i1, i2);
                    auto const m20 = getOrCreateMidpoint(i2, i0);

                    newIndices.push_back(i0);
                    newIndices.push_back(m01);
                    newIndices.push_back(m20);

                    newIndices.push_back(m01);
                    newIndices.push_back(i1);
                    newIndices.push_back(m12);

                    newIndices.push_back(m20);
                    newIndices.push_back(m12);
                    newIndices.push_back(i2);

                    newIndices.push_back(m01);
                    newIndices.push_back(m12);
                    newIndices.push_back(m20);
                }
            }
        }

        m_mesh.indices = std::move(newIndices);

        std::cout << "Subdivision added " << edgeMidpoints.size() << " midpoint vertices"
                  << std::endl;
        std::cout << "New triangle count: " << (m_mesh.indices.size() / 3U) << std::endl;
    }

    void ManifoldDualContouringGpu::projectVerticesToSurface()
    {
        auto context = m_core.getComputeContext();
        if (!context)
        {
            std::cerr << "Compute context unavailable for vertex projection" << std::endl;
            return;
        }

        try
        {
            std::size_t const numVertices = m_mesh.positions.size();
            if (numVertices == 0U)
            {
                return;
            }

            // Create a VertexBuffer and populate it with positions as cl_float4
            VertexBuffer vertexBuffer(*context);
            auto & bufferData = vertexBuffer.getData();
            bufferData.resize(numVertices);

            for (std::size_t i = 0U; i < numVertices; ++i)
            {
                auto const & pos = m_mesh.positions[i];
                bufferData[i] = {pos.x(), pos.y(), pos.z(), 0.0F};
            }

            // Write to GPU
            vertexBuffer.write();

            // Use ComputeCore's adoptVertexOfMeshToSurface which handles primitives setup
            m_core.adoptVertexOfMeshToSurface(vertexBuffer);

            // Read back projected positions
            vertexBuffer.read();

            // Convert back to Eigen::Vector3f
            for (std::size_t i = 0U; i < numVertices; ++i)
            {
                auto const & pos = bufferData[i];
                m_mesh.positions[i] = Eigen::Vector3f{pos.s[0], pos.s[1], pos.s[2]};
            }

            std::cout << "Projected " << numVertices << " vertices to SDF surface" << std::endl;
        }
        catch (std::exception const & e)
        {
            std::cerr << "Vertex projection failed: " << e.what() << std::endl;
        }
    }

    float ManifoldDualContouringGpu::evaluateSdf(Eigen::Vector3f const & pos) const
    {
        // For single-point SDF evaluation, we use the precomputed SDF grid if available
        // This is a simplified approach - for production, consider caching the SDF grid
        auto resources = m_core.getResourceContext();
        if (!resources)
        {
            return 0.0F;
        }

        auto & sdfBuffer = resources->getPrecompSdfBuffer();
        auto const width = sdfBuffer.getWidth();
        auto const height = sdfBuffer.getHeight();
        auto const depth = sdfBuffer.getDepth();

        if (width == 0U || height == 0U || depth == 0U || !m_cachedBoundingBox.has_value())
        {
            return 0.0F;
        }

        // Transform world position to normalized coordinates within the bounding box
        Eigen::Vector3f const extent = m_cachedBboxMax - m_cachedBboxMin;
        Eigen::Vector3f const safeExtent = extent.cwiseMax(Eigen::Vector3f::Constant(1e-6F));
        Eigen::Vector3f normalized = (pos - m_cachedBboxMin).cwiseQuotient(safeExtent);
        normalized = normalized.cwiseMax(Eigen::Vector3f::Zero()).cwiseMin(Eigen::Vector3f::Ones());

        // Convert to grid indices
        float const gx = normalized.x() * static_cast<float>(width - 1U);
        float const gy = normalized.y() * static_cast<float>(height - 1U);
        float const gz = normalized.z() * static_cast<float>(depth - 1U);

        auto const x0 = static_cast<std::size_t>(std::floor(gx));
        auto const y0 = static_cast<std::size_t>(std::floor(gy));
        auto const z0 = static_cast<std::size_t>(std::floor(gz));
        auto const x1 = std::min(x0 + 1U, width - 1U);
        auto const y1 = std::min(y0 + 1U, height - 1U);
        auto const z1 = std::min(z0 + 1U, depth - 1U);

        float const fx = gx - static_cast<float>(x0);
        float const fy = gy - static_cast<float>(y0);
        float const fz = gz - static_cast<float>(z0);

        // Trilinear interpolation
        auto const & data = sdfBuffer.getData();
        auto index = [&](std::size_t x, std::size_t y, std::size_t z) -> std::size_t
        { return z * width * height + y * width + x; };

        float const v000 = data[index(x0, y0, z0)];
        float const v100 = data[index(x1, y0, z0)];
        float const v010 = data[index(x0, y1, z0)];
        float const v110 = data[index(x1, y1, z0)];
        float const v001 = data[index(x0, y0, z1)];
        float const v101 = data[index(x1, y0, z1)];
        float const v011 = data[index(x0, y1, z1)];
        float const v111 = data[index(x1, y1, z1)];

        float const c00 = v000 * (1.0F - fx) + v100 * fx;
        float const c10 = v010 * (1.0F - fx) + v110 * fx;
        float const c01 = v001 * (1.0F - fx) + v101 * fx;
        float const c11 = v011 * (1.0F - fx) + v111 * fx;
        float const c0 = c00 * (1.0F - fy) + c10 * fy;
        float const c1 = c01 * (1.0F - fy) + c11 * fy;

        return c0 * (1.0F - fz) + c1 * fz;
    }

    void ManifoldDualContouringGpu::simplifyMesh()
    {
        std::size_t const initialTriangles = m_mesh.indices.size() / 3U;
        std::size_t const initialVertices = m_mesh.positions.size();

        if (initialTriangles < 2U || initialVertices < 4U)
        {
            std::cout << "Mesh too small for simplification" << std::endl;
            return;
        }

        // Determine which method to use
        SimplificationMethod method = m_config.simplificationMethod;

        // Legacy support: if enableSimplification is set but method is None, use QEM
        if (method == SimplificationMethod::None && m_config.enableSimplification)
        {
            method = SimplificationMethod::QemSdfAware;
        }

        switch (method)
        {
        case SimplificationMethod::QemSdfAware:
            simplifyMeshQemSdfAware();
            break;

        case SimplificationMethod::None:
        default:
            // Should not reach here due to caller check, but handle gracefully
            break;
        }
    }

    void ManifoldDualContouringGpu::simplifyMeshQemSdfAware()
    {
        std::cout << "Starting QEM SDF-aware mesh simplification..." << std::endl;
        std::size_t const initialTriangles = m_mesh.indices.size() / 3U;
        std::size_t const initialVertices = m_mesh.positions.size();

        // Configure QEM simplifier
        QemSimplificationConfig qemConfig;
        qemConfig.targetTriangleCount = m_config.simplificationTargetTriangles;
        qemConfig.targetReductionPercent = m_config.simplificationTargetReduction;
        qemConfig.sdfErrorWeight = m_config.simplificationSdfWeight;
        qemConfig.qemErrorWeight = m_config.simplificationQemWeight;
        qemConfig.normalDeviationWeight = m_config.simplificationNormalWeight;
        qemConfig.maxSdfError = m_config.simplificationMaxSdfError;
        qemConfig.maxQemError = m_config.simplificationMaxQemError;
        qemConfig.maxNormalDeviation = m_config.simplificationMaxNormalDeviation;
        qemConfig.sharpEdgeAngleThreshold = m_config.simplificationSharpEdgeThreshold;
        qemConfig.batchSize = m_config.simplificationBatchSize;
        qemConfig.maxPasses = m_config.simplificationMaxPasses;

        // Create SDF evaluation callback using GPU batch evaluation
        auto sdfCallback =
          [this](std::vector<Eigen::Vector3f> const & positions) -> std::vector<float>
        { return evaluateSdfBatchGpu(positions); };

        // Create SDF gradient evaluation callback for normal deviation checking
        auto gradientCallback =
          [this](std::vector<Eigen::Vector3f> const & positions) -> std::vector<Eigen::Vector3f>
        { return evaluateSdfGradientBatchGpu(positions); };

        // Create and run the QEM simplifier
        QemMeshSimplifier simplifier;
        simplifier.setConfig(qemConfig);
        simplifier.setGpuSdfEvaluator(sdfCallback);
        simplifier.setGpuSdfGradientEvaluator(gradientCallback);

        // Simplify the mesh in-place
        std::size_t const collapsedEdges =
          simplifier.simplify(m_mesh.positions, m_mesh.normals, m_mesh.indices);

        std::size_t const finalTriangles = m_mesh.indices.size() / 3U;
        std::size_t const finalVertices = m_mesh.positions.size();

        float const reductionPercent = 100.0F *
                                       static_cast<float>(initialTriangles - finalTriangles) /
                                       static_cast<float>(initialTriangles);

        std::cout << "QEM mesh simplification complete:" << std::endl;
        std::cout << "  Triangles: " << initialTriangles << " -> " << finalTriangles << " (removed "
                  << (initialTriangles - finalTriangles) << ", " << std::fixed
                  << std::setprecision(1) << reductionPercent << "%)" << std::endl;
        std::cout << "  Vertices: " << initialVertices << " -> " << finalVertices << " (removed "
                  << (initialVertices - finalVertices) << ")" << std::endl;
        std::cout << "  Collapsed edges: " << collapsedEdges << std::endl;
    }

    void ManifoldDualContouringGpu::improveMeshQuality()
    {
        std::size_t const triangleCount = m_mesh.indices.size() / 3U;
        if (triangleCount < 2U)
        {
            return;
        }

        MeshQualityImprover::Config config;
        config.minAngleThreshold = m_config.qualityMinAngleThreshold;
        config.maxEdgeFlipPasses = m_config.qualityImprovementPasses;
        MeshQualityImprover improver(config);

        // Measure quality before
        auto const beforeStats = improver.computeQualityStats(m_mesh.positions, m_mesh.indices);

        std::cout << "Mesh quality improvement starting..." << std::endl;
        std::cout << "  Before: min angle " << std::fixed << std::setprecision(1)
                  << beforeStats.minAngle << "°, avg min angle " << beforeStats.avgMinAngle
                  << "°, avg aspect ratio " << std::setprecision(2) << beforeStats.avgAspectRatio
                  << std::endl;

        // Run quality improvement passes with SDF projection
        std::size_t const flipsTotal = improver.improveQualityWithSdf(
          m_mesh.positions,
          m_mesh.normals,
          m_mesh.indices,
          [this](Eigen::Vector3f const & pos) { return evaluateSdf(pos); },
          [this](Eigen::Vector3f const & pos) -> Eigen::Vector3f
          {
              // Compute gradient using central differences
              float const epsilon = 0.001F;
              float const dx = evaluateSdf(pos + Eigen::Vector3f(epsilon, 0.0F, 0.0F)) -
                               evaluateSdf(pos - Eigen::Vector3f(epsilon, 0.0F, 0.0F));
              float const dy = evaluateSdf(pos + Eigen::Vector3f(0.0F, epsilon, 0.0F)) -
                               evaluateSdf(pos - Eigen::Vector3f(0.0F, epsilon, 0.0F));
              float const dz = evaluateSdf(pos + Eigen::Vector3f(0.0F, 0.0F, epsilon)) -
                               evaluateSdf(pos - Eigen::Vector3f(0.0F, 0.0F, epsilon));
              Eigen::Vector3f gradient(dx, dy, dz);
              float const len = gradient.norm();
              if (len > 1e-6F)
              {
                  return (gradient / len).eval();
              }
              return Eigen::Vector3f::Zero();
          },
          m_config.qualityImprovementPasses);

        // Measure quality after
        auto const afterStats = improver.computeQualityStats(m_mesh.positions, m_mesh.indices);

        std::cout << "  After:  min angle " << std::fixed << std::setprecision(1)
                  << afterStats.minAngle << "°, avg min angle " << afterStats.avgMinAngle
                  << "°, avg aspect ratio " << std::setprecision(2) << afterStats.avgAspectRatio
                  << std::endl;
        std::cout << "  Edge flips: " << flipsTotal << std::endl;
    }

    std::vector<float> ManifoldDualContouringGpu::evaluateSdfBatchGpu(
      std::vector<Eigen::Vector3f> const & positions) const
    {
        std::vector<float> results(positions.size(), 0.0F);

        if (positions.empty())
        {
            return results;
        }

        // For now, use CPU evaluation via the cached SDF grid
        // TODO: Add GPU kernel from sdf_mesh_simplification.cl for batch evaluation
        for (std::size_t i = 0; i < positions.size(); ++i)
        {
            results[i] = evaluateSdf(positions[i]);
        }

        return results;
    }

    std::vector<Eigen::Vector3f> ManifoldDualContouringGpu::evaluateSdfGradientBatchGpu(
      std::vector<Eigen::Vector3f> const & positions) const
    {
        std::vector<Eigen::Vector3f> results(positions.size(), Eigen::Vector3f::Zero());

        if (positions.empty())
        {
            return results;
        }

        // Compute gradient using central differences
        // Use a small epsilon relative to the mesh scale
        float const epsilon = 0.001F; // 1 micron for mm-scale models

        for (std::size_t i = 0; i < positions.size(); ++i)
        {
            Eigen::Vector3f const & pos = positions[i];

            float const dx_pos = evaluateSdf(pos + Eigen::Vector3f(epsilon, 0.0F, 0.0F));
            float const dx_neg = evaluateSdf(pos - Eigen::Vector3f(epsilon, 0.0F, 0.0F));
            float const dy_pos = evaluateSdf(pos + Eigen::Vector3f(0.0F, epsilon, 0.0F));
            float const dy_neg = evaluateSdf(pos - Eigen::Vector3f(0.0F, epsilon, 0.0F));
            float const dz_pos = evaluateSdf(pos + Eigen::Vector3f(0.0F, 0.0F, epsilon));
            float const dz_neg = evaluateSdf(pos - Eigen::Vector3f(0.0F, 0.0F, epsilon));

            Eigen::Vector3f gradient((dx_pos - dx_neg) / (2.0F * epsilon),
                                     (dy_pos - dy_neg) / (2.0F * epsilon),
                                     (dz_pos - dz_neg) / (2.0F * epsilon));

            float const gradLen = gradient.norm();
            if (gradLen > 1e-6F)
            {
                gradient /= gradLen;
            }

            results[i] = gradient;
        }

        return results;
    }

    // ============================================================================
    // Chunked Processing for Large Models with Fine Features
    // ============================================================================

    std::size_t ManifoldDualContouringGpu::calculateRequiredDepth(float bboxExtent,
                                                                  float minFeatureSize) const
    {
        if (minFeatureSize <= 0.0F || bboxExtent <= 0.0F)
        {
            return m_config.maxDepth;
        }

        // Cell size at depth d = bboxExtent / 2^d
        // We need cell size <= minFeatureSize
        // So 2^d >= bboxExtent / minFeatureSize
        // d >= log2(bboxExtent / minFeatureSize)
        float const ratio = bboxExtent / minFeatureSize;
        auto const requiredDepth = static_cast<std::size_t>(std::ceil(std::log2(ratio)));
        return std::max(requiredDepth, std::size_t{1U});
    }

    std::size_t ManifoldDualContouringGpu::calculateChunkDivisor() const
    {
        if (m_config.minFeatureSize <= 0.0F || !m_cachedBoundingBox.has_value())
        {
            return 1U; // No chunking needed
        }

        Eigen::Vector3f const bboxSize = m_cachedBboxMax - m_cachedBboxMin;
        float const maxExtent = bboxSize.maxCoeff();

        std::size_t const requiredDepth =
          calculateRequiredDepth(maxExtent, m_config.minFeatureSize);

        if (requiredDepth <= m_config.maxDepth)
        {
            return 1U; // Can handle with single octree
        }

        // Number of subdivisions needed: 2^(requiredDepth - maxDepth)
        std::size_t const depthDiff = requiredDepth - m_config.maxDepth;
        return std::size_t{1U} << depthDiff;
    }

    std::vector<ManifoldDualContouringGpu::ChunkInfo>
    ManifoldDualContouringGpu::generateChunkGrid() const
    {
        std::vector<ChunkInfo> chunks;

        if (!m_cachedBoundingBox.has_value())
        {
            return chunks;
        }

        std::size_t const divisor = calculateChunkDivisor();

        // Global grid approach: all chunks share the same voxel grid
        // Total voxels across full bbox = divisor * 2^maxDepth
        std::size_t const cellsPerChunk = std::size_t{1U} << m_config.maxDepth;
        std::size_t const totalCells = divisor * cellsPerChunk;

        // Global voxel size - same for all chunks
        Eigen::Vector3f const globalVoxelSize = m_cachedBboxSize / static_cast<float>(totalCells);

        // Chunk size in world units (aligned to global grid)
        Eigen::Vector3f const chunkSize = globalVoxelSize * static_cast<float>(cellsPerChunk);

        // For DC, we need 2 extra cells at each internal boundary:
        // - Cell at boundary edge needs neighbor at +1 offset for quad generation
        // - So the cell at boundary (at +1 padding) needs its neighbor (at +2 padding)
        // The boundary cells will be duplicated in adjacent chunks, but clipping removes duplicates
        Eigen::Vector3f const boundaryPadding = globalVoxelSize * 2.0F;

        chunks.reserve(divisor * divisor * divisor);

        for (std::size_t iz = 0U; iz < divisor; ++iz)
        {
            for (std::size_t iy = 0U; iy < divisor; ++iy)
            {
                for (std::size_t ix = 0U; ix < divisor; ++ix)
                {
                    ChunkInfo chunk;
                    chunk.indexX = ix;
                    chunk.indexY = iy;
                    chunk.indexZ = iz;

                    // Core region: exactly aligned chunk (no overlap)
                    chunk.coreMin =
                      m_cachedBboxMin + Eigen::Vector3f(static_cast<float>(ix) * chunkSize.x(),
                                                        static_cast<float>(iy) * chunkSize.y(),
                                                        static_cast<float>(iz) * chunkSize.z());
                    chunk.coreMax = chunk.coreMin + chunkSize;

                    // Processing region: add 1-cell padding at internal boundaries
                    // This allows DC to generate quads at boundaries
                    chunk.min = chunk.coreMin;
                    chunk.max = chunk.coreMax;

                    // Extend by 1 voxel at internal boundaries (not at global bbox edges)
                    if (ix > 0U)
                    {
                        chunk.min.x() -= boundaryPadding.x();
                    }
                    if (iy > 0U)
                    {
                        chunk.min.y() -= boundaryPadding.y();
                    }
                    if (iz > 0U)
                    {
                        chunk.min.z() -= boundaryPadding.z();
                    }
                    if (ix < divisor - 1U)
                    {
                        chunk.max.x() += boundaryPadding.x();
                    }
                    if (iy < divisor - 1U)
                    {
                        chunk.max.y() += boundaryPadding.y();
                    }
                    if (iz < divisor - 1U)
                    {
                        chunk.max.z() += boundaryPadding.z();
                    }

                    chunks.push_back(chunk);
                }
            }
        }

        return chunks;
    }

    bool ManifoldDualContouringGpu::isChunkNonEmpty(ChunkInfo const & chunk) const
    {
        // For now, assume all chunks are potentially non-empty.
        // The octree construction will quickly determine if a chunk is empty
        // (no surface crossings = no output nodes), so the overhead is minimal.
        //
        // A more sophisticated approach would be to use GPU-based SDF sampling,
        // but that requires additional infrastructure. For typical TPMS structures,
        // most chunks will contain surface anyway.
        (void) chunk;
        return true;
    }

    void ManifoldDualContouringGpu::generateMeshForChunk(ChunkInfo const & chunk,
                                                         ManifoldDualContouringMesh & chunkMesh)
    {
        chunkMesh.positions.clear();
        chunkMesh.normals.clear();
        chunkMesh.indices.clear();

        // Clear the member mesh before generating for this chunk
        m_mesh.positions.clear();
        m_mesh.normals.clear();
        m_mesh.indices.clear();

        auto primitives = m_core.getPrimitives();
        if (!primitives)
        {
            std::cerr << "  Chunk [" << chunk.indexX << "," << chunk.indexY << "," << chunk.indexZ
                      << "]: No primitives available" << std::endl;
            return;
        }

        // Temporarily override cached bbox for this chunk
        Eigen::Vector3f const savedBboxMin = m_cachedBboxMin;
        Eigen::Vector3f const savedBboxMax = m_cachedBboxMax;
        Eigen::Vector3f const savedBboxSize = m_cachedBboxSize;

        m_cachedBboxMin = chunk.min;
        m_cachedBboxMax = chunk.max;
        m_cachedBboxSize = chunk.max - chunk.min;

        // Build octree for this chunk
        m_octreeDepth = static_cast<std::uint32_t>(m_config.maxDepth);
        m_gridResolution = 1U << m_octreeDepth;

        try
        {
            m_program->constructOctree(m_octreeBuffer,
                                       m_octreeNodeCount,
                                       m_cachedBboxMin,
                                       m_cachedBboxMax,
                                       static_cast<std::uint32_t>(m_config.initialDepth),
                                       static_cast<std::uint32_t>(m_config.maxDepth),
                                       *primitives,
                                       m_config.isoValue);

            if (m_octreeNodeCount > 0)
            {
                refreshCpuOctreeCache();
                generateVertices();
                generateIndices();
            }
        }
        catch (std::exception const & e)
        {
            std::cerr << "Error processing chunk [" << chunk.indexX << "," << chunk.indexY << ","
                      << chunk.indexZ << "]: " << e.what() << std::endl;
        }

        // Copy results to chunk mesh
        chunkMesh = m_mesh;

        // Restore original bbox
        m_cachedBboxMin = savedBboxMin;
        m_cachedBboxMax = savedBboxMax;
        m_cachedBboxSize = savedBboxSize;
    }

    void ManifoldDualContouringGpu::mergeMeshes(ManifoldDualContouringMesh & target,
                                                ManifoldDualContouringMesh const & source)
    {
        if (source.positions.empty())
        {
            return;
        }

        std::uint32_t const vertexOffset = static_cast<std::uint32_t>(target.positions.size());

        // Append vertices
        target.positions.insert(
          target.positions.end(), source.positions.begin(), source.positions.end());
        target.normals.insert(target.normals.end(), source.normals.begin(), source.normals.end());

        // Append indices with offset
        target.indices.reserve(target.indices.size() + source.indices.size());
        for (std::uint32_t idx : source.indices)
        {
            target.indices.push_back(idx + vertexOffset);
        }
    }

    void ManifoldDualContouringGpu::clipMeshToCore(ManifoldDualContouringMesh & mesh,
                                                   ChunkInfo const & chunk)
    {
        if (mesh.indices.empty())
        {
            return;
        }

        // Use centroid-based triangle ownership:
        // Keep a triangle if its centroid is within the core region.
        // This ensures each triangle is generated by exactly one chunk.
        Eigen::Vector3f const & coreMin = chunk.coreMin;
        Eigen::Vector3f const & coreMax = chunk.coreMax;

        auto isCentroidInCore = [&coreMin, &coreMax](Eigen::Vector3f const & p0,
                                                     Eigen::Vector3f const & p1,
                                                     Eigen::Vector3f const & p2) -> bool
        {
            Eigen::Vector3f const centroid = (p0 + p1 + p2) / 3.0F;
            return centroid.x() >= coreMin.x() && centroid.x() < coreMax.x() &&
                   centroid.y() >= coreMin.y() && centroid.y() < coreMax.y() &&
                   centroid.z() >= coreMin.z() && centroid.z() < coreMax.z();
        };

        // Find triangles to keep (centroid inside core)
        std::vector<std::uint32_t> newIndices;
        newIndices.reserve(mesh.indices.size());

        std::vector<bool> vertexUsed(mesh.positions.size(), false);

        for (std::size_t t = 0U; t < mesh.indices.size(); t += 3U)
        {
            std::uint32_t const i0 = mesh.indices[t + 0U];
            std::uint32_t const i1 = mesh.indices[t + 1U];
            std::uint32_t const i2 = mesh.indices[t + 2U];

            // Keep triangle if centroid is inside core region
            if (isCentroidInCore(mesh.positions[i0], mesh.positions[i1], mesh.positions[i2]))
            {
                newIndices.push_back(i0);
                newIndices.push_back(i1);
                newIndices.push_back(i2);
                vertexUsed[i0] = true;
                vertexUsed[i1] = true;
                vertexUsed[i2] = true;
            }
        }

        // Build vertex compaction map
        std::vector<std::uint32_t> compactMap(mesh.positions.size(), 0U);
        std::uint32_t newCount = 0U;
        for (std::size_t i = 0U; i < mesh.positions.size(); ++i)
        {
            if (vertexUsed[i])
            {
                compactMap[i] = newCount++;
            }
        }

        // Compact vertices
        std::vector<Eigen::Vector3f> newPositions(newCount);
        std::vector<Eigen::Vector3f> newNormals(newCount);
        for (std::size_t i = 0U; i < mesh.positions.size(); ++i)
        {
            if (vertexUsed[i])
            {
                newPositions[compactMap[i]] = mesh.positions[i];
                newNormals[compactMap[i]] = mesh.normals[i];
            }
        }

        // Remap indices
        for (auto & idx : newIndices)
        {
            idx = compactMap[idx];
        }

        mesh.positions = std::move(newPositions);
        mesh.normals = std::move(newNormals);
        mesh.indices = std::move(newIndices);
    }

    void ManifoldDualContouringGpu::weldBoundaryVertices(float tolerance)
    {
        if (m_mesh.positions.size() < 2U)
        {
            return;
        }

        float const toleranceSq = tolerance * tolerance;
        std::size_t const numVertices = m_mesh.positions.size();

        // Build a simple spatial hash for faster neighbor lookup
        float const cellSize = tolerance * 2.0F;
        float const invCellSize = 1.0F / cellSize;

        auto hashPos = [invCellSize](Eigen::Vector3f const & pos) -> std::uint64_t
        {
            auto const ix = static_cast<std::int32_t>(std::floor(pos.x() * invCellSize));
            auto const iy = static_cast<std::int32_t>(std::floor(pos.y() * invCellSize));
            auto const iz = static_cast<std::int32_t>(std::floor(pos.z() * invCellSize));

            // Simple hash combining
            std::uint64_t const hx = static_cast<std::uint64_t>(ix) & 0x1FFFFF;
            std::uint64_t const hy = static_cast<std::uint64_t>(iy) & 0x1FFFFF;
            std::uint64_t const hz = static_cast<std::uint64_t>(iz) & 0x1FFFFF;
            return (hx << 42) | (hy << 21) | hz;
        };

        // Map from cell hash to vertex indices in that cell
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> spatialHash;
        for (std::uint32_t i = 0U; i < numVertices; ++i)
        {
            std::uint64_t const hash = hashPos(m_mesh.positions[i]);
            spatialHash[hash].push_back(i);
        }

        // Vertex remapping: vertexRemap[old] = new (canonical vertex)
        std::vector<std::uint32_t> vertexRemap(numVertices);
        std::iota(vertexRemap.begin(), vertexRemap.end(), 0U);

        // For each vertex, find nearby vertices and potentially merge
        for (std::uint32_t i = 0U; i < numVertices; ++i)
        {
            if (vertexRemap[i] != i)
            {
                continue; // Already remapped
            }

            Eigen::Vector3f const & pos = m_mesh.positions[i];

            // Check neighboring cells
            auto const ix = static_cast<std::int32_t>(std::floor(pos.x() * invCellSize));
            auto const iy = static_cast<std::int32_t>(std::floor(pos.y() * invCellSize));
            auto const iz = static_cast<std::int32_t>(std::floor(pos.z() * invCellSize));

            for (int dz = -1; dz <= 1; ++dz)
            {
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        std::uint64_t const hx = static_cast<std::uint64_t>(ix + dx) & 0x1FFFFF;
                        std::uint64_t const hy = static_cast<std::uint64_t>(iy + dy) & 0x1FFFFF;
                        std::uint64_t const hz = static_cast<std::uint64_t>(iz + dz) & 0x1FFFFF;
                        std::uint64_t const neighborHash = (hx << 42) | (hy << 21) | hz;

                        auto it = spatialHash.find(neighborHash);
                        if (it == spatialHash.end())
                        {
                            continue;
                        }

                        for (std::uint32_t j : it->second)
                        {
                            if (j <= i || vertexRemap[j] != j)
                            {
                                continue; // Skip self, already processed, or already remapped
                            }

                            float const distSq = (m_mesh.positions[j] - pos).squaredNorm();
                            if (distSq < toleranceSq)
                            {
                                // Merge j into i
                                vertexRemap[j] = i;
                            }
                        }
                    }
                }
            }
        }

        // Count how many vertices survive and build compaction map
        std::vector<std::uint32_t> compactMap(numVertices,
                                              std::numeric_limits<std::uint32_t>::max());
        std::uint32_t newVertexCount = 0U;

        for (std::uint32_t i = 0U; i < numVertices; ++i)
        {
            if (vertexRemap[i] == i)
            {
                compactMap[i] = newVertexCount++;
            }
        }

        // Build final remap: old index -> new compacted index
        std::vector<std::uint32_t> finalRemap(numVertices);
        for (std::uint32_t i = 0U; i < numVertices; ++i)
        {
            std::uint32_t canonical = vertexRemap[i];
            finalRemap[i] = compactMap[canonical];
        }

        // Compact vertex buffer
        std::vector<Eigen::Vector3f> newPositions(newVertexCount);
        std::vector<Eigen::Vector3f> newNormals(newVertexCount);

        for (std::uint32_t i = 0U; i < numVertices; ++i)
        {
            if (vertexRemap[i] == i)
            {
                std::uint32_t const newIdx = compactMap[i];
                newPositions[newIdx] = m_mesh.positions[i];
                newNormals[newIdx] = m_mesh.normals[i];
            }
        }

        // Remap indices
        for (auto & idx : m_mesh.indices)
        {
            idx = finalRemap[idx];
        }

        // Remove degenerate triangles
        std::vector<std::uint32_t> validIndices;
        validIndices.reserve(m_mesh.indices.size());

        for (std::size_t t = 0U; t < m_mesh.indices.size(); t += 3U)
        {
            std::uint32_t const i0 = m_mesh.indices[t + 0U];
            std::uint32_t const i1 = m_mesh.indices[t + 1U];
            std::uint32_t const i2 = m_mesh.indices[t + 2U];

            if (i0 != i1 && i1 != i2 && i2 != i0)
            {
                validIndices.push_back(i0);
                validIndices.push_back(i1);
                validIndices.push_back(i2);
            }
        }

        std::size_t const vertsBefore = m_mesh.positions.size();
        std::size_t const trisBefore = m_mesh.indices.size() / 3U;

        m_mesh.positions = std::move(newPositions);
        m_mesh.normals = std::move(newNormals);
        m_mesh.indices = std::move(validIndices);

        std::size_t const vertsAfter = m_mesh.positions.size();
        std::size_t const trisAfter = m_mesh.indices.size() / 3U;

        std::cout << "  Vertex welding: " << vertsBefore << " -> " << vertsAfter << " vertices, "
                  << trisBefore << " -> " << trisAfter << " triangles" << std::endl;
    }

    void ManifoldDualContouringGpu::fillBoundaryGaps(float searchRadius)
    {
        if (m_mesh.indices.size() < 3U)
        {
            return;
        }

        bool const debugEnabled = (std::getenv("GLADIUS_DEBUG_MDC_CONFIG") != nullptr);

        // ----------------------------------------------------------------
        // Pre-pass: weld near-coincident boundary vertices (index remap only)
        //
        // The non-hierarchical single-pass path can still produce tiny cracks
        // where two vertices should be identical but differ slightly.
        // Those cracks show up as degree-1 chains instead of clean loops,
        // which makes loop-based capping unreliable.
        //
        // We do a very conservative weld ONLY on vertices that participate in
        // boundary edges, and we only remap indices (no vertex buffer compaction).
        // This keeps the operation cheap and avoids accidentally merging interior
        // vertices.
        // ----------------------------------------------------------------
        auto weldBoundaryVerticesByRemap = [&](std::vector<std::uint32_t> const & boundaryVertexList)
        {
            if (boundaryVertexList.size() < 2U)
            {
                return;
            }

            // Use a *very* small fraction of the search radius as weld tolerance.
            // This is meant to collapse numerical jitter, not to pull geometry together.
            float const tolerance = std::max(1e-6F, searchRadius * 0.05F);
            float const toleranceSq = tolerance * tolerance;

            // Simple union-find over the (small) boundary-vertex set.
            struct DisjointSet
            {
                std::vector<std::uint32_t> parent;
                explicit DisjointSet(std::size_t n)
                    : parent(n)
                {
                    std::iota(parent.begin(), parent.end(), 0U);
                }
                std::uint32_t find(std::uint32_t x)
                {
                    while (parent[x] != x)
                    {
                        parent[x] = parent[parent[x]];
                        x = parent[x];
                    }
                    return x;
                }
                void unite(std::uint32_t a, std::uint32_t b)
                {
                    a = find(a);
                    b = find(b);
                    if (a != b)
                    {
                        parent[b] = a;
                    }
                }
            };

            DisjointSet dsu(boundaryVertexList.size());

            for (std::size_t i = 0U; i < boundaryVertexList.size(); ++i)
            {
                std::uint32_t const vi = boundaryVertexList[i];
                Eigen::Vector3f const & pi = m_mesh.positions[vi];
                for (std::size_t j = i + 1U; j < boundaryVertexList.size(); ++j)
                {
                    std::uint32_t const vj = boundaryVertexList[j];
                    Eigen::Vector3f const & pj = m_mesh.positions[vj];
                    if ((pj - pi).squaredNorm() < toleranceSq)
                    {
                        dsu.unite(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j));
                    }
                }
            }

            // Map from original vertex index -> representative vertex index.
            std::unordered_map<std::uint32_t, std::uint32_t> remap;
            remap.reserve(boundaryVertexList.size());

            // Choose the lowest original vertex index in each set as representative.
            std::unordered_map<std::uint32_t, std::uint32_t> repToMinVertex;
            repToMinVertex.reserve(boundaryVertexList.size());

            for (std::size_t i = 0U; i < boundaryVertexList.size(); ++i)
            {
                std::uint32_t const rep = dsu.find(static_cast<std::uint32_t>(i));
                std::uint32_t const v = boundaryVertexList[i];
                auto it = repToMinVertex.find(rep);
                if (it == repToMinVertex.end())
                {
                    repToMinVertex.emplace(rep, v);
                }
                else
                {
                    it->second = std::min(it->second, v);
                }
            }

            std::size_t merges = 0U;
            for (std::size_t i = 0U; i < boundaryVertexList.size(); ++i)
            {
                std::uint32_t const rep = dsu.find(static_cast<std::uint32_t>(i));
                std::uint32_t const v = boundaryVertexList[i];
                std::uint32_t const canonical = repToMinVertex[rep];
                remap.emplace(v, canonical);
                if (canonical != v)
                {
                    ++merges;
                }
            }

            if (merges == 0U)
            {
                return;
            }

            // Remap indices. This may create degenerate triangles; we cull them.
            std::vector<std::uint32_t> newIndices;
            newIndices.reserve(m_mesh.indices.size());
            for (std::size_t t = 0U; t < m_mesh.indices.size(); t += 3U)
            {
                std::uint32_t i0 = m_mesh.indices[t + 0U];
                std::uint32_t i1 = m_mesh.indices[t + 1U];
                std::uint32_t i2 = m_mesh.indices[t + 2U];

                auto r0 = remap.find(i0);
                auto r1 = remap.find(i1);
                auto r2 = remap.find(i2);
                if (r0 != remap.end())
                {
                    i0 = r0->second;
                }
                if (r1 != remap.end())
                {
                    i1 = r1->second;
                }
                if (r2 != remap.end())
                {
                    i2 = r2->second;
                }

                if (i0 == i1 || i1 == i2 || i2 == i0)
                {
                    continue;
                }

                newIndices.push_back(i0);
                newIndices.push_back(i1);
                newIndices.push_back(i2);
            }
            m_mesh.indices = std::move(newIndices);

            if (debugEnabled)
            {
                std::cout << "  Gap filling: Boundary vertex weld remapped " << merges
                          << " indices (tolerance=" << tolerance << ")" << std::endl;
            }
        };

        // Build edge-to-triangle map to find boundary edges
        // An edge is a boundary edge if it's used by only 1 triangle
        // IMPORTANT: We store DIRECTED edges to preserve winding order
        struct DirectedEdge
        {
            std::uint32_t v0; // Start vertex (as in original triangle winding)
            std::uint32_t v1; // End vertex (as in original triangle winding)
        };

        // For counting, we use undirected edge representation
        struct UndirectedEdgeHash
        {
            std::size_t operator()(DirectedEdge const & e) const
            {
                std::uint32_t minV = std::min(e.v0, e.v1);
                std::uint32_t maxV = std::max(e.v0, e.v1);
                return std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(minV) << 32) | maxV);
            }
        };

        struct UndirectedEdgeEqual
        {
            bool operator()(DirectedEdge const & a, DirectedEdge const & b) const
            {
                std::uint32_t minA = std::min(a.v0, a.v1);
                std::uint32_t maxA = std::max(a.v0, a.v1);
                std::uint32_t minB = std::min(b.v0, b.v1);
                std::uint32_t maxB = std::max(b.v0, b.v1);
                return minA == minB && maxA == maxB;
            }
        };

        // Store edge usage count and the DIRECTED edge from the original triangle
        // Also store the third vertex of the triangle for normal computation
        struct EdgeInfo
        {
            std::size_t count = 0U;
            DirectedEdge directedEdge; // Direction from the first triangle using this edge
            std::uint32_t thirdVertex; // The third vertex of the triangle (for normal)
        };

        std::unordered_map<DirectedEdge, EdgeInfo, UndirectedEdgeHash, UndirectedEdgeEqual> edgeInfo;

        auto buildEdgeInfo = [&]()
        {
            edgeInfo.clear();

            for (std::size_t t = 0U; t < m_mesh.indices.size(); t += 3U)
            {
                std::uint32_t const i0 = m_mesh.indices[t + 0U];
                std::uint32_t const i1 = m_mesh.indices[t + 1U];
                std::uint32_t const i2 = m_mesh.indices[t + 2U];

                // Store directed edges as they appear in the triangle (preserves winding)
                // Also store the opposite vertex for each edge (for normal computation)
                std::tuple<DirectedEdge, std::uint32_t> const edges[3] = {
                  {{i0, i1}, i2}, {{i1, i2}, i0}, {{i2, i0}, i1}};
                for (auto const & [e, opposite] : edges)
                {
                    auto & info = edgeInfo[e];
                    if (info.count == 0U)
                    {
                        info.directedEdge = e; // Store the direction from first occurrence
                        info.thirdVertex = opposite;
                    }
                    info.count++;
                }
            }
        };

        // Build edge info, then do a conservative boundary-vertex weld and rebuild once.
        buildEdgeInfo();

        // Classify "bbox boundary" edges.
        //
        // Default: use the *padded domain bbox* used for octree construction/SDF evaluation.
        // This avoids misclassifying holes near the shape's AABB as "bbox boundary".
        //
        // Special case (auto-detected): for models where the SDF intentionally contains a
        // clipping box (e.g. ImplicitGyroid clipped by a bbox via max()), the relevant
        // boundary loops lie on the *original* (un-padded) model bbox faces. In that case
        // routing those edges through the bbox-face capping path produces better results.
        Eigen::Vector3f const paddedBboxMin = m_cachedBboxMin;
        Eigen::Vector3f const paddedBboxMax = m_cachedBboxMax;

        Eigen::Vector3f chosenBboxMin = paddedBboxMin;
        Eigen::Vector3f chosenBboxMax = paddedBboxMax;

        Eigen::Vector3f originalBboxMin = paddedBboxMin;
        Eigen::Vector3f originalBboxMax = paddedBboxMax;
        bool hasOriginalBbox = false;
        if (m_cachedBoundingBox.has_value())
        {
            originalBboxMin = Eigen::Vector3f(m_cachedBoundingBox->min.s[0],
                                              m_cachedBoundingBox->min.s[1],
                                              m_cachedBoundingBox->min.s[2]);
            originalBboxMax = Eigen::Vector3f(m_cachedBoundingBox->max.s[0],
                                              m_cachedBoundingBox->max.s[1],
                                              m_cachedBoundingBox->max.s[2]);
            hasOriginalBbox = true;
        }

        // Use a tolerance derived from the voxel size at the extraction depth.
        // Using searchRadius here is unsafe because it can be larger than the padding margin
        // (2 voxels), which would incorrectly mark many interior edges as "bbox boundary".
        Eigen::Vector3f const domainSize = paddedBboxMax - paddedBboxMin;
        float const maxExtent = domainSize.maxCoeff();
        float const voxelSize = maxExtent / static_cast<float>(1U << m_config.maxDepth);
        // Tight tolerance: we only want to classify edges that are *really* on a clipping plane.
        // A too-large tolerance will misclassify near-boundary interior cracks as bbox-boundary
        // and can lead to bad capping/stitching artifacts.
        float const bboxTolerance = voxelSize * 0.1F;

        auto faceMask = [bboxTolerance](Eigen::Vector3f const & p,
                                        Eigen::Vector3f const & bMin,
                                        Eigen::Vector3f const & bMax) -> std::uint8_t
        {
            std::uint8_t mask = 0U;
            if (p.x() <= bMin.x() + bboxTolerance)
            {
                mask |= 1U << 0U;
            }
            if (p.x() >= bMax.x() - bboxTolerance)
            {
                mask |= 1U << 1U;
            }
            if (p.y() <= bMin.y() + bboxTolerance)
            {
                mask |= 1U << 2U;
            }
            if (p.y() >= bMax.y() - bboxTolerance)
            {
                mask |= 1U << 3U;
            }
            if (p.z() <= bMin.z() + bboxTolerance)
            {
                mask |= 1U << 4U;
            }
            if (p.z() >= bMax.z() - bboxTolerance)
            {
                mask |= 1U << 5U;
            }
            return mask;
        };

        auto isOnSameBboxFace = [&faceMask](Eigen::Vector3f const & p0,
                                            Eigen::Vector3f const & p1,
                                            Eigen::Vector3f const & bMin,
                                            Eigen::Vector3f const & bMax) -> bool
        {
            std::uint8_t const m0 = faceMask(p0, bMin, bMax);
            std::uint8_t const m1 = faceMask(p1, bMin, bMax);
            auto isSingleFace = [](std::uint8_t mask) -> bool
            {
                return mask != 0U && (mask & static_cast<std::uint8_t>(mask - 1U)) == 0U;
            };

            // Only accept edges where both endpoints clearly belong to exactly one face,
            // and it is the *same* face. Points on bbox edges/corners are ambiguous
            // (multiple face bits set) and should be treated as internal boundaries.
            if (!isSingleFace(m0) || !isSingleFace(m1))
            {
                return false;
            }
            return m0 == m1;
        };

        auto countBboxLikeBoundaryEdges = [&](Eigen::Vector3f const & bMin,
                                              Eigen::Vector3f const & bMax) -> std::size_t
        {
            std::size_t count = 0U;
            for (auto const & [edge, info] : edgeInfo)
            {
                if (info.count != 1U)
                {
                    continue;
                }
                Eigen::Vector3f const & p0 = m_mesh.positions[info.directedEdge.v0];
                Eigen::Vector3f const & p1 = m_mesh.positions[info.directedEdge.v1];
                if (isOnSameBboxFace(p0, p1, bMin, bMax))
                {
                    ++count;
                }
            }
            return count;
        };

        // Compute the AABB of boundary vertices (edges with usage==1). This often matches
        // the clipping volume for SDFs built via max()/min() against an axis-aligned box.
        Eigen::Vector3f boundaryAabbMin = Eigen::Vector3f::Constant(std::numeric_limits<float>::infinity());
        Eigen::Vector3f boundaryAabbMax = Eigen::Vector3f::Constant(-std::numeric_limits<float>::infinity());
        std::size_t boundaryEdgeCount = 0U;
        for (auto const & [edge, info] : edgeInfo)
        {
            if (info.count != 1U)
            {
                continue;
            }
            ++boundaryEdgeCount;
            Eigen::Vector3f const & p0 = m_mesh.positions[info.directedEdge.v0];
            Eigen::Vector3f const & p1 = m_mesh.positions[info.directedEdge.v1];
            boundaryAabbMin = boundaryAabbMin.cwiseMin(p0);
            boundaryAabbMin = boundaryAabbMin.cwiseMin(p1);
            boundaryAabbMax = boundaryAabbMax.cwiseMax(p0);
            boundaryAabbMax = boundaryAabbMax.cwiseMax(p1);
        }

        // Auto-select bbox for face classification.
        if (hasOriginalBbox)
        {
            std::size_t const paddedFaceEdges = countBboxLikeBoundaryEdges(paddedBboxMin, paddedBboxMax);
            std::size_t const originalFaceEdges = countBboxLikeBoundaryEdges(originalBboxMin, originalBboxMax);

            std::size_t boundaryFaceEdges = 0U;
            if (boundaryEdgeCount > 0U)
            {
                boundaryFaceEdges = countBboxLikeBoundaryEdges(boundaryAabbMin, boundaryAabbMax);
            }

            // If we see no face edges on the padded domain, but a significant number on the
            // original bbox, assume an internal clipping boundary and use the original bbox.
            // Prefer the boundary-vertex AABB when it indicates a strong axis-aligned clipping boundary.
            if (paddedFaceEdges == 0U && boundaryFaceEdges > 1024U)
            {
                chosenBboxMin = boundaryAabbMin;
                chosenBboxMax = boundaryAabbMax;
                if (debugEnabled)
                {
                    std::cout << "  Gap filling: Using BOUNDARY-VERTEX AABB for face classification (boundaryFaceEdges="
                              << boundaryFaceEdges << ", paddedFaceEdges=" << paddedFaceEdges << ")" << std::endl;
                }
            }
            else if (paddedFaceEdges == 0U && originalFaceEdges > 1024U)
            {
                chosenBboxMin = originalBboxMin;
                chosenBboxMax = originalBboxMax;
                if (debugEnabled)
                {
                    std::cout << "  Gap filling: Using ORIGINAL bbox for face classification (originalFaceEdges="
                              << originalFaceEdges << ", paddedFaceEdges=" << paddedFaceEdges << ")" << std::endl;
                }
            }
        }

        // Collect boundary edges (used by only 1 triangle) with their original direction
        // Separate internal edges from bbox boundary edges for different handling
        struct BoundaryEdge
        {
            DirectedEdge edge;
            std::uint32_t thirdVertex; // Third vertex of original triangle (for normal)
        };
        std::vector<BoundaryEdge> boundaryEdges;     // Internal boundary edges
        std::vector<BoundaryEdge> bboxBoundaryEdges; // Edges on bbox faces (need capping)

        auto collectBoundaryEdges = [&]()
        {
            boundaryEdges.clear();
            bboxBoundaryEdges.clear();

            for (auto const & [edge, info] : edgeInfo)
            {
                if (info.count == 1U)
                {
                    // Check if both vertices are on the same bbox face
                    Eigen::Vector3f const & p0 = m_mesh.positions[info.directedEdge.v0];
                    Eigen::Vector3f const & p1 = m_mesh.positions[info.directedEdge.v1];

                    if (isOnSameBboxFace(p0, p1, chosenBboxMin, chosenBboxMax))
                    {
                        // Collect bbox boundary edges for capping (CSG intersection boundaries)
                        bboxBoundaryEdges.push_back({info.directedEdge, info.thirdVertex});
                    }
                    else
                    {
                        boundaryEdges.push_back({info.directedEdge, info.thirdVertex});
                    }
                }
            }
        };

        collectBoundaryEdges();

        // Weld boundary vertices (internal and bbox) once and rebuild edge info/boundary lists.
        // This helps convert small chains into clean loops.
        {
            std::unordered_set<std::uint32_t> boundaryVertexSet;
            boundaryVertexSet.reserve((boundaryEdges.size() + bboxBoundaryEdges.size()) * 2U);
            for (BoundaryEdge const & be : boundaryEdges)
            {
                boundaryVertexSet.insert(be.edge.v0);
                boundaryVertexSet.insert(be.edge.v1);
            }
            for (BoundaryEdge const & be : bboxBoundaryEdges)
            {
                boundaryVertexSet.insert(be.edge.v0);
                boundaryVertexSet.insert(be.edge.v1);
            }

            std::vector<std::uint32_t> boundaryVertexList;
            boundaryVertexList.reserve(boundaryVertexSet.size());
            for (std::uint32_t v : boundaryVertexSet)
            {
                boundaryVertexList.push_back(v);
            }
            weldBoundaryVerticesByRemap(boundaryVertexList);

            // Rebuild edge info and boundary edges after remap.
            buildEdgeInfo();
            collectBoundaryEdges();
        }

        if (boundaryEdges.empty() && bboxBoundaryEdges.empty())
        {
            std::cout << "  Gap filling: No boundary edges found - mesh is closed" << std::endl;
            return;
        }

        // Heuristic: bbox-face capping/stitching works best for relatively simple boundaries.
        // For very large boundary sets (often produced by complex clipped implicit surfaces),
        // it can introduce a large amount of duplicate geometry and non-manifold edges.
        // In those cases, treat bbox-boundary edges as internal boundaries and use the
        // loop-based internal capping path instead.
        if (bboxBoundaryEdges.size() > 1024U)
        {
            if (debugEnabled)
            {
                std::cout << "  Gap filling: Treating " << bboxBoundaryEdges.size()
                          << " bbox-boundary edges as internal due to complexity" << std::endl;
            }
            boundaryEdges.insert(boundaryEdges.end(), bboxBoundaryEdges.begin(), bboxBoundaryEdges.end());
            bboxBoundaryEdges.clear();
        }

        std::cout << "  Gap filling: Found " << boundaryEdges.size() << " internal boundary edges"
                  << ", " << bboxBoundaryEdges.size() << " on bbox boundary (will cap)"
                  << std::endl;

        // ----------------------------------------------------------------
        // Phase 1: Cap internal boundary loops (non-bbox holes)
        //
        // The previous implementation tried to stitch boundary edges in pairs based on proximity.
        // That can leave holes and/or introduce non-manifold edges. Here we instead try to trace
        // boundary loops and cap each loop with a simple centroid fan.
        // ----------------------------------------------------------------
        if (!boundaryEdges.empty())
        {
            // Build undirected adjacency for boundary edges.
            std::unordered_map<std::uint32_t, std::vector<std::size_t>> vertexToEdges;
            vertexToEdges.reserve(boundaryEdges.size());
            for (std::size_t i = 0U; i < boundaryEdges.size(); ++i)
            {
                vertexToEdges[boundaryEdges[i].edge.v0].push_back(i);
                vertexToEdges[boundaryEdges[i].edge.v1].push_back(i);
            }

            if (debugEnabled)
            {
                std::size_t deg1 = 0U;
                std::size_t deg2 = 0U;
                std::size_t deg3p = 0U;
                for (auto const & [v, edges] : vertexToEdges)
                {
                    if (edges.size() == 1U)
                    {
                        ++deg1;
                    }
                    else if (edges.size() == 2U)
                    {
                        ++deg2;
                    }
                    else
                    {
                        ++deg3p;
                    }
                }
                std::cout << "  Gap filling: Internal boundary vertex degrees: deg1=" << deg1
                          << ", deg2=" << deg2 << ", deg>=3=" << deg3p << std::endl;
            }

            // Split into connected components (by vertices) so we can compute a local best-fit plane per component.
            std::unordered_set<std::uint32_t> visitedVertices;
            visitedVertices.reserve(vertexToEdges.size());

            std::vector<bool> edgeUsed(boundaryEdges.size(), false);
            std::vector<std::uint32_t> capTriangles;
            std::size_t loopCount = 0U;
            std::size_t cappedEdges = 0U;
            std::size_t const maxLoopSize = 5000U;
            std::size_t componentCount = 0U;

            for (auto const & [seedVertex, _] : vertexToEdges)
            {
                if (visitedVertices.count(seedVertex) != 0)
                {
                    continue;
                }

                ++componentCount;

                // Collect component vertices and edges.
                std::vector<std::uint32_t> componentVertices;
                componentVertices.reserve(64U);
                std::vector<std::uint32_t> queue;
                queue.reserve(64U);

                visitedVertices.insert(seedVertex);
                queue.push_back(seedVertex);

                std::unordered_set<std::size_t> componentEdgeIndices;
                componentEdgeIndices.reserve(128U);

                for (std::size_t q = 0U; q < queue.size(); ++q)
                {
                    std::uint32_t const v = queue[q];
                    componentVertices.push_back(v);
                    auto it = vertexToEdges.find(v);
                    if (it == vertexToEdges.end())
                    {
                        continue;
                    }
                    for (std::size_t edgeIdx : it->second)
                    {
                        componentEdgeIndices.insert(edgeIdx);
                        BoundaryEdge const & e = boundaryEdges[edgeIdx];
                        std::uint32_t const other = (e.edge.v0 == v) ? e.edge.v1 : e.edge.v0;
                        if (visitedVertices.count(other) == 0)
                        {
                            visitedVertices.insert(other);
                            queue.push_back(other);
                        }
                    }
                }

                if (componentVertices.size() < 3U || componentEdgeIndices.size() < 3U)
                {
                    continue;
                }

                // Best-fit plane via PCA.
                Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
                for (std::uint32_t v : componentVertices)
                {
                    Eigen::Vector3f const & p = m_mesh.positions[v];
                    centroid += Eigen::Vector3d(p.x(), p.y(), p.z());
                }
                centroid /= static_cast<double>(componentVertices.size());

                Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
                for (std::uint32_t v : componentVertices)
                {
                    Eigen::Vector3f const & pf = m_mesh.positions[v];
                    Eigen::Vector3d const p(pf.x(), pf.y(), pf.z());
                    Eigen::Vector3d const d = p - centroid;
                    cov += d * d.transpose();
                }
                cov /= static_cast<double>(componentVertices.size());

                Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(cov);
                Eigen::Vector3d normal = Eigen::Vector3d::UnitZ();
                if (solver.info() == Eigen::Success)
                {
                    normal = solver.eigenvectors().col(0); // smallest eigenvalue
                    if (normal.squaredNorm() > 0.0)
                    {
                        normal.normalize();
                    }
                }

                // Fall back to triangle normals if PCA is degenerate.
                if (normal.squaredNorm() < 1e-20)
                {
                    Eigen::Vector3d n = Eigen::Vector3d::Zero();
                    for (std::size_t edgeIdx : componentEdgeIndices)
                    {
                        BoundaryEdge const & be = boundaryEdges[edgeIdx];
                        Eigen::Vector3f const & p0 = m_mesh.positions[be.edge.v0];
                        Eigen::Vector3f const & p1 = m_mesh.positions[be.edge.v1];
                        Eigen::Vector3f const & p2 = m_mesh.positions[be.thirdVertex];
                        Eigen::Vector3d const nn = Eigen::Vector3d((p1 - p0).cross(p2 - p0).cast<double>());
                        if (nn.squaredNorm() > 0.0)
                        {
                            n += nn.normalized();
                        }
                    }
                    if (n.squaredNorm() > 0.0)
                    {
                        normal = n.normalized();
                    }
                }

                Eigen::Vector3d const uAxis = normal.unitOrthogonal();
                Eigen::Vector3d const vAxis = normal.cross(uAxis);

                // If the boundary graph is not a collection of pure cycles (e.g., due to small cracks
                // producing open chains), try to add short "bridge" edges between odd-degree vertices
                // to restore closure before loop extraction.
                struct SyntheticEdge
                {
                    std::uint32_t v0;
                    std::uint32_t v1;
                };
                std::vector<SyntheticEdge> syntheticEdges;
                syntheticEdges.reserve(32U);

                {
                    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> neighbors;
                    neighbors.reserve(componentVertices.size());

                    auto undirectedKey = [](std::uint32_t a, std::uint32_t b) -> std::uint64_t
                    {
                        std::uint32_t const lo = std::min(a, b);
                        std::uint32_t const hi = std::max(a, b);
                        return (static_cast<std::uint64_t>(lo) << 32U) | static_cast<std::uint64_t>(hi);
                    };

                    std::unordered_set<std::uint64_t> existingEdges;
                    existingEdges.reserve(componentEdgeIndices.size());

                    for (std::size_t edgeIdx : componentEdgeIndices)
                    {
                        BoundaryEdge const & be = boundaryEdges[edgeIdx];
                        neighbors[be.edge.v0].push_back(be.edge.v1);
                        neighbors[be.edge.v1].push_back(be.edge.v0);
                        existingEdges.insert(undirectedKey(be.edge.v0, be.edge.v1));
                    }

                    std::vector<std::uint32_t> oddVertices;
                    oddVertices.reserve(16U);
                    for (auto const & [v, ns] : neighbors)
                    {
                        if ((ns.size() % 2U) == 1U)
                        {
                            oddVertices.push_back(v);
                        }
                    }

                    // Allow bridging across small cracks. Keep this conservative to avoid
                    // introducing non-manifold geometry by connecting unrelated boundaries.
                    float avgEdgeLen = 0.0F;
                    if (!componentEdgeIndices.empty())
                    {
                        for (std::size_t edgeIdx : componentEdgeIndices)
                        {
                            BoundaryEdge const & be = boundaryEdges[edgeIdx];
                            Eigen::Vector3f const & p0 = m_mesh.positions[be.edge.v0];
                            Eigen::Vector3f const & p1 = m_mesh.positions[be.edge.v1];
                            avgEdgeLen += (p1 - p0).norm();
                        }
                        avgEdgeLen /= static_cast<float>(componentEdgeIndices.size());
                    }

                    float const maxBridgeLength = std::max(searchRadius * 2.0F, avgEdgeLen * 2.0F);
                    float const maxBridgeLengthSq = maxBridgeLength * maxBridgeLength;

                    std::size_t bridgesAdded = 0U;
                    while (oddVertices.size() >= 2U)
                    {
                        std::uint32_t const v0 = oddVertices.back();
                        oddVertices.pop_back();
                        Eigen::Vector3f const & p0 = m_mesh.positions[v0];

                        std::size_t bestIdx = std::numeric_limits<std::size_t>::max();
                        float bestDistSq = maxBridgeLengthSq;

                        for (std::size_t i = 0U; i < oddVertices.size(); ++i)
                        {
                            std::uint32_t const v1 = oddVertices[i];
                            if (existingEdges.count(undirectedKey(v0, v1)) != 0)
                            {
                                continue;
                            }

                            Eigen::Vector3f const & p1 = m_mesh.positions[v1];
                            float const distSq = (p1 - p0).squaredNorm();
                            if (distSq < bestDistSq)
                            {
                                bestDistSq = distSq;
                                bestIdx = i;
                            }
                        }

                        if (bestIdx == std::numeric_limits<std::size_t>::max())
                        {
                            break;
                        }

                        std::uint32_t const v1 = oddVertices[bestIdx];
                        oddVertices.erase(oddVertices.begin() + static_cast<std::ptrdiff_t>(bestIdx));

                        syntheticEdges.push_back({v0, v1});
                        existingEdges.insert(undirectedKey(v0, v1));
                        neighbors[v0].push_back(v1);
                        neighbors[v1].push_back(v0);
                        ++bridgesAdded;
                    }

                    if (debugEnabled && bridgesAdded > 0U)
                    {
                        std::cout << "  Gap filling: Added " << bridgesAdded << " short bridge edges to close odd-degree internal boundaries" << std::endl;
                    }
                }

                // Build half-edges for the component (two per undirected boundary edge).
                struct HalfEdge
                {
                    std::uint32_t from;
                    std::uint32_t to;
                    bool isSynthetic;
                    std::size_t idx; // boundaryEdges index if !isSynthetic, else syntheticEdges index
                };

                std::vector<HalfEdge> halfEdges;
                halfEdges.reserve(componentEdgeIndices.size() * 2U);
                std::unordered_map<std::uint32_t, std::vector<std::size_t>> outgoing;
                outgoing.reserve(componentVertices.size());

                auto halfEdgeKey = [](std::uint32_t from, std::uint32_t to) -> std::uint64_t
                {
                    return (static_cast<std::uint64_t>(from) << 32U) | static_cast<std::uint64_t>(to);
                };

                std::unordered_map<std::uint64_t, std::size_t> directedToHalfEdge;
                directedToHalfEdge.reserve(componentEdgeIndices.size() * 2U);

                for (std::size_t edgeIdx : componentEdgeIndices)
                {
                    BoundaryEdge const & be = boundaryEdges[edgeIdx];
                    std::uint32_t const a = be.edge.v0;
                    std::uint32_t const b = be.edge.v1;

                    std::size_t const he0 = halfEdges.size();
                    halfEdges.push_back({a, b, false, edgeIdx});
                    outgoing[a].push_back(he0);
                    directedToHalfEdge[halfEdgeKey(a, b)] = he0;

                    std::size_t const he1 = halfEdges.size();
                    halfEdges.push_back({b, a, false, edgeIdx});
                    outgoing[b].push_back(he1);
                    directedToHalfEdge[halfEdgeKey(b, a)] = he1;
                }

                // Add synthetic bridge edges to the half-edge set.
                for (std::size_t i = 0U; i < syntheticEdges.size(); ++i)
                {
                    std::uint32_t const a = syntheticEdges[i].v0;
                    std::uint32_t const b = syntheticEdges[i].v1;

                    std::size_t const he0 = halfEdges.size();
                    halfEdges.push_back({a, b, true, i});
                    outgoing[a].push_back(he0);
                    directedToHalfEdge[halfEdgeKey(a, b)] = he0;

                    std::size_t const he1 = halfEdges.size();
                    halfEdges.push_back({b, a, true, i});
                    outgoing[b].push_back(he1);
                    directedToHalfEdge[halfEdgeKey(b, a)] = he1;
                }

                auto projectToPlane = [&uAxis, &vAxis](Eigen::Vector3f const & vec) -> Eigen::Vector2d
                {
                    Eigen::Vector3d const v(vec.x(), vec.y(), vec.z());
                    return Eigen::Vector2d(v.dot(uAxis), v.dot(vAxis));
                };

                auto nextHalfEdge = [&](HalfEdge const & he) -> std::size_t
                {
                    // We arrived at he.to from he.from. Choose outgoing (he.to -> w)
                    // that is the next CCW edge in the component's best-fit plane.
                    auto itOut = outgoing.find(he.to);
                    if (itOut == outgoing.end() || itOut->second.empty())
                    {
                        return std::numeric_limits<std::size_t>::max();
                    }

                    Eigen::Vector3f const & pFrom = m_mesh.positions[he.from];
                    Eigen::Vector3f const & pTo = m_mesh.positions[he.to];
                    Eigen::Vector2d const inDir = projectToPlane(pFrom - pTo);
                    double const inLenSq = inDir.squaredNorm();
                    if (inLenSq < 1e-24)
                    {
                        return std::numeric_limits<std::size_t>::max();
                    }
                    Eigen::Vector2d const inUnit = inDir / std::sqrt(inLenSq);

                    std::size_t best = std::numeric_limits<std::size_t>::max();
                    double bestAngle = std::numeric_limits<double>::infinity();

                    std::size_t bestFallback = std::numeric_limits<std::size_t>::max();
                    double bestFallbackDot = -std::numeric_limits<double>::infinity();

                    for (std::size_t candIdx : itOut->second)
                    {
                        HalfEdge const & cand = halfEdges[candIdx];
                        if (cand.to == he.from)
                        {
                            // Prefer not to immediately reverse, but keep as fallback.
                            continue;
                        }

                        Eigen::Vector3f const & pOther = m_mesh.positions[cand.to];
                        Eigen::Vector2d outDir = projectToPlane(pOther - pTo);
                        double const outLenSq = outDir.squaredNorm();
                        if (outLenSq < 1e-24)
                        {
                            continue;
                        }
                        Eigen::Vector2d const outUnit = outDir / std::sqrt(outLenSq);

                        double dot = inUnit.dot(outUnit);
                        dot = std::clamp(dot, -1.0, 1.0);
                        double cross = inUnit.x() * outUnit.y() - inUnit.y() * outUnit.x();
                        double angle = std::atan2(cross, dot); // (-pi, pi]
                        if (angle <= 1e-12)
                        {
                            angle += 2.0 * M_PI;
                        }

                        if (angle < bestAngle)
                        {
                            bestAngle = angle;
                            best = candIdx;
                        }

                        // Fallback: keep the "straightest" option in case we must reverse.
                        if (dot > bestFallbackDot)
                        {
                            bestFallbackDot = dot;
                            bestFallback = candIdx;
                        }
                    }

                    if (best != std::numeric_limits<std::size_t>::max())
                    {
                        return best;
                    }

                    // If the only option is to reverse (degree 1), do it.
                    auto itReverse = directedToHalfEdge.find(halfEdgeKey(he.to, he.from));
                    if (itReverse != directedToHalfEdge.end())
                    {
                        return itReverse->second;
                    }

                    return bestFallback;
                };

                // Extract loops using the half-edge "next" relation.
                std::vector<bool> localHalfUsed(halfEdges.size(), false);
                std::vector<bool> syntheticUsed(syntheticEdges.size(), false);
                for (std::size_t startHe = 0U; startHe < halfEdges.size(); ++startHe)
                {
                    if (localHalfUsed[startHe])
                    {
                        continue;
                    }

                    HalfEdge const & startHalf = halfEdges[startHe];
                    if (!startHalf.isSynthetic && edgeUsed[startHalf.idx])
                    {
                        localHalfUsed[startHe] = true;
                        continue;
                    }
                    if (startHalf.isSynthetic && startHalf.idx < syntheticUsed.size() && syntheticUsed[startHalf.idx])
                    {
                        localHalfUsed[startHe] = true;
                        continue;
                    }

                    struct EdgeRef
                    {
                        bool isSynthetic;
                        std::size_t idx;
                    };

                    std::vector<std::size_t> cycleHalfEdges;
                    std::vector<EdgeRef> cycleEdges;
                    std::vector<std::size_t> cycleBoundaryEdgeIndices;
                    std::vector<std::uint32_t> loopVertices;
                    std::unordered_set<std::size_t> visited;
                    visited.reserve(128U);

                    std::size_t he = startHe;
                    bool foundLoop = false;
                    bool containsSynthetic = false;
                    while (cycleHalfEdges.size() < maxLoopSize)
                    {
                        if (visited.count(he) != 0)
                        {
                            break;
                        }

                        HalfEdge const & curr = halfEdges[he];
                        if (!curr.isSynthetic && edgeUsed[curr.idx])
                        {
                            break;
                        }
                        if (curr.isSynthetic && curr.idx < syntheticUsed.size() && syntheticUsed[curr.idx])
                        {
                            break;
                        }

                        visited.insert(he);
                        cycleHalfEdges.push_back(he);
                        cycleEdges.push_back({curr.isSynthetic, curr.idx});
                        containsSynthetic = containsSynthetic || curr.isSynthetic;
                        if (!curr.isSynthetic)
                        {
                            cycleBoundaryEdgeIndices.push_back(curr.idx);
                        }
                        loopVertices.push_back(curr.from);

                        std::size_t const nextHe = nextHalfEdge(halfEdges[he]);
                        if (nextHe == std::numeric_limits<std::size_t>::max())
                        {
                            break;
                        }
                        if (nextHe == startHe && loopVertices.size() >= 3U)
                        {
                            foundLoop = true;
                            break;
                        }
                        he = nextHe;
                    }

                    // Mark locally visited half-edges so we don't repeatedly attempt the same walk.
                    for (std::size_t heIdx : cycleHalfEdges)
                    {
                        localHalfUsed[heIdx] = true;
                    }

                    if (!foundLoop || loopVertices.size() < 3U)
                    {
                        continue;
                    }

                    // Never cap loops that depend on synthetic edges. Those synthetic edges are
                    // not part of the original boundary and would become *new* boundary edges
                    // after capping.
                    if (containsSynthetic)
                    {
                        continue;
                    }

                    // Commit: mark underlying undirected edges as used.
                    for (EdgeRef const & e : cycleEdges)
                    {
                        // containsSynthetic is false here.
                        edgeUsed[e.idx] = true;
                    }

                    ++loopCount;
                    cappedEdges += loopVertices.size();

                    // Compute a representative normal (from the original boundary triangles).
                    Eigen::Vector3f avgNormal = Eigen::Vector3f::Zero();
                    for (std::size_t edgeIdx : cycleBoundaryEdgeIndices)
                    {
                        BoundaryEdge const & be = boundaryEdges[edgeIdx];
                        Eigen::Vector3f const & p0 = m_mesh.positions[be.edge.v0];
                        Eigen::Vector3f const & p1 = m_mesh.positions[be.edge.v1];
                        Eigen::Vector3f const & p2 = m_mesh.positions[be.thirdVertex];
                        Eigen::Vector3f const n = (p1 - p0).cross(p2 - p0);
                        if (n.squaredNorm() > 0.0F)
                        {
                            avgNormal += n.normalized();
                        }
                    }
                    if (avgNormal.squaredNorm() > 0.0F)
                    {
                        avgNormal.normalize();
                    }

                    // Robust-ish triangulation:
                    // Project the polygon to 2D in the (avgNormal) plane and ear-clip.
                    // This avoids the centroid fan degeneracies that can leave open edges or
                    // create non-manifold overlaps on concave / non-uniform loops.
                    auto triangulateLoopEarClipping =
                      [this, &capTriangles](std::vector<std::uint32_t> const & loop,
                                            Eigen::Vector3f const & targetNormal) -> bool
                    {
                        // Remove consecutive duplicates (including wrap-around).
                        std::vector<std::uint32_t> poly;
                        poly.reserve(loop.size());
                        for (std::uint32_t v : loop)
                        {
                            if (poly.empty() || poly.back() != v)
                            {
                                poly.push_back(v);
                            }
                        }
                        if (poly.size() >= 2U && poly.front() == poly.back())
                        {
                            poly.pop_back();
                        }
                        if (poly.size() < 3U)
                        {
                            return false;
                        }

                        // Build 2D basis from targetNormal.
                        Eigen::Vector3f n = targetNormal;
                        if (n.squaredNorm() < 1e-12F)
                        {
                            // Fallback: derive a normal from first non-degenerate triple.
                            for (std::size_t i = 0U; i + 2U < poly.size(); ++i)
                            {
                                Eigen::Vector3f const & a = m_mesh.positions[poly[i]];
                                Eigen::Vector3f const & b = m_mesh.positions[poly[i + 1U]];
                                Eigen::Vector3f const & c = m_mesh.positions[poly[i + 2U]];
                                Eigen::Vector3f const nn = (b - a).cross(c - a);
                                if (nn.squaredNorm() > 1e-12F)
                                {
                                    n = nn.normalized();
                                    break;
                                }
                            }
                        }
                        if (n.squaredNorm() < 1e-12F)
                        {
                            return false;
                        }
                        n.normalize();

                        Eigen::Vector3f ref = (std::abs(n.x()) < 0.9F) ? Eigen::Vector3f::UnitX()
                                                                     : Eigen::Vector3f::UnitY();
                        Eigen::Vector3f u = n.cross(ref);
                        if (u.squaredNorm() < 1e-12F)
                        {
                            ref = Eigen::Vector3f::UnitZ();
                            u = n.cross(ref);
                        }
                        u.normalize();
                        Eigen::Vector3f const v = n.cross(u);

                        struct Vec2
                        {
                            float x;
                            float y;
                        };
                        std::vector<Vec2> poly2;
                        poly2.reserve(poly.size());
                        for (std::uint32_t idx : poly)
                        {
                            Eigen::Vector3f const & p = m_mesh.positions[idx];
                            poly2.push_back({p.dot(u), p.dot(v)});
                        }

                        auto signedArea = [&poly2]() -> float
                        {
                            double area = 0.0;
                            for (std::size_t i = 0U; i < poly2.size(); ++i)
                            {
                                Vec2 const & a = poly2[i];
                                Vec2 const & b = poly2[(i + 1U) % poly2.size()];
                                area += static_cast<double>(a.x) * static_cast<double>(b.y) -
                                        static_cast<double>(b.x) * static_cast<double>(a.y);
                            }
                            return static_cast<float>(0.5 * area);
                        };

                        float const area = signedArea();
                        if (std::abs(area) < 1e-8F)
                        {
                            return false;
                        }
                        bool const isCcw = (area > 0.0F);

                        auto cross2 = [](Vec2 const & a, Vec2 const & b, Vec2 const & c) -> float
                        {
                            // Cross of (b-a) x (c-a) in 2D (z-component)
                            float const abx = b.x - a.x;
                            float const aby = b.y - a.y;
                            float const acx = c.x - a.x;
                            float const acy = c.y - a.y;
                            return abx * acy - aby * acx;
                        };

                        auto pointInTri = [](Vec2 const & p, Vec2 const & a, Vec2 const & b, Vec2 const & c) -> bool
                        {
                            // Barycentric test in 2D
                            float const v0x = c.x - a.x;
                            float const v0y = c.y - a.y;
                            float const v1x = b.x - a.x;
                            float const v1y = b.y - a.y;
                            float const v2x = p.x - a.x;
                            float const v2y = p.y - a.y;

                            float const dot00 = v0x * v0x + v0y * v0y;
                            float const dot01 = v0x * v1x + v0y * v1y;
                            float const dot02 = v0x * v2x + v0y * v2y;
                            float const dot11 = v1x * v1x + v1y * v1y;
                            float const dot12 = v1x * v2x + v1y * v2y;

                            float const denom = dot00 * dot11 - dot01 * dot01;
                            if (std::abs(denom) < 1e-12F)
                            {
                                return false;
                            }
                            float const invDenom = 1.0F / denom;
                            float const uu = (dot11 * dot02 - dot01 * dot12) * invDenom;
                            float const vv = (dot00 * dot12 - dot01 * dot02) * invDenom;
                            return (uu >= -1e-6F) && (vv >= -1e-6F) && (uu + vv <= 1.0F + 1e-6F);
                        };

                        // Work on a mutable index list.
                        std::vector<std::size_t> indices(poly.size());
                        for (std::size_t i = 0U; i < indices.size(); ++i)
                        {
                            indices[i] = i;
                        }

                        std::size_t emittedTris = 0U;
                        std::size_t safety = 0U;
                        std::size_t const maxSteps = poly.size() * poly.size();
                        while (indices.size() >= 3U && safety++ < maxSteps)
                        {
                            bool earFound = false;
                            for (std::size_t ii = 0U; ii < indices.size(); ++ii)
                            {
                                std::size_t const iPrev = indices[(ii + indices.size() - 1U) % indices.size()];
                                std::size_t const iCurr = indices[ii];
                                std::size_t const iNext = indices[(ii + 1U) % indices.size()];

                                Vec2 const & a = poly2[iPrev];
                                Vec2 const & b = poly2[iCurr];
                                Vec2 const & c = poly2[iNext];

                                float const cr = cross2(a, b, c);
                                if (isCcw)
                                {
                                    if (cr <= 1e-8F)
                                    {
                                        continue; // Not convex
                                    }
                                }
                                else
                                {
                                    if (cr >= -1e-8F)
                                    {
                                        continue; // Not convex
                                    }
                                }

                                bool containsPoint = false;
                                for (std::size_t jj = 0U; jj < indices.size(); ++jj)
                                {
                                    std::size_t const k = indices[jj];
                                    if (k == iPrev || k == iCurr || k == iNext)
                                    {
                                        continue;
                                    }
                                    if (pointInTri(poly2[k], a, b, c))
                                    {
                                        containsPoint = true;
                                        break;
                                    }
                                }
                                if (containsPoint)
                                {
                                    continue;
                                }

                                std::uint32_t const vA = poly[iPrev];
                                std::uint32_t const vB = poly[iCurr];
                                std::uint32_t const vC = poly[iNext];
                                if (vA == vB || vB == vC || vC == vA)
                                {
                                    indices.erase(indices.begin() + static_cast<std::ptrdiff_t>(ii));
                                    earFound = true;
                                    break;
                                }

                                // Emit with winding consistent with targetNormal.
                                Eigen::Vector3f const & pA = m_mesh.positions[vA];
                                Eigen::Vector3f const & pB = m_mesh.positions[vB];
                                Eigen::Vector3f const & pC = m_mesh.positions[vC];
                                Eigen::Vector3f const triN = (pB - pA).cross(pC - pA);
                                if (triN.squaredNorm() < 1e-12F)
                                {
                                    indices.erase(indices.begin() + static_cast<std::ptrdiff_t>(ii));
                                    earFound = true;
                                    break;
                                }

                                if (triN.dot(n) >= 0.0F)
                                {
                                    capTriangles.push_back(vA);
                                    capTriangles.push_back(vB);
                                    capTriangles.push_back(vC);
                                }
                                else
                                {
                                    capTriangles.push_back(vA);
                                    capTriangles.push_back(vC);
                                    capTriangles.push_back(vB);
                                }

                                ++emittedTris;

                                indices.erase(indices.begin() + static_cast<std::ptrdiff_t>(ii));
                                earFound = true;
                                break;
                            }

                            if (!earFound)
                            {
                                return false;
                            }
                        }

                        // If we hit the safety limit or didn't fully reduce to a line segment,
                        // treat as failure so we can fall back to a more conservative method.
                        if (indices.size() >= 3U || safety >= maxSteps || emittedTris == 0U)
                        {
                            return false;
                        }

                        return true;
                    };

                    if (!triangulateLoopEarClipping(loopVertices, avgNormal))
                    {
                        // Fall back to the previous centroid fan if ear clipping fails.
                        Eigen::Vector3f centroidF = Eigen::Vector3f::Zero();
                        for (std::uint32_t vtx : loopVertices)
                        {
                            centroidF += m_mesh.positions[vtx];
                        }
                        centroidF /= static_cast<float>(loopVertices.size());

                        std::uint32_t const centroidIndex =
                          static_cast<std::uint32_t>(m_mesh.positions.size());
                        m_mesh.positions.push_back(centroidF);
                        if (!m_mesh.normals.empty())
                        {
                            m_mesh.normals.push_back(avgNormal);
                        }

                        for (std::size_t i = 0U; i < loopVertices.size(); ++i)
                        {
                            std::uint32_t const v0 = loopVertices[i];
                            std::uint32_t const v1 = loopVertices[(i + 1U) % loopVertices.size()];

                            if (v0 == v1 || v0 == centroidIndex || v1 == centroidIndex)
                            {
                                continue;
                            }

                            Eigen::Vector3f const & p0 = m_mesh.positions[v0];
                            Eigen::Vector3f const & p1 = m_mesh.positions[v1];
                            Eigen::Vector3f const & pc = m_mesh.positions[centroidIndex];
                            Eigen::Vector3f const triNormal = (p1 - p0).cross(pc - p0);
                            if (triNormal.squaredNorm() < 1e-12F)
                            {
                                continue;
                            }

                            if (avgNormal.squaredNorm() == 0.0F || triNormal.dot(avgNormal) >= 0.0F)
                            {
                                capTriangles.push_back(v0);
                                capTriangles.push_back(v1);
                                capTriangles.push_back(centroidIndex);
                            }
                            else
                            {
                                capTriangles.push_back(v0);
                                capTriangles.push_back(centroidIndex);
                                capTriangles.push_back(v1);
                            }
                        }
                    }
                }
            }

            // Phase 1b (fallback): Stitch remaining internal boundary edges in pairs.
            // These remaining edges are typically short open chains (degree-1 endpoints) caused by
            // missing neighbor coverage. Pairing nearby boundary edges and bridging them with a quad
            // often closes the crack without introducing branching.
            std::vector<std::size_t> remainingEdges;
            remainingEdges.reserve(boundaryEdges.size());
            for (std::size_t i = 0U; i < boundaryEdges.size(); ++i)
            {
                if (!edgeUsed[i])
                {
                    remainingEdges.push_back(i);
                }
            }

            if (!remainingEdges.empty())
            {
                float avgRemainingEdgeLen = 0.0F;
                for (std::size_t idx : remainingEdges)
                {
                    BoundaryEdge const & be = boundaryEdges[idx];
                    avgRemainingEdgeLen += (m_mesh.positions[be.edge.v1] - m_mesh.positions[be.edge.v0]).norm();
                }
                avgRemainingEdgeLen /= static_cast<float>(remainingEdges.size());

                // Stitching is a last resort. Keep the radius tight so we only bridge tiny cracks.
                float const stitchRadius = std::max(searchRadius * 2.0F, avgRemainingEdgeLen * 2.0F);
                float const stitchRadiusSq = stitchRadius * stitchRadius;
                float const cellSize = stitchRadius * 2.0F;
                float const invCellSize = 1.0F / cellSize;

                auto hashPos = [invCellSize](Eigen::Vector3f const & pos) -> std::uint64_t
                {
                    auto const ix = static_cast<std::int32_t>(std::floor(pos.x() * invCellSize));
                    auto const iy = static_cast<std::int32_t>(std::floor(pos.y() * invCellSize));
                    auto const iz = static_cast<std::int32_t>(std::floor(pos.z() * invCellSize));
                    std::uint64_t const hx = static_cast<std::uint64_t>(ix) & 0x1FFFFF;
                    std::uint64_t const hy = static_cast<std::uint64_t>(iy) & 0x1FFFFF;
                    std::uint64_t const hz = static_cast<std::uint64_t>(iz) & 0x1FFFFF;
                    return (hx << 42) | (hy << 21) | hz;
                };

                std::unordered_map<std::uint64_t, std::vector<std::size_t>> spatial;
                spatial.reserve(remainingEdges.size());
                std::vector<Eigen::Vector3f> midpoints(boundaryEdges.size(), Eigen::Vector3f::Zero());

                for (std::size_t idx : remainingEdges)
                {
                    BoundaryEdge const & be = boundaryEdges[idx];
                    Eigen::Vector3f const & p0 = m_mesh.positions[be.edge.v0];
                    Eigen::Vector3f const & p1 = m_mesh.positions[be.edge.v1];
                    midpoints[idx] = (p0 + p1) * 0.5F;
                    spatial[hashPos(midpoints[idx])].push_back(idx);
                }

                std::vector<std::uint32_t> stitchTriangles;
                stitchTriangles.reserve(remainingEdges.size() * 6U);
                std::size_t stitchedPairs = 0U;

                std::vector<bool> stitched(boundaryEdges.size(), false);

                for (std::size_t idx : remainingEdges)
                {
                    if (edgeUsed[idx] || stitched[idx])
                    {
                        continue;
                    }

                    Eigen::Vector3f const & mid = midpoints[idx];
                    auto const ix = static_cast<std::int32_t>(std::floor(mid.x() * invCellSize));
                    auto const iy = static_cast<std::int32_t>(std::floor(mid.y() * invCellSize));
                    auto const iz = static_cast<std::int32_t>(std::floor(mid.z() * invCellSize));

                    std::size_t bestMatch = std::numeric_limits<std::size_t>::max();
                    float bestDistSq = stitchRadiusSq;

                    for (int dz = -1; dz <= 1; ++dz)
                    {
                        for (int dy = -1; dy <= 1; ++dy)
                        {
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                std::uint64_t const hx = static_cast<std::uint64_t>(ix + dx) & 0x1FFFFF;
                                std::uint64_t const hy = static_cast<std::uint64_t>(iy + dy) & 0x1FFFFF;
                                std::uint64_t const hz = static_cast<std::uint64_t>(iz + dz) & 0x1FFFFF;
                                std::uint64_t const h = (hx << 42) | (hy << 21) | hz;

                                auto it = spatial.find(h);
                                if (it == spatial.end())
                                {
                                    continue;
                                }
                                for (std::size_t cand : it->second)
                                {
                                    if (cand == idx || edgeUsed[cand] || stitched[cand])
                                    {
                                        continue;
                                    }
                                    float const distSq = (midpoints[cand] - mid).squaredNorm();
                                    if (distSq < bestDistSq)
                                    {
                                        bestDistSq = distSq;
                                        bestMatch = cand;
                                    }
                                }
                            }
                        }
                    }

                    if (bestMatch == std::numeric_limits<std::size_t>::max())
                    {
                        continue;
                    }

                    // Bridge idx and bestMatch with a quad, choosing endpoint pairing by minimal distance.
                    BoundaryEdge const & e0 = boundaryEdges[idx];
                    BoundaryEdge const & e1 = boundaryEdges[bestMatch];

                    std::uint32_t const a0 = e0.edge.v0;
                    std::uint32_t const b0 = e0.edge.v1;
                    std::uint32_t const a1 = e1.edge.v0;
                    std::uint32_t const b1 = e1.edge.v1;

                    Eigen::Vector3f const & pA0 = m_mesh.positions[a0];
                    Eigen::Vector3f const & pB0 = m_mesh.positions[b0];
                    Eigen::Vector3f const & pA1 = m_mesh.positions[a1];
                    Eigen::Vector3f const & pB1 = m_mesh.positions[b1];

                    float const pair0 = (pA0 - pA1).squaredNorm() + (pB0 - pB1).squaredNorm();
                    float const pair1 = (pA0 - pB1).squaredNorm() + (pB0 - pA1).squaredNorm();

                    std::uint32_t c = a1;
                    std::uint32_t d = b1;
                    if (pair1 < pair0)
                    {
                        c = b1;
                        d = a1;
                    }

                    // Average normal from the two original boundary triangles.
                    Eigen::Vector3f avgNormal = Eigen::Vector3f::Zero();
                    {
                        Eigen::Vector3f const & p2 = m_mesh.positions[e0.thirdVertex];
                        Eigen::Vector3f const n0 = (pB0 - pA0).cross(p2 - pA0);
                        if (n0.squaredNorm() > 0.0F)
                        {
                            avgNormal += n0.normalized();
                        }
                    }
                    {
                        Eigen::Vector3f const & p2 = m_mesh.positions[e1.thirdVertex];
                        Eigen::Vector3f const n1 = (m_mesh.positions[b1] - m_mesh.positions[a1]).cross(p2 - m_mesh.positions[a1]);
                        if (n1.squaredNorm() > 0.0F)
                        {
                            avgNormal += n1.normalized();
                        }
                    }
                    if (avgNormal.squaredNorm() > 0.0F)
                    {
                        avgNormal.normalize();
                    }

                    // Triangulate quad (a0, b0, d, c) as (a0,b0,d) and (a0,d,c).
                    auto emitTri = [&](std::uint32_t v0, std::uint32_t v1, std::uint32_t v2)
                    {
                        if (v0 == v1 || v1 == v2 || v2 == v0)
                        {
                            return;
                        }
                        Eigen::Vector3f const & p0 = m_mesh.positions[v0];
                        Eigen::Vector3f const & p1 = m_mesh.positions[v1];
                        Eigen::Vector3f const & p2 = m_mesh.positions[v2];
                        Eigen::Vector3f const n = (p1 - p0).cross(p2 - p0);
                        if (n.squaredNorm() < 1e-12F)
                        {
                            return;
                        }
                        if (avgNormal.squaredNorm() == 0.0F || n.dot(avgNormal) >= 0.0F)
                        {
                            stitchTriangles.push_back(v0);
                            stitchTriangles.push_back(v1);
                            stitchTriangles.push_back(v2);
                        }
                        else
                        {
                            stitchTriangles.push_back(v0);
                            stitchTriangles.push_back(v2);
                            stitchTriangles.push_back(v1);
                        }
                    };

                    emitTri(a0, b0, d);
                    emitTri(a0, d, c);

                    edgeUsed[idx] = true;
                    edgeUsed[bestMatch] = true;
                    stitched[idx] = true;
                    stitched[bestMatch] = true;
                    ++stitchedPairs;
                }

                if (!stitchTriangles.empty())
                {
                    capTriangles.insert(capTriangles.end(), stitchTriangles.begin(), stitchTriangles.end());
                }

                if (debugEnabled)
                {
                    std::cout << "  Gap filling: Stitched " << stitchedPairs << " internal boundary edge pairs with "
                              << (stitchTriangles.size() / 3U) << " bridge triangles (radius=" << stitchRadius << ")" << std::endl;
                }
            }

            if (!capTriangles.empty())
            {
                std::size_t const triCount = capTriangles.size() / 3U;
                m_mesh.indices.insert(m_mesh.indices.end(), capTriangles.begin(), capTriangles.end());

                std::size_t remaining = 0U;
                for (bool u : edgeUsed)
                {
                    if (!u)
                    {
                        ++remaining;
                    }
                }

                if (debugEnabled)
                {
                    std::cout << "  Gap filling: Internal boundary components=" << componentCount
                              << ", loops capped=" << loopCount << std::endl;
                }
                std::cout << "  Gap filling: Capped " << loopCount << " internal loops with "
                          << triCount << " cap triangles (capped " << cappedEdges
                          << " edges, " << remaining << " edges unlooped)" << std::endl;
            }
            else
            {
                std::cout << "  Gap filling: No closed internal boundary loops found" << std::endl;
            }
        }

        // ========================================================================
        // Phase 2: Cap bbox boundary edges (CSG intersection boundaries)
        // These edges form closed loops on each bbox face that need triangulation
        // ========================================================================
        if (!bboxBoundaryEdges.empty())
        {
            std::vector<std::uint32_t> capTriangles;

            // Build adjacency map for bbox boundary edges: vertex -> list of edges starting there
            std::unordered_map<std::uint32_t, std::vector<std::size_t>> vertexToEdges;
            for (std::size_t i = 0U; i < bboxBoundaryEdges.size(); ++i)
            {
                vertexToEdges[bboxBoundaryEdges[i].edge.v0].push_back(i);
            }

            // Track which edges have been used in loops
            std::vector<bool> bboxEdgeUsed(bboxBoundaryEdges.size(), false);
            std::size_t loopCount = 0U;
            std::size_t cappedEdges = 0U;

            // Find and triangulate closed loops
            for (std::size_t startIdx = 0U; startIdx < bboxBoundaryEdges.size(); ++startIdx)
            {
                if (bboxEdgeUsed[startIdx])
                {
                    continue;
                }

                // Try to build a closed loop starting from this edge
                // Don't mark edges as used until we confirm the loop is closed
                std::vector<std::size_t> loopEdgeIndices; // Edge indices in the loop
                std::vector<std::uint32_t> loopVertices;  // Vertex sequence
                std::set<std::size_t> visitedEdges;       // Edges in current attempt

                std::size_t currentIdx = startIdx;
                std::uint32_t startVertex = bboxBoundaryEdges[startIdx].edge.v0;

                bool foundLoop = false;
                std::size_t const maxLoopSize = 1000U; // Prevent infinite loops

                while (loopEdgeIndices.size() < maxLoopSize)
                {
                    if (bboxEdgeUsed[currentIdx] || visitedEdges.count(currentIdx) > 0)
                    {
                        break; // Already used or visited in this attempt
                    }

                    BoundaryEdge const & edge = bboxBoundaryEdges[currentIdx];
                    loopEdgeIndices.push_back(currentIdx);
                    loopVertices.push_back(edge.edge.v0);
                    visitedEdges.insert(currentIdx);

                    std::uint32_t nextVertex = edge.edge.v1;

                    // Check if we closed the loop
                    if (nextVertex == startVertex && loopVertices.size() >= 3U)
                    {
                        foundLoop = true;
                        break;
                    }

                    // Find next edge in the chain
                    auto it = vertexToEdges.find(nextVertex);
                    if (it == vertexToEdges.end() || it->second.empty())
                    {
                        break; // Dead end
                    }

                    // Find an unused edge from this vertex
                    bool foundNext = false;
                    for (std::size_t nextIdx : it->second)
                    {
                        if (!bboxEdgeUsed[nextIdx] && visitedEdges.count(nextIdx) == 0)
                        {
                            currentIdx = nextIdx;
                            foundNext = true;
                            break;
                        }
                    }

                    if (!foundNext)
                    {
                        break; // No more edges
                    }
                }

                // If we found a closed loop with at least 3 vertices, triangulate it
                if (foundLoop && loopVertices.size() >= 3U)
                {
                    // Mark all edges in this loop as used
                    for (std::size_t edgeIdx : loopEdgeIndices)
                    {
                        bboxEdgeUsed[edgeIdx] = true;
                    }

                    ++loopCount;
                    cappedEdges += loopVertices.size();

                    // Get the average normal from original triangles (for winding)
                    Eigen::Vector3f avgNormal = Eigen::Vector3f::Zero();
                    for (std::size_t i = 0U; i < loopVertices.size(); ++i)
                    {
                        std::uint32_t v0 = loopVertices[i];
                        std::uint32_t v1 = loopVertices[(i + 1U) % loopVertices.size()];

                        // Find the original edge to get its third vertex
                        for (auto const & be : bboxBoundaryEdges)
                        {
                            if (be.edge.v0 == v0 && be.edge.v1 == v1)
                            {
                                Eigen::Vector3f const & p0 = m_mesh.positions[be.edge.v0];
                                Eigen::Vector3f const & p1 = m_mesh.positions[be.edge.v1];
                                Eigen::Vector3f const & p2 = m_mesh.positions[be.thirdVertex];
                                avgNormal += (p1 - p0).cross(p2 - p0).normalized();
                                break;
                            }
                        }
                    }
                    if (avgNormal.squaredNorm() > 0.0F)
                    {
                        avgNormal.normalize();
                    }

                    // Simple fan triangulation from first vertex
                    // Works well for convex-ish loops, may need ear-clipping for complex ones
                    std::uint32_t const anchor = loopVertices[0];
                    for (std::size_t i = 1U; i + 1U < loopVertices.size(); ++i)
                    {
                        std::uint32_t v1 = loopVertices[i];
                        std::uint32_t v2 = loopVertices[i + 1U];

                        // Skip degenerate triangles
                        if (anchor == v1 || v1 == v2 || v2 == anchor)
                        {
                            continue;
                        }

                        // Check winding and emit triangle
                        Eigen::Vector3f const & p0 = m_mesh.positions[anchor];
                        Eigen::Vector3f const & p1 = m_mesh.positions[v1];
                        Eigen::Vector3f const & p2 = m_mesh.positions[v2];

                        Eigen::Vector3f triNormal = (p1 - p0).cross(p2 - p0);
                        if (triNormal.squaredNorm() < 1e-12F)
                        {
                            continue; // Degenerate
                        }

                        // Emit with correct winding based on average normal
                        if (triNormal.dot(avgNormal) >= 0.0F)
                        {
                            capTriangles.push_back(anchor);
                            capTriangles.push_back(v1);
                            capTriangles.push_back(v2);
                        }
                        else
                        {
                            capTriangles.push_back(anchor);
                            capTriangles.push_back(v2);
                            capTriangles.push_back(v1);
                        }
                    }
                }
            }

            // Add cap triangles to mesh
            if (!capTriangles.empty())
            {
                std::size_t const capTriCount = capTriangles.size() / 3U;
                m_mesh.indices.insert(
                  m_mesh.indices.end(), capTriangles.begin(), capTriangles.end());

                std::size_t uncappedEdges = 0U;
                for (bool used : bboxEdgeUsed)
                {
                    if (!used)
                    {
                        ++uncappedEdges;
                    }
                }

                std::cout << "  Bbox capping: Created " << capTriCount << " cap triangles from "
                          << loopCount << " loops"
                          << " (capped " << cappedEdges << " edges"
                          << ", " << uncappedEdges << " remain)" << std::endl;
            }
            else
            {
                std::cout << "  Bbox capping: No closed loops found in " << bboxBoundaryEdges.size()
                          << " bbox boundary edges" << std::endl;
            }

            // ----------------------------------------------------------------
            // Phase 2b: Stitch remaining uncapped bbox boundary edges
            // Use spatial hashing to find nearby edges and create bridge triangles
            // ----------------------------------------------------------------
            std::size_t uncappedCount = 0U;
            for (bool used : bboxEdgeUsed)
            {
                if (!used)
                {
                    ++uncappedCount;
                }
            }

            if (uncappedCount > 0U)
            {
                // Build spatial hash for uncapped bbox edge midpoints
                float const bboxStitchRadius = searchRadius * 2.0F; // Slightly larger radius
                float const bboxStitchRadiusSq = bboxStitchRadius * bboxStitchRadius;
                float const bboxCellSize = bboxStitchRadius * 2.0F;
                float const bboxInvCellSize = 1.0F / bboxCellSize;

                std::unordered_map<std::uint64_t, std::vector<std::size_t>> bboxEdgeSpatialHash;
                std::vector<Eigen::Vector3f> bboxEdgeMidpoints(bboxBoundaryEdges.size());

                auto bboxHashPos = [bboxInvCellSize](Eigen::Vector3f const & pos) -> std::uint64_t
                {
                    auto const ix =
                      static_cast<std::int32_t>(std::floor(pos.x() * bboxInvCellSize));
                    auto const iy =
                      static_cast<std::int32_t>(std::floor(pos.y() * bboxInvCellSize));
                    auto const iz =
                      static_cast<std::int32_t>(std::floor(pos.z() * bboxInvCellSize));
                    std::uint64_t const hx = static_cast<std::uint64_t>(ix) & 0x1FFFFF;
                    std::uint64_t const hy = static_cast<std::uint64_t>(iy) & 0x1FFFFF;
                    std::uint64_t const hz = static_cast<std::uint64_t>(iz) & 0x1FFFFF;
                    return (hx << 42) | (hy << 21) | hz;
                };

                for (std::size_t i = 0U; i < bboxBoundaryEdges.size(); ++i)
                {
                    if (bboxEdgeUsed[i])
                    {
                        continue;
                    }
                    Eigen::Vector3f const & p0 = m_mesh.positions[bboxBoundaryEdges[i].edge.v0];
                    Eigen::Vector3f const & p1 = m_mesh.positions[bboxBoundaryEdges[i].edge.v1];
                    bboxEdgeMidpoints[i] = (p0 + p1) * 0.5F;
                    bboxEdgeSpatialHash[bboxHashPos(bboxEdgeMidpoints[i])].push_back(i);
                }

                std::vector<std::uint32_t> stitchTriangles;

                for (std::size_t i = 0U; i < bboxBoundaryEdges.size(); ++i)
                {
                    if (bboxEdgeUsed[i])
                    {
                        continue;
                    }

                    Eigen::Vector3f const & midI = bboxEdgeMidpoints[i];
                    BoundaryEdge const & boundaryI = bboxBoundaryEdges[i];
                    DirectedEdge const & edgeI = boundaryI.edge;
                    Eigen::Vector3f const & pI0 = m_mesh.positions[edgeI.v0];
                    Eigen::Vector3f const & pI1 = m_mesh.positions[edgeI.v1];
                    float const edgeLenI = (pI1 - pI0).norm();

                    // Get normal from original triangle
                    Eigen::Vector3f const & pI2 = m_mesh.positions[boundaryI.thirdVertex];
                    Eigen::Vector3f const normalI = (pI1 - pI0).cross(pI2 - pI0).normalized();

                    // Search neighboring cells
                    auto const ix =
                      static_cast<std::int32_t>(std::floor(midI.x() * bboxInvCellSize));
                    auto const iy =
                      static_cast<std::int32_t>(std::floor(midI.y() * bboxInvCellSize));
                    auto const iz =
                      static_cast<std::int32_t>(std::floor(midI.z() * bboxInvCellSize));

                    std::size_t bestMatch = std::numeric_limits<std::size_t>::max();
                    float bestDistSq = bboxStitchRadiusSq;

                    for (int dz = -1; dz <= 1; ++dz)
                    {
                        for (int dy = -1; dy <= 1; ++dy)
                        {
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                std::uint64_t const hx =
                                  static_cast<std::uint64_t>(ix + dx) & 0x1FFFFF;
                                std::uint64_t const hy =
                                  static_cast<std::uint64_t>(iy + dy) & 0x1FFFFF;
                                std::uint64_t const hz =
                                  static_cast<std::uint64_t>(iz + dz) & 0x1FFFFF;
                                std::uint64_t const neighborHash = (hx << 42) | (hy << 21) | hz;

                                auto it = bboxEdgeSpatialHash.find(neighborHash);
                                if (it == bboxEdgeSpatialHash.end())
                                {
                                    continue;
                                }

                                for (std::size_t j : it->second)
                                {
                                    if (j == i || bboxEdgeUsed[j])
                                    {
                                        continue;
                                    }

                                    // Check edge length similarity
                                    BoundaryEdge const & boundaryJ = bboxBoundaryEdges[j];
                                    DirectedEdge const & edgeJ = boundaryJ.edge;
                                    Eigen::Vector3f const & pJ0 = m_mesh.positions[edgeJ.v0];
                                    Eigen::Vector3f const & pJ1 = m_mesh.positions[edgeJ.v1];
                                    float const edgeLenJ = (pJ1 - pJ0).norm();

                                    float const lenRatio =
                                      std::min(edgeLenI, edgeLenJ) / std::max(edgeLenI, edgeLenJ);
                                    if (lenRatio < 0.3F)
                                    {
                                        continue; // Edge lengths too different
                                    }

                                    // Check normal compatibility
                                    Eigen::Vector3f const & pJ2 =
                                      m_mesh.positions[boundaryJ.thirdVertex];
                                    Eigen::Vector3f const normalJ =
                                      (pJ1 - pJ0).cross(pJ2 - pJ0).normalized();
                                    float const normalDot = normalI.dot(normalJ);
                                    if (normalDot < 0.5F) // More lenient for bbox edges
                                    {
                                        continue;
                                    }

                                    float const distSq =
                                      (bboxEdgeMidpoints[j] - midI).squaredNorm();
                                    if (distSq < bestDistSq)
                                    {
                                        bestDistSq = distSq;
                                        bestMatch = j;
                                    }
                                }
                            }
                        }
                    }

                    if (bestMatch != std::numeric_limits<std::size_t>::max())
                    {
                        BoundaryEdge const & boundaryJ = bboxBoundaryEdges[bestMatch];
                        DirectedEdge const & edgeJ = boundaryJ.edge;

                        // Determine vertex pairing
                        float const d00 = (pI0 - m_mesh.positions[edgeJ.v0]).squaredNorm();
                        float const d01 = (pI0 - m_mesh.positions[edgeJ.v1]).squaredNorm();

                        std::uint32_t const jNear0 = (d00 < d01) ? edgeJ.v0 : edgeJ.v1;
                        std::uint32_t const jNear1 = (d00 < d01) ? edgeJ.v1 : edgeJ.v0;

                        // Create bridge triangles with winding check
                        auto addTriWithWinding =
                          [this, &stitchTriangles, &normalI](
                            std::uint32_t a, std::uint32_t b, std::uint32_t c) -> bool
                        {
                            if (a == b || b == c || c == a)
                            {
                                return false;
                            }

                            Eigen::Vector3f const & pA = m_mesh.positions[a];
                            Eigen::Vector3f const & pB = m_mesh.positions[b];
                            Eigen::Vector3f const & pC = m_mesh.positions[c];

                            Eigen::Vector3f const triNormal = (pB - pA).cross(pC - pA);
                            if (triNormal.squaredNorm() < 1e-12F)
                            {
                                return false;
                            }

                            if (triNormal.dot(normalI) < 0.0F)
                            {
                                stitchTriangles.push_back(a);
                                stitchTriangles.push_back(c);
                                stitchTriangles.push_back(b);
                            }
                            else
                            {
                                stitchTriangles.push_back(a);
                                stitchTriangles.push_back(b);
                                stitchTriangles.push_back(c);
                            }
                            return true;
                        };

                        addTriWithWinding(edgeI.v0, edgeI.v1, jNear1);
                        addTriWithWinding(edgeI.v0, jNear1, jNear0);

                        bboxEdgeUsed[i] = true;
                        bboxEdgeUsed[bestMatch] = true;
                    }
                }

                if (!stitchTriangles.empty())
                {
                    std::size_t const stitchTriCount = stitchTriangles.size() / 3U;
                    m_mesh.indices.insert(
                      m_mesh.indices.end(), stitchTriangles.begin(), stitchTriangles.end());

                    std::size_t finalUncapped = 0U;
                    for (bool used : bboxEdgeUsed)
                    {
                        if (!used)
                        {
                            ++finalUncapped;
                        }
                    }

                    std::cout << "  Bbox stitching: Created " << stitchTriCount
                              << " stitch triangles, " << finalUncapped
                              << " bbox edges remain uncapped" << std::endl;
                }
            }
        }
    }

    void ManifoldDualContouringGpu::generateMeshHierarchical()
    {
        std::cout << "Using hierarchical octree approach for watertight mesh generation"
                  << std::endl;

        reportProgress(0.08F, "Precomputing SDF");

        // Early cancellation check
        if (isCancelled())
        {
            return;
        }

        // Ensure SDF is precomputed for the SAME bounding box the octree will use.
        // Use the padded bbox computed in generateMesh() (2 voxels at maxDepth) to
        // avoid clipping the surface at the domain boundary.
        BoundingBox paddedBbox{};
        paddedBbox.min.s[0] = m_cachedBboxMin.x();
        paddedBbox.min.s[1] = m_cachedBboxMin.y();
        paddedBbox.min.s[2] = m_cachedBboxMin.z();
        paddedBbox.min.s[3] = 0.0F;
        paddedBbox.max.s[0] = m_cachedBboxMax.x();
        paddedBbox.max.s[1] = m_cachedBboxMax.y();
        paddedBbox.max.s[2] = m_cachedBboxMax.z();
        paddedBbox.max.s[3] = 0.0F;

        m_core.precomputeSdfForBBox(paddedBbox);

        // Check for cancellation after SDF precomputation
        if (isCancelled())
        {
            return;
        }

        reportProgress(0.15F, "Building hierarchical octree");

        // Create or reuse the hierarchical octree
        if (!m_hierarchicalOctree)
        {
            m_hierarchicalOctree = std::make_unique<GlobalMortonOctree>(m_core);
        }

        // Wire cancellation callback to the octree
        if (m_cancellationCheckCallback)
        {
            m_hierarchicalOctree->setCancellationCheckCallback(m_cancellationCheckCallback);
        }

        // Configure the octree
        GlobalMortonOctreeConfig octreeConfig;
        octreeConfig.initialDepth = m_config.initialDepth;
        octreeConfig.maxDepth = m_config.maxDepth;
        octreeConfig.isoValue = m_config.isoValue;
        octreeConfig.minFeatureSize = m_config.minFeatureSize;
        octreeConfig.enableAdaptiveRefinement = m_config.enableAdaptiveRefinement;
        octreeConfig.curvatureThreshold = m_config.curvatureThreshold;
        octreeConfig.refinementPasses = m_config.refinementPasses;
        octreeConfig.boundingBoxOverride = paddedBbox;

        // Build the octree
        m_hierarchicalOctree->build(octreeConfig);

        // Check for cancellation after octree build (including if cancelled during balancing)
        if (isCancelled() || m_hierarchicalOctree->wasCancelled())
        {
            return;
        }

        reportProgress(0.35F, "Extracting mesh");

        // Extract mesh
        m_hierarchicalOctree->extractMesh(m_mesh.positions, m_mesh.normals, m_mesh.indices);

        // Check for cancellation after mesh extraction
        if (isCancelled())
        {
            m_mesh.positions.clear();
            m_mesh.normals.clear();
            m_mesh.indices.clear();
            return;
        }

        // Report statistics
        auto const & stats = m_hierarchicalOctree->getStats();
        std::cout << "Hierarchical mesh generation complete:" << std::endl;
        std::cout << "  Vertices: " << m_mesh.positions.size() << std::endl;
        std::cout << "  Triangles: " << m_mesh.indices.size() / 3U << std::endl;
        std::cout << "  Boundary edges: " << stats.boundaryEdges << std::endl;
        std::cout << "  Non-manifold edges: " << stats.nonManifoldEdges << std::endl;

        if (stats.boundaryEdges > 0U)
        {
            std::cout << "  WARNING: Mesh is not watertight!" << std::endl;
        }
        if (stats.nonManifoldEdges > 0U)
        {
            std::cout << "  WARNING: Mesh has non-manifold edges!" << std::endl;
        }

        // The hierarchical path can also fail silently (e.g., due to numerical issues or
        // degenerate iso-surfaces) and return an empty mesh while reporting no boundary
        // or non-manifold edges. Treat empty output as a failure and fall back.
        if (m_mesh.positions.empty() || m_mesh.indices.empty())
        {
            std::cout << "  Falling back to non-hierarchical MDC because hierarchical output is empty." << std::endl;
            generateMeshNonHierarchical();
            return;
        }

        // The GlobalMortonOctree path is still experimental. If it produces a mesh that
        // fails basic watertight/manifold criteria, fall back to the proven GPU octree
        // path (single-pass or chunked). This preserves export correctness for users.
        if (stats.boundaryEdges > 0U || stats.nonManifoldEdges > 0U)
        {
            std::cout << "  Falling back to non-hierarchical MDC because hierarchical output is not watertight/manifold." << std::endl;
            generateMeshNonHierarchical();
        }
    }
}
