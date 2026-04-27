/// @file MeshFwnBenchmark_tests.cpp
/// @brief Cross-method timing benchmark for the host-side mesh-SDF reference
///        and the byte-faithful host emulator of the GPU FWN traversal.
///
/// This is a *CPU* benchmark — it validates the algorithmic cost of the FWN
/// optimisations (early-exit, voxel-magnitude reuse) without standing up the
/// full OpenCL pipeline. The host emulator mirrors the kernel, so relative
/// numbers between methods are representative of the GPU path's per-query
/// cost ratios.
///
/// Output is a printed table; the test passes when correctness on a sampled
/// subset is preserved. Run with:
///   ./gladius_test --gtest_filter='MeshFwnBenchmark*'

#include "MeshBVH.h"
#include "MeshSdfReference.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

namespace gladius::tests
{
    namespace
    {
        constexpr float kFourPi = 4.0f * 3.14159265358979323846f;

        float solidAngleAtOrigin(float4 const & av, float4 const & bv, float4 const & cv)
        {
            float const ax = av.x, ay = av.y, az = av.z;
            float const bx = bv.x, by = bv.y, bz = bv.z;
            float const cx = cv.x, cy = cv.y, cz = cv.z;
            float const la = std::sqrt(ax * ax + ay * ay + az * az);
            float const lb = std::sqrt(bx * bx + by * by + bz * bz);
            float const lc = std::sqrt(cx * cx + cy * cy + cz * cz);
            float const dotAB = ax * bx + ay * by + az * bz;
            float const dotBC = bx * cx + by * cy + bz * cz;
            float const dotCA = cx * ax + cy * ay + cz * az;
            float const numerator =
                ax * (by * cz - bz * cy) + ay * (bz * cx - bx * cz) + az * (bx * cy - by * cx);
            float const denominator = la * lb * lc + dotAB * lc + dotBC * la + dotCA * lb;
            return 2.0f * std::atan2(numerator, denominator);
        }

        /// Host port of `fwnHierarchical` from mesh_sdf.cl, byte-faithful with
        /// the early-exit at |windingSum| > 0.75.
        float fwnHierarchicalHost(SpatialMeshData const & data,
                                  float4 const & query,
                                  float beta,
                                  bool earlyExit)
        {
            if (data.nodes.empty())
            {
                return 0.f;
            }
            std::vector<int> stack;
            stack.reserve(64);
            stack.push_back(0);
            float windingSum = 0.f;
            float const betaSq = beta * beta;
            float const invFourPi = 1.f / kFourPi;
            while (!stack.empty())
            {
                int const nodeIdx = stack.back();
                stack.pop_back();
                auto const & ag = data.fwnAggregates[static_cast<std::size_t>(nodeIdx)];
                if (ag.areaCentroid.w <= 0.f)
                {
                    continue;
                }
                float const cx = ag.areaCentroid.x / ag.areaCentroid.w;
                float const cy = ag.areaCentroid.y / ag.areaCentroid.w;
                float const cz = ag.areaCentroid.z / ag.areaCentroid.w;
                float const dx = cx - query.x;
                float const dy = cy - query.y;
                float const dz = cz - query.z;
                float const distSq = dx * dx + dy * dy + dz * dz;
                float const radius = ag.weightedNormalSum.w;
                if (distSq > betaSq * radius * radius && distSq > 0.f)
                {
                    float const invDist = 1.f / std::sqrt(distSq);
                    float const invDistCubed = invDist * invDist * invDist;
                    float const Nx = 0.5f * ag.weightedNormalSum.x;
                    float const Ny = 0.5f * ag.weightedNormalSum.y;
                    float const Nz = 0.5f * ag.weightedNormalSum.z;
                    windingSum += invFourPi * (dx * Nx + dy * Ny + dz * Nz) * invDistCubed;
                    if (earlyExit && std::fabs(windingSum) > 0.75f)
                    {
                        return windingSum;
                    }
                    continue;
                }
                auto const & node = data.nodes[static_cast<std::size_t>(nodeIdx)];
                if (node.isLeaf())
                {
                    int const end = node.primStart + node.primCount;
                    for (int i = node.primStart; i < end; ++i)
                    {
                        auto const & tri = data.triangles[static_cast<std::size_t>(i)];
                        float4 const a{tri.v0.x - query.x, tri.v0.y - query.y, tri.v0.z - query.z, 0.f};
                        float4 const b{tri.v1.x - query.x, tri.v1.y - query.y, tri.v1.z - query.z, 0.f};
                        float4 const c{tri.v2.x - query.x, tri.v2.y - query.y, tri.v2.z - query.z, 0.f};
                        windingSum += invFourPi * solidAngleAtOrigin(a, b, c);
                    }
                }
                else
                {
                    if (node.rightChild >= 0)
                    {
                        stack.push_back(node.rightChild);
                    }
                    if (node.leftChild >= 0)
                    {
                        stack.push_back(node.leftChild);
                    }
                }
            }
            return windingSum;
        }

        // ----- Mesh generators --------------------------------------------------

        struct Mesh
        {
            std::vector<float4> vertices;
            std::vector<TriangleIndices> indices;
        };

        Mesh makeIcosphere(int subdivisions)
        {
            Mesh m;
            float const t = (1.0f + std::sqrt(5.0f)) * 0.5f;
            std::vector<float4> v = {
                {-1, t, 0, 0}, {1, t, 0, 0}, {-1, -t, 0, 0}, {1, -t, 0, 0},
                {0, -1, t, 0}, {0, 1, t, 0}, {0, -1, -t, 0}, {0, 1, -t, 0},
                {t, 0, -1, 0}, {t, 0, 1, 0}, {-t, 0, -1, 0}, {-t, 0, 1, 0},
            };
            for (auto & p : v)
            {
                float const len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
                p.x /= len; p.y /= len; p.z /= len;
            }
            std::vector<TriangleIndices> tris = {
                {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
                {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
                {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1},
            };
            for (int level = 0; level < subdivisions; ++level)
            {
                std::vector<TriangleIndices> next;
                next.reserve(tris.size() * 4);
                for (auto const & f : tris)
                {
                    auto midpoint = [&](int a, int b) {
                        float4 mp{(v[a].x + v[b].x) * 0.5f, (v[a].y + v[b].y) * 0.5f,
                                  (v[a].z + v[b].z) * 0.5f, 0.f};
                        float const len = std::sqrt(mp.x * mp.x + mp.y * mp.y + mp.z * mp.z);
                        mp.x /= len; mp.y /= len; mp.z /= len;
                        v.push_back(mp);
                        return static_cast<int>(v.size()) - 1;
                    };
                    int const a = midpoint(f.i0, f.i1);
                    int const b = midpoint(f.i1, f.i2);
                    int const c = midpoint(f.i2, f.i0);
                    next.push_back({f.i0, a, c});
                    next.push_back({f.i1, b, a});
                    next.push_back({f.i2, c, b});
                    next.push_back({a, b, c});
                }
                tris = std::move(next);
            }
            m.vertices = std::move(v);
            m.indices = std::move(tris);
            return m;
        }

        std::vector<float4> makeQueryGrid(std::size_t count, float radius, std::uint32_t seed = 42)
        {
            std::mt19937 rng(seed);
            std::uniform_real_distribution<float> dist(-radius * 1.5f, radius * 1.5f);
            std::vector<float4> queries;
            queries.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                queries.push_back({dist(rng), dist(rng), dist(rng), 0.f});
            }
            return queries;
        }

        struct Result
        {
            double meanUs;
            double p95Us;
            double queriesPerSecond;
            double accumulator;  ///< prevents the loop from being optimised away
        };

        template <typename Fn>
        Result timeRuns(std::vector<float4> const & queries, Fn && fn)
        {
            constexpr int kRuns = 5;
            std::vector<double> perRunUs;
            perRunUs.reserve(kRuns);
            double accumulator = 0.0;
            // Warm-up
            for (auto const & q : queries) { accumulator += fn(q); }
            for (int r = 0; r < kRuns; ++r)
            {
                auto const t0 = std::chrono::high_resolution_clock::now();
                for (auto const & q : queries) { accumulator += fn(q); }
                auto const t1 = std::chrono::high_resolution_clock::now();
                perRunUs.push_back(
                    std::chrono::duration<double, std::micro>(t1 - t0).count());
            }
            std::sort(perRunUs.begin(), perRunUs.end());
            double const mean = std::accumulate(perRunUs.begin(), perRunUs.end(), 0.0) / kRuns;
            double const p95 = perRunUs[static_cast<std::size_t>(kRuns * 0.95)];
            double const qps = static_cast<double>(queries.size()) / (mean * 1e-6);
            return {mean, p95, qps, accumulator};
        }
    } // namespace

    /// CPU benchmark that times each algorithm on the same icosphere + query
    /// set and prints a comparison table. The test asserts sign-correctness
    /// of the FWN-with-early-exit path against the brute-force reference on
    /// a sampled subset.
    TEST(MeshFwnBenchmark, ReferenceVsHierarchical_PrintsTimingTable)
    {
        auto const mesh = makeIcosphere(3); // ~1280 triangles
        auto const queries = makeQueryGrid(2048, 1.0f);
        MeshBVHBuilder builder;
        SpatialMeshData data = builder.build(mesh.vertices, mesh.indices);
        computeFwnAggregates(data);

        auto const refRes = timeRuns(queries, [&](float4 const & q) {
            return static_cast<double>(
                mesh_sdf_reference::referenceUnsignedDistance(mesh.vertices, mesh.indices, q));
        });
        auto const refWnRes = timeRuns(queries, [&](float4 const & q) {
            return static_cast<double>(
                mesh_sdf_reference::referenceWindingNumber(mesh.vertices, mesh.indices, q));
        });
        auto const fwnNoExit = timeRuns(queries, [&](float4 const & q) {
            return static_cast<double>(fwnHierarchicalHost(data, q, 2.0f, false));
        });
        auto const fwnWithExit = timeRuns(queries, [&](float4 const & q) {
            return static_cast<double>(fwnHierarchicalHost(data, q, 2.0f, true));
        });

        std::cout << "\n=== FWN host benchmark (" << mesh.indices.size() << " tris, "
                  << queries.size() << " queries, 5 runs) ===\n";
        std::cout << std::left << std::setw(40) << "Method"
                  << std::right << std::setw(12) << "mean (ms)"
                  << std::setw(12) << "p95 (ms)"
                  << std::setw(16) << "queries/s\n";
        auto print = [](char const * name, Result const & r) {
            std::cout << std::left << std::setw(40) << name
                      << std::right << std::fixed << std::setprecision(3)
                      << std::setw(12) << r.meanUs / 1000.0
                      << std::setw(12) << r.p95Us / 1000.0
                      << std::scientific << std::setprecision(2)
                      << std::setw(16) << r.queriesPerSecond << "\n";
        };
        print("reference unsigned distance (O(N))", refRes);
        print("reference winding (O(N))", refWnRes);
        print("FWN hierarchical (no early-exit)", fwnNoExit);
        print("FWN hierarchical (early-exit @ 0.75)", fwnWithExit);
        std::cout << std::defaultfloat;

        // Sign correctness on a small subset.
        std::size_t signMatches = 0;
        constexpr std::size_t kSignCheckCount = 256;
        for (std::size_t i = 0; i < kSignCheckCount; ++i)
        {
            auto const & q = queries[i];
            float const wRef = mesh_sdf_reference::referenceWindingNumber(
                mesh.vertices, mesh.indices, q);
            float const wFwn = fwnHierarchicalHost(data, q, 2.0f, true);
            if ((wRef > 0.5f) == (wFwn > 0.5f))
            {
                ++signMatches;
            }
        }
        EXPECT_GE(signMatches, kSignCheckCount * 99 / 100)
            << "FWN sign disagrees with reference on > 1% of queries";

        // Sanity: early-exit must be at least as fast as no-early-exit.
        EXPECT_LE(fwnWithExit.meanUs, fwnNoExit.meanUs * 1.05)
            << "Early-exit unexpectedly slower than no-early-exit";

        // Ensure accumulators differ from zero so the optimiser keeps the loops.
        EXPECT_NE(refRes.accumulator + fwnWithExit.accumulator, 0.0);
    }

} // namespace gladius::tests
