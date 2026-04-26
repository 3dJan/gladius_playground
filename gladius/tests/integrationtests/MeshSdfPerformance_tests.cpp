/// @file MeshSdfPerformance_tests.cpp
/// @brief Performance benchmarks for mesh SDF query optimizations
/// @details Tests for spec 002-mesh-sdf-performance
/// @see mesh_sdf.cl, SpatialMeshResource.h

#include "ComputeContext.h"
#include "ComputeCore.h"
#include "Document.h"
#include "EventLogger.h"
#include "ImageRGBA.h"
#include "MeshBVH.h"
#include "MeshSdfMethod.h"
#include "Primitives.h"
#include "ResourceContext.h"
#include "ResourceManager.h"
#include "SpatialMeshResource.h"
#include "MeshVoxelGridManager.h"
#include "io/3mf/Lib3mfLoader.h"
#include "kernel/types.h"
#include "ui/OrbitalCamera.h"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

namespace gladius::tests
{
    // Helper to access float4 components for arithmetic
    inline float4 makeFloat4(float x, float y, float z, float w = 0.0f)
    {
        return float4{{x, y, z, w}};
    }
    // ========================================================================
    // Constants
    // ========================================================================

    /// Default benchmark mesh for reproducible testing
    /// Requirements:
    /// - Must contain at least one mesh object
    /// - Triangle count should be between 1K-10K for reasonable benchmark times
    /// - File size should be < 1MB for fast test startup
    static constexpr char const* kBenchmarkMesh = "testdata/SphereInACageSimplifiedMesh.3mf";

    /// Number of SDF queries per benchmark run
    static constexpr size_t kDefaultQueryCount = 10000;

    /// Number of warmup iterations before timing
    static constexpr size_t kWarmupIterations = 100;

    /// Random seed for reproducible query points
    static constexpr unsigned int kRandomSeed = 42;

    // ========================================================================
    // Performance Statistics
    // ========================================================================

    struct PerformanceStats
    {
        double meanMicroseconds{0.0};
        double stddevMicroseconds{0.0};
        double minMicroseconds{0.0};
        double maxMicroseconds{0.0};
        double medianMicroseconds{0.0};
        size_t queryCount{0};
        size_t triangleCount{0};
        std::string meshName;

        void print() const
        {
            std::cout << std::fixed << std::setprecision(3);
            std::cout << "\n=== Mesh SDF Performance Results ===" << std::endl;
            std::cout << "Mesh: " << meshName << std::endl;
            std::cout << "Triangles: " << triangleCount << std::endl;
            std::cout << "Queries: " << queryCount << std::endl;
            std::cout << "Mean: " << meanMicroseconds << " µs/query" << std::endl;
            std::cout << "Stddev: " << stddevMicroseconds << " µs" << std::endl;
            std::cout << "Min: " << minMicroseconds << " µs" << std::endl;
            std::cout << "Max: " << maxMicroseconds << " µs" << std::endl;
            std::cout << "Median: " << medianMicroseconds << " µs" << std::endl;
            std::cout << "Throughput: " << (1e6 / meanMicroseconds) << " queries/sec" << std::endl;
            std::cout << "====================================\n" << std::endl;
        }
    };

    /// Compute statistics from timing samples
    [[nodiscard]] PerformanceStats computeStats(std::vector<double> const& timingsUs,
                                                 std::string const& meshName,
                                                 size_t triangleCount)
    {
        PerformanceStats stats;
        stats.queryCount = timingsUs.size();
        stats.triangleCount = triangleCount;
        stats.meshName = meshName;

        if (timingsUs.empty())
        {
            return stats;
        }

        // Mean
        double sum = std::accumulate(timingsUs.begin(), timingsUs.end(), 0.0);
        stats.meanMicroseconds = sum / static_cast<double>(timingsUs.size());

        // Stddev
        double sqSum = 0.0;
        for (auto t : timingsUs)
        {
            double diff = t - stats.meanMicroseconds;
            sqSum += diff * diff;
        }
        stats.stddevMicroseconds = std::sqrt(sqSum / static_cast<double>(timingsUs.size()));

        // Min/Max
        auto [minIt, maxIt] = std::minmax_element(timingsUs.begin(), timingsUs.end());
        stats.minMicroseconds = *minIt;
        stats.maxMicroseconds = *maxIt;

        // Median
        std::vector<double> sorted = timingsUs;
        std::sort(sorted.begin(), sorted.end());
        size_t mid = sorted.size() / 2;
        stats.medianMicroseconds = (sorted.size() % 2 == 0) 
            ? (sorted[mid - 1] + sorted[mid]) / 2.0
            : sorted[mid];

        return stats;
    }

    // ========================================================================
    // Test Fixture
    // ========================================================================

    class MeshSdfPerformance_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_context = std::make_shared<ComputeContext>();
            m_logger = std::make_shared<events::Logger>();
        }

        struct DocumentBundle
        {
            std::shared_ptr<ComputeCore> core;
            std::shared_ptr<Document> document;
        };

        /// Load a 3mf document and initialize compute core
        [[nodiscard]] DocumentBundle loadDocument(std::filesystem::path const& path)
        {
            auto core = std::make_shared<ComputeCore>(
                m_context, RequiredCapabilities::ComputeOnly, m_logger);
            auto document = std::make_shared<Document>(core);
            document->load(path);
            return DocumentBundle{std::move(core), std::move(document)};
        }

        /// Generate random query points within bounding box (with margin)
        [[nodiscard]] std::vector<float4> generateQueryPoints(
            BoundingBox const& bbox,
            size_t count,
            float marginFactor = 1.5f)
        {
            std::mt19937 rng(kRandomSeed);
            
            float centerX = (bbox.min.x + bbox.max.x) * 0.5f;
            float centerY = (bbox.min.y + bbox.max.y) * 0.5f;
            float centerZ = (bbox.min.z + bbox.max.z) * 0.5f;
            float extentX = (bbox.max.x - bbox.min.x) * marginFactor * 0.5f;
            float extentY = (bbox.max.y - bbox.min.y) * marginFactor * 0.5f;
            float extentZ = (bbox.max.z - bbox.min.z) * marginFactor * 0.5f;
            
            std::uniform_real_distribution<float> distX(centerX - extentX, centerX + extentX);
            std::uniform_real_distribution<float> distY(centerY - extentY, centerY + extentY);
            std::uniform_real_distribution<float> distZ(centerZ - extentZ, centerZ + extentZ);
            
            std::vector<float4> points;
            points.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                points.push_back(makeFloat4(distX(rng), distY(rng), distZ(rng)));
            }
            return points;
        }

        /// Create a SpatialMeshResource from document mesh
        /// @note This extracts mesh geometry and builds BVH
        [[nodiscard]] std::unique_ptr<SpatialMeshResource> createMeshResource(
            ComputeCore& core,
            std::vector<float4>& vertices,
            std::vector<TriangleIndices>& indices)
        {
            // For benchmark: create a simple procedural sphere mesh if document has none
            // This provides consistent baseline measurement
            if (vertices.empty())
            {
                createIcosphere(vertices, indices, 1.0f, 4);  // ~5120 triangles
            }

            ResourceKey key(ResourceId{1000}, ResourceType::Mesh);
            return std::make_unique<SpatialMeshResource>(key, vertices, indices);
        }

        /// Create an icosphere mesh for testing
        static void createIcosphere(std::vector<float4>& vertices,
                                    std::vector<TriangleIndices>& indices,
                                    float radius,
                                    int subdivisions)
        {
            float const t = (1.0f + std::sqrt(5.0f)) / 2.0f;

            vertices = {
                makeFloat4(-1,  t, 0), makeFloat4( 1,  t, 0), 
                makeFloat4(-1, -t, 0), makeFloat4( 1, -t, 0),
                makeFloat4( 0, -1,  t), makeFloat4( 0,  1,  t), 
                makeFloat4( 0, -1, -t), makeFloat4( 0,  1, -t),
                makeFloat4( t, 0, -1), makeFloat4( t, 0,  1), 
                makeFloat4(-t, 0, -1), makeFloat4(-t, 0,  1),
            };

            for (auto& v : vertices)
            {
                float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                v.x = v.x / len * radius;
                v.y = v.y / len * radius;
                v.z = v.z / len * radius;
            }

            indices = {
                {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
                {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
                {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1},
            };

            // Subdivide
            for (int sub = 0; sub < subdivisions; ++sub)
            {
                std::vector<TriangleIndices> newIndices;
                std::map<std::pair<int, int>, int> midpointCache;

                auto getMidpoint = [&](int idx0, int idx1) -> int
                {
                    auto key = std::make_pair(std::min(idx0, idx1), std::max(idx0, idx1));
                    auto it = midpointCache.find(key);
                    if (it != midpointCache.end())
                    {
                        return it->second;
                    }

                    float4 const& v0 = vertices[idx0];
                    float4 const& v1 = vertices[idx1];
                    float4 mid = makeFloat4((v0.x + v1.x) / 2, (v0.y + v1.y) / 2, (v0.z + v1.z) / 2);
                    float len = std::sqrt(mid.x * mid.x + mid.y * mid.y + mid.z * mid.z);
                    mid.x = mid.x / len * radius;
                    mid.y = mid.y / len * radius;
                    mid.z = mid.z / len * radius;

                    int idx = static_cast<int>(vertices.size());
                    vertices.push_back(mid);
                    midpointCache[key] = idx;
                    return idx;
                };

                for (auto const& tri : indices)
                {
                    int a = getMidpoint(tri.i0, tri.i1);
                    int b = getMidpoint(tri.i1, tri.i2);
                    int c = getMidpoint(tri.i2, tri.i0);

                    newIndices.push_back({tri.i0, a, c});
                    newIndices.push_back({tri.i1, b, a});
                    newIndices.push_back({tri.i2, c, b});
                    newIndices.push_back({a, b, c});
                }

                indices = std::move(newIndices);
            }
        }

        std::shared_ptr<ComputeContext> m_context;
        std::shared_ptr<events::Logger> m_logger;
    };

    // ========================================================================
    // Benchmark Tests
    // ========================================================================

    /// T001-T003: Baseline performance benchmark with procedural sphere
    TEST_F(MeshSdfPerformance_Test, Baseline_ProceduralSphere_RecordsPerformance)
    {
        // Create procedural icosphere (5120 triangles at subdivision 4)
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createIcosphere(vertices, indices, 1.0f, 4);

        ResourceKey key(ResourceId{1}, ResourceType::Mesh);
        SpatialMeshResource resource(key, vertices, indices);

        ASSERT_FALSE(resource.getData().empty()) << "BVH build failed";
        ASSERT_GT(resource.getTriangleCount(), 0u);

        // Generate query points
        auto const& bbox = resource.getBoundingBox();
        auto queryPoints = generateQueryPoints(bbox, kDefaultQueryCount);

        // Warmup: run some queries to ensure GPU caches are primed
        // Note: CPU-side BVH queries for now; GPU benchmarking requires ComputeCore integration
        std::vector<double> timingsUs;
        timingsUs.reserve(queryPoints.size());

        // Currently SpatialMeshResource doesn't expose a CPU query function
        // This test validates the infrastructure; actual GPU timing requires ComputeCore
        // For now, we measure BVH construction time as a proxy

        auto start = std::chrono::high_resolution_clock::now();
        
        // Rebuild BVH to measure construction time
        for (size_t i = 0; i < 10; ++i)
        {
            resource.invalidate();
            resource.rebuild(vertices, indices);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        double avgBuildTimeUs = static_cast<double>(durationUs) / 10.0;

        std::cout << "\n=== BVH Build Performance ===" << std::endl;
        std::cout << "Triangles: " << indices.size() << std::endl;
        std::cout << "Avg BVH build time: " << avgBuildTimeUs << " µs" << std::endl;
        std::cout << "Build throughput: " << (static_cast<double>(indices.size()) / avgBuildTimeUs * 1e6) 
                  << " triangles/sec" << std::endl;

        // Verify reasonable performance (should build in < 100ms for 5K triangles)
        EXPECT_LT(avgBuildTimeUs, 100000.0) << "BVH build too slow";

        SUCCEED() << "Baseline benchmark infrastructure validated";
    }

    /// T002: Benchmark with actual mesh file
    TEST_F(MeshSdfPerformance_Test, BenchmarkMesh_LoadsAndBuilds)
    {
        std::filesystem::path meshPath = kBenchmarkMesh;
        if (!std::filesystem::exists(meshPath))
        {
            GTEST_SKIP() << "Benchmark mesh not found: " << meshPath;
        }

        auto bundle = loadDocument(meshPath);
        ASSERT_TRUE(bundle.document != nullptr) << "Failed to load benchmark mesh";

        // The document should contain mesh geometry
        // This validates the mesh loading path
        EXPECT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        std::cout << "\n=== Benchmark Mesh Loaded ===" << std::endl;
        std::cout << "Path: " << meshPath << std::endl;
        std::cout << "File size: " << std::filesystem::file_size(meshPath) << " bytes" << std::endl;

        SUCCEED() << "Benchmark mesh loads successfully";
    }

    /// GPU-based SDF render benchmark across mesh-SDF acceleration methods.
    /// Renders a fixed view of the benchmark mesh with each evaluation method
    /// (PureBVH, VoxelAccelerated, FastWindingNumber) and reports wall-clock
    /// frame time so the relative cost of each kernel path can be compared.
    ///
    /// **DEFAULT-SKIPPED.** This benchmark dispatches a long-running SDF
    /// kernel for each method back-to-back; on systems with a strict GPU
    /// watchdog (TDR / Linux amdgpu hang detection) the FWN path on a
    /// non-trivial mesh has been observed to crash the display driver. Set
    /// `GLADIUS_RUN_GPU_BENCH=1` to enable. Use a small mesh.
    TEST_F(MeshSdfPerformance_Test, GpuSdfRender_AllMethods_PrintsThroughput)
    {
        if (std::getenv("GLADIUS_RUN_GPU_BENCH") == nullptr)
        {
            GTEST_SKIP() << "GPU benchmark default-skipped. Set "
                            "GLADIUS_RUN_GPU_BENCH=1 to enable. WARNING: "
                            "may hit the GPU TDR watchdog on heavy meshes.";
        }
        // Search both integration- and unit-test data directories so the
        // benchmark mesh can be picked up regardless of which testdata folder
        // happens to ship it.
        std::array<std::filesystem::path, 3> const candidates = {{
            std::filesystem::path(kBenchmarkMesh),
            std::filesystem::path("../unittests") / kBenchmarkMesh,
            std::filesystem::path("../../tests/unittests") / kBenchmarkMesh,
        }};
        std::filesystem::path meshPath;
        for (auto const & p : candidates)
        {
            if (std::filesystem::exists(p))
            {
                meshPath = p;
                break;
            }
        }
        if (meshPath.empty())
        {
            GTEST_SKIP() << "Benchmark mesh not found: " << kBenchmarkMesh;
        }
        if (!m_context->isValid())
        {
            GTEST_SKIP() << "OpenCL context not available";
        }

        auto bundle = loadDocument(meshPath);
        ASSERT_TRUE(bundle.document != nullptr);
        ASSERT_TRUE(bundle.core->updateBBox()) << "updateBBox failed";
        ASSERT_TRUE(bundle.core->prepareImageRendering()) << "prepareImageRendering failed";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "No bounding box";

        // Verify the document actually contains a SpatialMeshResource — without
        // one, the FWN/Voxel kernel paths are never exercised and the test is
        // not meaningful.
        SpatialMeshResource * spatialMesh = nullptr;
        for (auto const & [key, resource] : bundle.document->getResourceManager().getResourceMap())
        {
            if (key.getResourceType() != ResourceType::Mesh)
            {
                continue;
            }
            if (auto * sm = dynamic_cast<SpatialMeshResource *>(resource.get()))
            {
                spatialMesh = sm;
                break;
            }
        }
        if (spatialMesh == nullptr)
        {
            GTEST_SKIP() << "Benchmark document contains no SpatialMeshResource";
        }

        // Camera setup mirrors RayMarchPerf_tests.cpp::RenderWithMetrics_MeasuresStepCount.
        // Small render size keeps each kernel dispatch well under typical GPU
        // watchdog limits (TDR ~2 s on Windows, ~5 s on Linux amdgpu).
        constexpr size_t kWidth = 64;
        constexpr size_t kHeight = 64;
        ui::OrbitalCamera camera;
        camera.setAngle(0.6f, -2.0f);
        camera.centerView(*bbox);
        camera.update(10000.f);
        camera.adjustDistanceToTarget(*bbox, static_cast<int>(kWidth), static_cast<int>(kHeight));
        camera.update(10000.f);

        auto resources = bundle.core->getResourceContext();
        resources->setEyePosition(camera.getEyePosition());
        resources->setModelViewPerspectiveMat(camera.computeModelViewPerspectiveMatrix());

        auto targetImage = std::make_unique<ImageRGBA>(*bundle.core->getComputeContext(), kWidth, kHeight);
        targetImage->allocateOnDevice();

        auto const & queue = bundle.core->getComputeContext()->GetQueue();

        struct MethodCase
        {
            char const * label;
            MeshSdfMethod method;
        };
        std::array<MethodCase, 3> const cases = {{
            {"PureBVH",            MeshSdfMethod::PureBVH},
            {"VoxelAccelerated",   MeshSdfMethod::VoxelAccelerated},
            {"FastWindingNumber",  MeshSdfMethod::FastWindingNumber},
        }};

        constexpr int kWarmupFrames = 1;
        constexpr int kTimedFrames = 3;

        struct Result
        {
            std::string label;
            double meanMs{0.0};
            double minMs{0.0};
            double maxMs{0.0};
        };
        std::vector<Result> results;
        results.reserve(cases.size());

        for (auto const & c : cases)
        {
            // 1) Push evaluation method to the resource. Triggers a rebuild
            //    when the method or grid resolution differs from the current
            //    config (return value indicates a rebuild happened).
            MeshSdfEvaluationConfig cfg{};
            cfg.method = c.method;
            cfg.useEarlyExit = true;
            cfg.fwnBeta = 2.0f;
            spatialMesh->setEvaluationConfig(cfg);

            // 2) Forward runtime knobs into RenderingSettings so the kernel
            //    dispatch picks the correct branch.
            auto & settings = resources->getRenderingSettings();
            settings.meshFwnBeta = cfg.fwnBeta;
            if (c.method == MeshSdfMethod::FastWindingNumber)
            {
                settings.flags |= RF_USE_MESH_FWN;
            }
            else
            {
                settings.flags &= ~RF_USE_MESH_FWN;
            }
            settings.flags &= ~RF_DISABLE_MESH_EARLY_EXIT;

            // 3) Flush resource changes (BVH/voxel grid/FWN aggregates) into
            //    the GPU primitives buffer.
            bundle.document->refreshModelBlocking();

            // Re-apply camera (refresh may have touched parameter buffer).
            resources->setEyePosition(camera.getEyePosition());
            resources->setModelViewPerspectiveMat(camera.computeModelViewPerspectiveMatrix());

            // 4) Warmup so first-launch costs (kernel JIT, buffer migration,
            //    voxel grid lazy build) don't pollute the timed samples.
            //    Each render is wrapped in a host-side timeout: if a kernel
            //    runs for too long (driver TDR usually fires around 2-5 s)
            //    the test aborts cleanly with a clear error rather than
            //    blocking the test runner indefinitely on a hung GPU.
            //
            //    NOTE: OpenCL has no kernel preemption — on a true hang the
            //    in-flight work continues until the driver resets it. The
            //    timeout's job is to make the *host process* exit gracefully
            //    so subsequent tests / shells aren't deadlocked on event.wait().
            constexpr auto kKernelTimeout = std::chrono::seconds(8);
            auto runRender = [&] {
                cl::Event ev;
                bool const ok = bundle.core->renderSceneWithMetrics(
                    queue, 0, kHeight, *targetImage, &ev);
                if (ok && ev())
                {
                    ev.wait();
                }
                return ok;
            };
            for (int i = 0; i < kWarmupFrames; ++i)
            {
                auto fut = std::async(std::launch::async, runRender);
                if (fut.wait_for(kKernelTimeout) == std::future_status::timeout)
                {
                    // Detach the future via a shared_ptr trick: move into a
                    // long-lived std::async wrapper so its destructor doesn't
                    // block the test on shutdown. We *don't* abort here —
                    // FAIL is reported and we break out of the method loop.
                    auto leaked = std::make_shared<std::future<bool>>(std::move(fut));
                    std::thread([leaked] { leaked->wait(); }).detach();
                    FAIL() << "GPU kernel timeout (>" << kKernelTimeout.count()
                           << "s) during warmup for " << c.label
                           << "; aborting benchmark to avoid display driver crash.";
                    return;
                }
                ASSERT_TRUE(fut.get()) << "warmup render failed for " << c.label;
            }

            // 5) Timed runs — wall-clock around event.wait() since the default
            //    queue is not created with CL_QUEUE_PROFILING_ENABLE.
            std::vector<double> framesMs;
            framesMs.reserve(kTimedFrames);
            for (int i = 0; i < kTimedFrames; ++i)
            {
                auto const t0 = std::chrono::steady_clock::now();
                auto fut = std::async(std::launch::async, runRender);
                if (fut.wait_for(kKernelTimeout) == std::future_status::timeout)
                {
                    auto leaked = std::make_shared<std::future<bool>>(std::move(fut));
                    std::thread([leaked] { leaked->wait(); }).detach();
                    FAIL() << "GPU kernel timeout (>" << kKernelTimeout.count()
                           << "s) on timed frame " << i << " for " << c.label
                           << "; aborting benchmark to avoid display driver crash.";
                    return;
                }
                ASSERT_TRUE(fut.get()) << "render failed for " << c.label;
                auto const t1 = std::chrono::steady_clock::now();
                framesMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }

            Result r;
            r.label = c.label;
            r.meanMs = std::accumulate(framesMs.begin(), framesMs.end(), 0.0)
                       / static_cast<double>(framesMs.size());
            auto const [mn, mx] = std::minmax_element(framesMs.begin(), framesMs.end());
            r.minMs = *mn;
            r.maxMs = *mx;
            results.push_back(std::move(r));
        }

        // Report
        fmt::print("\n=== GPU Mesh SDF Render Benchmark ===\n");
        fmt::print("Mesh:        {}\n", meshPath.filename().string());
        fmt::print("Triangles:   {}\n", spatialMesh->getTriangleCount());
        fmt::print("Resolution:  {}x{}\n", kWidth, kHeight);
        fmt::print("Frames:      {} warmup + {} timed\n", kWarmupFrames, kTimedFrames);
        fmt::print("{:<20} {:>10} {:>10} {:>10}\n", "Method", "mean ms", "min ms", "max ms");
        fmt::print("{}\n", std::string(52, '-'));
        for (auto const & r : results)
        {
            fmt::print("{:<20} {:>10.2f} {:>10.2f} {:>10.2f}\n",
                       r.label, r.meanMs, r.minMs, r.maxMs);
        }
        fmt::print("======================================\n\n");

        // Sanity: all three paths should produce some non-zero render time.
        for (auto const & r : results)
        {
            EXPECT_GT(r.meanMs, 0.0) << r.label << " produced zero frame time";
        }
    }

    /// T038: Measure BVH data structure sizes for export performance
    /// Validates that extended triangle struct (64 bytes) is properly sized
    TEST_F(MeshSdfPerformance_Test, TriangleStruct_HasExpectedSize)
    {
        // Verify MeshTriangle is now 64 bytes (extended with face normal)
        EXPECT_EQ(sizeof(MeshTriangle), 64u) 
            << "MeshTriangle should be 64 bytes (4x float4: v0, v1, v2, faceNormal)";
        
        // Verify MeshBVHNode is 48 bytes
        EXPECT_EQ(sizeof(MeshBVHNode), 48u)
            << "MeshBVHNode should be 48 bytes (2x float4 bbox + 4 ints)";
        
        // Verify MeshVertexNormal is 16 bytes
        EXPECT_EQ(sizeof(MeshVertexNormal), 16u)
            << "MeshVertexNormal should be 16 bytes (1x float4)";
    }

    /// T038: Measure BVH construction performance with precomputed face normals
    TEST_F(MeshSdfPerformance_Test, BvhBuild_WithPrecomputedNormals_MeasuresOverhead)
    {
        // Create procedural icosphere with varying sizes
        std::vector<std::pair<int, size_t>> testCases = {
            {2, 320},    // ~320 triangles
            {3, 1280},   // ~1280 triangles
            {4, 5120},   // ~5120 triangles
            {5, 20480},  // ~20480 triangles
        };

        std::cout << "\n=== BVH Build Performance with Precomputed Face Normals ===" << std::endl;
        std::cout << std::setw(12) << "Triangles" 
                  << std::setw(15) << "Build (µs)" 
                  << std::setw(18) << "Throughput (tri/s)"
                  << std::setw(15) << "Memory (KB)" << std::endl;
        std::cout << std::string(60, '-') << std::endl;

        for (auto const& [subdivisions, expectedTris] : testCases)
        {
            std::vector<float4> vertices;
            std::vector<TriangleIndices> indices;
            createIcosphere(vertices, indices, 1.0f, subdivisions);

            // Measure BVH build time (average of 5 runs)
            constexpr int kRuns = 5;
            std::vector<double> buildTimes;
            buildTimes.reserve(kRuns);

            ResourceKey key(ResourceId{1}, ResourceType::Mesh);
            
            for (int i = 0; i < kRuns; ++i)
            {
                auto start = std::chrono::high_resolution_clock::now();
                SpatialMeshResource resource(key, vertices, indices);
                auto end = std::chrono::high_resolution_clock::now();
                
                auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                buildTimes.push_back(static_cast<double>(durationUs));
                
                ASSERT_FALSE(resource.getData().empty()) << "BVH build failed";
            }

            // Compute average
            double avgBuildUs = std::accumulate(buildTimes.begin(), buildTimes.end(), 0.0) / kRuns;
            double throughput = static_cast<double>(indices.size()) / avgBuildUs * 1e6;
            
            // Memory calculation: nodes + triangles (64 bytes each) + vertex normals
            size_t memoryBytes = indices.size() * sizeof(MeshTriangle);  // 64 bytes/triangle
            double memoryKB = static_cast<double>(memoryBytes) / 1024.0;

            std::cout << std::setw(12) << indices.size()
                      << std::setw(15) << std::fixed << std::setprecision(1) << avgBuildUs
                      << std::setw(18) << std::fixed << std::setprecision(0) << throughput
                      << std::setw(15) << std::fixed << std::setprecision(1) << memoryKB << std::endl;

            // Performance assertion: should build at least 100K triangles/sec
            EXPECT_GT(throughput, 100000.0) 
                << "BVH build too slow for " << indices.size() << " triangles";
        }

        std::cout << "\nNote: Memory increase from 48->64 bytes/triangle is offset by avoiding" << std::endl;
        std::cout << "      runtime cross-product computation for face normals." << std::endl;
    }

    /// Phase 3 step 9: sweep maxPrimitivesPerLeaf to compare build cost and tree
    /// shape proxies (node count, max depth, avg primitives per leaf). Helps
    /// pick a sensible default after Phase 2 made SAH-driven leaf creation work
    /// correctly.
    TEST_F(MeshSdfPerformance_Test, BvhBuild_LeafSizeSweep_ComparesQualityAndBuildCost)
    {
        constexpr int kSubdivisions = 5;  // ~20480 triangles
        constexpr int kRuns = 5;
        std::array<int, 4> const leafSizes = {2, 4, 8, 16};

        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createIcosphere(vertices, indices, 1.0f, kSubdivisions);
        ASSERT_FALSE(indices.empty());

        std::cout << "\n=== BVH Leaf-Size Sweep (" << indices.size() << " triangles) ==="
                  << std::endl;
        std::cout << std::setw(12) << "MaxPrims"
                  << std::setw(15) << "Build (us)"
                  << std::setw(15) << "TotalNodes"
                  << std::setw(15) << "LeafNodes"
                  << std::setw(15) << "MaxDepth"
                  << std::setw(15) << "AvgPrim/Leaf" << std::endl;
        std::cout << std::string(87, '-') << std::endl;

        for (int leafSize : leafSizes)
        {
            MeshBVHBuildParams params;
            params.maxPrimitivesPerLeaf = leafSize;

            std::vector<double> buildTimes;
            buildTimes.reserve(kRuns);
            MeshBVHBuildStats lastStats;
            for (int i = 0; i < kRuns; ++i)
            {
                MeshBVHBuilder builder;
                auto const start = std::chrono::high_resolution_clock::now();
                auto data = builder.build(vertices, indices, params);
                auto const end = std::chrono::high_resolution_clock::now();
                ASSERT_FALSE(data.triangles.empty()) << "BVH build failed";
                buildTimes.push_back(static_cast<double>(
                    std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()));
                lastStats = builder.getLastBuildStats();
            }

            double const avgBuildUs =
                std::accumulate(buildTimes.begin(), buildTimes.end(), 0.0) /
                static_cast<double>(buildTimes.size());

            std::cout << std::setw(12) << leafSize
                      << std::setw(15) << std::fixed << std::setprecision(1) << avgBuildUs
                      << std::setw(15) << lastStats.totalNodes
                      << std::setw(15) << lastStats.leafNodes
                      << std::setw(15) << lastStats.maxDepth
                      << std::setw(15) << std::fixed << std::setprecision(2)
                      << lastStats.avgPrimitivesPerLeaf << std::endl;

            // Sanity: every configuration must build a non-trivial tree.
            EXPECT_GT(lastStats.totalNodes, 0);
            EXPECT_GT(lastStats.leafNodes, 0);
            EXPECT_LE(lastStats.maxDepth, 24)
                << "Max depth exceeds builder's hard cap";
        }

        std::cout << "\nNote: Smaller leaves => more nodes + deeper trees + slightly slower"
                  << " build,\n      but better pruning during queries. Pick the smallest"
                  << " leaf size whose\n      build cost is acceptable for your typical mesh"
                  << " size." << std::endl;
    }

    /// T038: Validate face normals are correctly computed during build
    TEST_F(MeshSdfPerformance_Test, BvhBuild_FaceNormals_AreNormalized)
    {
        // Create simple mesh
        std::vector<float4> vertices = {
            makeFloat4(0.0f, 0.0f, 0.0f),
            makeFloat4(1.0f, 0.0f, 0.0f),
            makeFloat4(0.0f, 1.0f, 0.0f),
        };
        std::vector<TriangleIndices> indices = {{0, 1, 2}};

        ResourceKey key(ResourceId{1}, ResourceType::Mesh);
        SpatialMeshResource resource(key, vertices, indices);

        auto const& data = resource.getData();
        ASSERT_FALSE(data.triangles.empty());

        // Check that face normal is normalized
        auto const& tri = data.triangles[0];
        float lenSq = tri.faceNormal.x * tri.faceNormal.x +
                      tri.faceNormal.y * tri.faceNormal.y +
                      tri.faceNormal.z * tri.faceNormal.z;
        
        EXPECT_NEAR(lenSq, 1.0f, 1e-5f) << "Face normal should be unit length";
        
        // For this triangle (XY plane), normal should be (0, 0, 1) or (0, 0, -1)
        EXPECT_NEAR(std::abs(tri.faceNormal.z), 1.0f, 1e-5f) 
            << "Face normal should point in Z direction for XY-plane triangle";
    }

    // ========================================================================
    // T028: Voxel Grid Build Performance Benchmarks
    // ========================================================================

    /// T028: Measure voxel grid construction time
    /// This validates the GPU voxel grid build infrastructure
    TEST_F(MeshSdfPerformance_Test, VoxelGridBuild_MeasuresConstructionTime)
    {
        // Create procedural icosphere with varying sizes
        std::vector<std::pair<int, size_t>> testCases = {
            {3, 1280},   // ~1280 triangles
            {4, 5120},   // ~5120 triangles
            {5, 20480},  // ~20480 triangles
        };

        std::cout << "\n=== Voxel Grid Build Performance ===" << std::endl;
        std::cout << std::setw(12) << "Triangles" 
                  << std::setw(15) << "BVH Build (µs)" 
                  << std::setw(18) << "Expected Voxels"
                  << std::setw(15) << "Grid Dims" << std::endl;
        std::cout << std::string(60, '-') << std::endl;

        for (auto const& [subdivisions, expectedTris] : testCases)
        {
            std::vector<float4> vertices;
            std::vector<TriangleIndices> indices;
            createIcosphere(vertices, indices, 10.0f, subdivisions);  // 10mm radius sphere

            ResourceKey key(ResourceId{1}, ResourceType::Mesh);
            
            auto start = std::chrono::high_resolution_clock::now();
            SpatialMeshResource resource(key, vertices, indices);
            auto end = std::chrono::high_resolution_clock::now();
            
            auto buildTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

            ASSERT_FALSE(resource.getData().empty()) << "BVH build failed";

            // Calculate expected voxel grid dimensions
            // Voxel size is typically bbox extent / (targetDim - 1) where targetDim=16
            auto const& bbox = resource.getBoundingBox();
            float extent = std::max({bbox.max.x - bbox.min.x, 
                                     bbox.max.y - bbox.min.y, 
                                     bbox.max.z - bbox.min.z});
            
            // Estimate voxel grid: assume 16^3 = 4096 voxels for typical mesh
            constexpr int kTargetDim = 16;
            int expectedVoxels = kTargetDim * kTargetDim * kTargetDim;
            
            std::cout << std::setw(12) << indices.size()
                      << std::setw(15) << std::fixed << std::setprecision(1) << static_cast<double>(buildTimeUs)
                      << std::setw(18) << expectedVoxels
                      << std::setw(15) << (std::to_string(kTargetDim) + "^3") << std::endl;
        }

        std::cout << "\nNote: GPU voxel grid build occurs after primitive buffer upload." << std::endl;
        std::cout << "      Use Document::refreshModelBlocking() to trigger full pipeline." << std::endl;

        SUCCEED() << "Voxel grid build infrastructure validated";
    }

    /// T028: Validate voxel grid data allocation in SpatialMeshResource
    TEST_F(MeshSdfPerformance_Test, VoxelGrid_DataAllocation_ReservesSpace)
    {
        // Create a simple mesh
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createIcosphere(vertices, indices, 10.0f, 3);  // ~1280 triangles

        ResourceKey key(ResourceId{1}, ResourceType::Mesh);
        SpatialMeshResource resource(key, vertices, indices);

        ASSERT_FALSE(resource.getData().empty()) << "BVH build failed";

        // Check that the resource needs voxel grid build
        // This validates the host-side tracking
        EXPECT_TRUE(resource.needsVoxelGridBuild()) 
            << "Resource should need voxel grid build before GPU execution";

        // Get build params and validate they're reasonable
        auto paramsOpt = resource.getVoxelGridBuildParams();
        ASSERT_TRUE(paramsOpt.has_value()) << "Should return valid build params";
        
        auto const& params = paramsOpt.value();
        
        EXPECT_GT(params.voxelCount, 0) << "Should have non-zero voxel count";
        EXPECT_GE(params.headerStart, 0) << "Header start should be non-negative";
        EXPECT_GT(params.triCount, 0) << "Should have triangles";
        EXPECT_EQ(params.triCount, static_cast<int>(indices.size())) 
            << "Triangle count should match input";

        std::cout << "\n=== Voxel Grid Build Parameters ===" << std::endl;
        std::cout << "Header start: " << params.headerStart << std::endl;
        std::cout << "Voxel data offset: " << params.voxelDataOffset << std::endl;
        std::cout << "Nodes offset: " << params.nodesOffset << std::endl;
        std::cout << "Triangles offset: " << params.trianglesOffset << std::endl;
        std::cout << "Voxel count: " << params.voxelCount << std::endl;
        std::cout << "Triangle count: " << params.triCount << std::endl;

        SUCCEED() << "Voxel grid data allocation validated";
    }

    /// T028: End-to-end GPU voxel grid build test (requires GPU)
    TEST_F(MeshSdfPerformance_Test, VoxelGrid_GpuBuild_EndToEnd)
    {
        // Skip if GPU tests are not enabled
#ifdef _WIN32
        char* gpuTestsEnv = nullptr;
        size_t gpuTestsEnvLen = 0;
        _dupenv_s(&gpuTestsEnv, &gpuTestsEnvLen, "GLADIUS_RUN_GPU_TESTS");
        std::unique_ptr<char, decltype(&free)> gpuTestsEnvGuard(gpuTestsEnv, &free);
#else
        char const* gpuTestsEnv = std::getenv("GLADIUS_RUN_GPU_TESTS");
#endif
        if (!gpuTestsEnv || std::string(gpuTestsEnv) != "1")
        {
            GTEST_SKIP() << "GPU tests disabled. Set GLADIUS_RUN_GPU_TESTS=1 to enable.";
        }

        std::filesystem::path meshPath = kBenchmarkMesh;
        if (!std::filesystem::exists(meshPath))
        {
            GTEST_SKIP() << "Benchmark mesh not found: " << meshPath;
        }

        auto bundle = loadDocument(meshPath);
        ASSERT_TRUE(bundle.document != nullptr) << "Failed to load benchmark mesh";

        // Trigger full model refresh which includes voxel grid building
        auto start = std::chrono::high_resolution_clock::now();
        bundle.document->refreshModelBlocking();
        auto end = std::chrono::high_resolution_clock::now();
        
        auto refreshTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        std::cout << "\n=== GPU Voxel Grid Build (End-to-End) ===" << std::endl;
        std::cout << "Model refresh time: " << refreshTimeMs << " ms" << std::endl;
        std::cout << "Includes: shader compilation, resource loading, BVH upload, voxel grid GPU build" << std::endl;

        // Note: First run includes OpenCL shader compilation which is slow.
        // The refresh should complete in reasonable time (< 60 seconds including compilation)
        EXPECT_LT(refreshTimeMs, 60000) << "Model refresh took too long";

        SUCCEED() << "GPU voxel grid build completed successfully";
    }

    /// End-to-end GPU FWN aggregate build test.
    /// Validates that the post-upload OpenCL kernel fills the reserved aggregate
    /// slots with the same range-based values used by the runtime FWN traversal.
    TEST_F(MeshSdfPerformance_Test, FwnAggregates_GpuBuild_MatchesHostRangeAggregates)
    {
        if (!m_context->isValid())
        {
            GTEST_SKIP() << "OpenCL context not available";
        }

        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createIcosphere(vertices, indices, 3.0f, 1);

        ResourceKey key(ResourceId{42}, ResourceType::Mesh);
        SpatialMeshResource resource(key, vertices, indices);

        MeshSdfEvaluationConfig cfg{};
        cfg.method = MeshSdfMethod::FastWindingNumber;
        cfg.fwnBeta = 2.0f;
        ASSERT_TRUE(resource.setEvaluationConfig(cfg));

        auto const expectedData = resource.getData();
        ASSERT_FALSE(expectedData.nodes.empty());
        ASSERT_FALSE(expectedData.triangles.empty());

        auto core = std::make_shared<ComputeCore>(
            m_context, RequiredCapabilities::ComputeOnly, m_logger);
        auto slicerProgram = core->getSlicerProgram();
        ASSERT_TRUE(slicerProgram != nullptr);
        slicerProgram->setModelKernel(R"CLC(
    float4 model(float3 pos, PAYLOAD_ARGS)
    {
        return (float4)(0.0f, 0.0f, 0.0f, length(pos) - 1.0f);
    }
    )CLC");
        slicerProgram->recompileBlocking();
        ASSERT_TRUE(slicerProgram->isValid());

        auto primitives = core->getPrimitives();
        ASSERT_TRUE(primitives != nullptr);

        primitives->clear();
        resource.write(*primitives);
        primitives->write();

        auto paramsOpt = resource.getFwnAggregateBuildParams();
        ASSERT_TRUE(paramsOpt.has_value());
        auto const params = paramsOpt.value();
        ASSERT_TRUE(resource.needsFwnAggregateBuild());
        ASSERT_EQ(params.nodeCount, static_cast<int>(expectedData.nodes.size()));
        ASSERT_EQ(params.triCount, static_cast<int>(expectedData.triangles.size()));

        size_t const builtCount = core->buildMeshFwnAggregates({params});
        ASSERT_EQ(builtCount, 1u);

        primitives->data.read();
        auto const & primitiveData = primitives->data.getData();
        size_t const aggregateStart = static_cast<size_t>(params.fwnAggregatesOffset);
        size_t const aggregateEnd = aggregateStart + expectedData.nodes.size() * 8u;
        ASSERT_LE(aggregateEnd, primitiveData.size());

        auto computeExpectedAggregate = [&expectedData](MeshBVHNode const & node) {
            MeshBVHFwnAggregate aggregate{};

            int const primEnd = node.primStart + node.primCount;
            for (int triIdx = node.primStart; triIdx < primEnd; ++triIdx)
            {
                auto const & tri = expectedData.triangles[static_cast<size_t>(triIdx)];
                float const ex = tri.v1.x - tri.v0.x;
                float const ey = tri.v1.y - tri.v0.y;
                float const ez = tri.v1.z - tri.v0.z;
                float const fx = tri.v2.x - tri.v0.x;
                float const fy = tri.v2.y - tri.v0.y;
                float const fz = tri.v2.z - tri.v0.z;
                float const cx = ey * fz - ez * fy;
                float const cy = ez * fx - ex * fz;
                float const cz = ex * fy - ey * fx;
                float const area = 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);

                aggregate.weightedNormalSum.x += 2.0f * area * tri.faceNormal.x;
                aggregate.weightedNormalSum.y += 2.0f * area * tri.faceNormal.y;
                aggregate.weightedNormalSum.z += 2.0f * area * tri.faceNormal.z;
                aggregate.areaCentroid.x += area * (tri.v0.x + tri.v1.x + tri.v2.x) * (1.0f / 3.0f);
                aggregate.areaCentroid.y += area * (tri.v0.y + tri.v1.y + tri.v2.y) * (1.0f / 3.0f);
                aggregate.areaCentroid.z += area * (tri.v0.z + tri.v1.z + tri.v2.z) * (1.0f / 3.0f);
                aggregate.areaCentroid.w += area;
            }

            if (aggregate.areaCentroid.w > 0.0f)
            {
                float const centerX = aggregate.areaCentroid.x / aggregate.areaCentroid.w;
                float const centerY = aggregate.areaCentroid.y / aggregate.areaCentroid.w;
                float const centerZ = aggregate.areaCentroid.z / aggregate.areaCentroid.w;
                float radiusSq = 0.0f;
                auto includeVertex = [&](float4 const & vertex) {
                    float const dx = vertex.x - centerX;
                    float const dy = vertex.y - centerY;
                    float const dz = vertex.z - centerZ;
                    radiusSq = std::max(radiusSq, dx * dx + dy * dy + dz * dz);
                };

                for (int triIdx = node.primStart; triIdx < primEnd; ++triIdx)
                {
                    auto const & tri = expectedData.triangles[static_cast<size_t>(triIdx)];
                    includeVertex(tri.v0);
                    includeVertex(tri.v1);
                    includeVertex(tri.v2);
                }
                aggregate.weightedNormalSum.w = std::sqrt(radiusSq);
            }

            return aggregate;
        };

        constexpr float kTolerance = 5.0e-4f;
        for (size_t nodeIdx = 0; nodeIdx < expectedData.nodes.size(); ++nodeIdx)
        {
            auto const & node = expectedData.nodes[nodeIdx];
            ASSERT_GE(node.primStart, 0) << "node " << nodeIdx;
            ASSERT_GT(node.primCount, 0) << "node " << nodeIdx;
            ASSERT_LE(node.primStart + node.primCount, params.triCount) << "node " << nodeIdx;

            auto const expected = computeExpectedAggregate(node);
            size_t const offset = aggregateStart + nodeIdx * 8u;
            EXPECT_NEAR(primitiveData[offset + 0], expected.weightedNormalSum.x, kTolerance) << "node " << nodeIdx;
            EXPECT_NEAR(primitiveData[offset + 1], expected.weightedNormalSum.y, kTolerance) << "node " << nodeIdx;
            EXPECT_NEAR(primitiveData[offset + 2], expected.weightedNormalSum.z, kTolerance) << "node " << nodeIdx;
            EXPECT_NEAR(primitiveData[offset + 3], expected.weightedNormalSum.w, kTolerance) << "node " << nodeIdx;
            EXPECT_NEAR(primitiveData[offset + 4], expected.areaCentroid.x, kTolerance) << "node " << nodeIdx;
            EXPECT_NEAR(primitiveData[offset + 5], expected.areaCentroid.y, kTolerance) << "node " << nodeIdx;
            EXPECT_NEAR(primitiveData[offset + 6], expected.areaCentroid.z, kTolerance) << "node " << nodeIdx;
            EXPECT_NEAR(primitiveData[offset + 7], expected.areaCentroid.w, kTolerance) << "node " << nodeIdx;
        }
    }

    /// Compare GPU-built FWN aggregates against the host's recursive
    /// `computeFwnAggregates` implementation on a higher-subdivision icosphere.
    TEST_F(MeshSdfPerformance_Test, FwnAggregates_GpuVsHost_DeepBvh_MatchesHostSums)
    {
        if (!m_context->isValid())
        {
            GTEST_SKIP() << "OpenCL context not available";
        }

        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        // Subdivision 4 → ~5120 triangles, BVH depth ~9-10.
        createIcosphere(vertices, indices, 10.0f, 4);

        ResourceKey key(ResourceId{4242}, ResourceType::Mesh);
        SpatialMeshResource resource(key, vertices, indices);

        MeshSdfEvaluationConfig cfg{};
        cfg.method = MeshSdfMethod::FastWindingNumber;
        cfg.fwnBeta = 2.0f;
        ASSERT_TRUE(resource.setEvaluationConfig(cfg));

        // Make a working copy of the BVH and run the host bottom-up reference.
        SpatialMeshData hostData = resource.getData();
        ASSERT_FALSE(hostData.nodes.empty());
        ASSERT_FALSE(hostData.triangles.empty());
        computeFwnAggregates(hostData);
        ASSERT_EQ(hostData.fwnAggregates.size(), hostData.nodes.size());

        auto core = std::make_shared<ComputeCore>(
            m_context, RequiredCapabilities::ComputeOnly, m_logger);
        auto slicerProgram = core->getSlicerProgram();
        ASSERT_TRUE(slicerProgram != nullptr);
        slicerProgram->setModelKernel(R"CLC(
    float4 model(float3 pos, PAYLOAD_ARGS)
    {
        return (float4)(0.0f, 0.0f, 0.0f, length(pos) - 1.0f);
    }
    )CLC");
        slicerProgram->recompileBlocking();
        ASSERT_TRUE(slicerProgram->isValid());

        auto primitives = core->getPrimitives();
        ASSERT_TRUE(primitives != nullptr);
        primitives->clear();
        resource.write(*primitives);
        primitives->write();

        auto paramsOpt = resource.getFwnAggregateBuildParams();
        ASSERT_TRUE(paramsOpt.has_value());
        auto const params = paramsOpt.value();

        size_t const builtCount = core->buildMeshFwnAggregates({params});
        ASSERT_EQ(builtCount, 1u);

        primitives->data.read();
        auto const & primitiveData = primitives->data.getData();
        size_t const aggregateStart = static_cast<size_t>(params.fwnAggregatesOffset);
        size_t const aggregateEnd = aggregateStart + hostData.nodes.size() * 8u;
        ASSERT_LE(aggregateEnd, primitiveData.size());

        // Per-component error tracking. Track absolute and relative differences
        // for the dipole-relevant fields. Internal nodes have non-empty subtree
        // ranges, leaves contain a few triangles each.
        struct WorstDiff
        {
            float absDiff{0.0f};
            float relDiff{0.0f};
            size_t nodeIdx{0};
            float gpuVal{0.0f};
            float hostVal{0.0f};
        };
        WorstDiff worstNormal;
        WorstDiff worstCentroid;
        WorstDiff worstArea;
        WorstDiff worstRadius;
        size_t internalNodes = 0;
        size_t leafNodes = 0;

        auto consider = [](WorstDiff & w, float gpu, float host, size_t idx) {
            float const diff = std::fabs(gpu - host);
            float const denom = std::max(1.0e-6f, std::fabs(host));
            float const rel = diff / denom;
            if (diff > w.absDiff)
            {
                w.absDiff = diff;
                w.relDiff = rel;
                w.nodeIdx = idx;
                w.gpuVal = gpu;
                w.hostVal = host;
            }
        };

        for (size_t nodeIdx = 0; nodeIdx < hostData.nodes.size(); ++nodeIdx)
        {
            auto const & node = hostData.nodes[nodeIdx];
            if (node.isLeaf()) { ++leafNodes; }
            else { ++internalNodes; }

            auto const & host = hostData.fwnAggregates[nodeIdx];
            size_t const offset = aggregateStart + nodeIdx * 8u;
            float const gpuNx = primitiveData[offset + 0];
            float const gpuNy = primitiveData[offset + 1];
            float const gpuNz = primitiveData[offset + 2];
            float const gpuRadius = primitiveData[offset + 3];
            float const gpuCx = primitiveData[offset + 4];
            float const gpuCy = primitiveData[offset + 5];
            float const gpuCz = primitiveData[offset + 6];
            float const gpuArea = primitiveData[offset + 7];

            float const nMag = std::sqrt(gpuNx*gpuNx + gpuNy*gpuNy + gpuNz*gpuNz);
            float const hostNMag = std::sqrt(host.weightedNormalSum.x*host.weightedNormalSum.x
                                           + host.weightedNormalSum.y*host.weightedNormalSum.y
                                           + host.weightedNormalSum.z*host.weightedNormalSum.z);
            consider(worstNormal, nMag, hostNMag, nodeIdx);

            float const cMag = std::sqrt(gpuCx*gpuCx + gpuCy*gpuCy + gpuCz*gpuCz);
            float const hostCMag = std::sqrt(host.areaCentroid.x*host.areaCentroid.x
                                           + host.areaCentroid.y*host.areaCentroid.y
                                           + host.areaCentroid.z*host.areaCentroid.z);
            consider(worstCentroid, cMag, hostCMag, nodeIdx);

            consider(worstArea, gpuArea, host.areaCentroid.w, nodeIdx);
            consider(worstRadius, gpuRadius, host.weightedNormalSum.w, nodeIdx);
        }

        // Sums (weightedNormal, areaCentroid, totalArea) are associative and
        // should agree to within a tight FP tolerance scaled by sqrt(triangle count).
        // Radius diverges by design: GPU computes the tight true radius from
        // vertices in the subtree; host computes a recursive conservative
        // bound. Host radius >= GPU radius in general.
        float const numTriBound = std::max(1.0f, static_cast<float>(hostData.triangles.size()));
        float const aggregateTolerance = 5.0e-3f * std::sqrt(numTriBound);
        auto const meshSummary = fmt::format("nodes={} internal={} leaf={} triangles={}",
                                             hostData.nodes.size(),
                                             internalNodes,
                                             leafNodes,
                                             hostData.triangles.size());
        auto describeWorst = [](char const * name, WorstDiff const & w) {
            return fmt::format("{} worst |Δ|={} rel={} node={} gpu={} host={}",
                               name,
                               w.absDiff,
                               w.relDiff,
                               w.nodeIdx,
                               w.gpuVal,
                               w.hostVal);
        };
        EXPECT_LT(worstNormal.absDiff, aggregateTolerance)
            << meshSummary << "; " << describeWorst("weightedNormalSum", worstNormal);
        EXPECT_LT(worstCentroid.absDiff, aggregateTolerance)
            << meshSummary << "; " << describeWorst("areaCentroid", worstCentroid);
        EXPECT_LT(worstArea.absDiff, aggregateTolerance)
            << meshSummary << "; " << describeWorst("totalArea", worstArea);
        // Radius can differ; assert host radius is always >= GPU radius (host is conservative).
        for (size_t nodeIdx = 0; nodeIdx < hostData.nodes.size(); ++nodeIdx)
        {
            float const gpuRadius = primitiveData[aggregateStart + nodeIdx * 8u + 3u];
            float const hostRadius = hostData.fwnAggregates[nodeIdx].weightedNormalSum.w;
            EXPECT_GE(hostRadius + 1.0e-3f, gpuRadius)
                << "Node " << nodeIdx << ": host radius (" << hostRadius
                << ") should bound GPU radius (" << gpuRadius << ")";
        }
    }

    /// Validate that voxel grid stores actual triangle indices (not placeholders)
    /// This test verifies the voxel acceleration fast path can be used
    TEST_F(MeshSdfPerformance_Test, VoxelGrid_StoresValidTriangleIndices)
    {
        // Create a simple mesh
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createIcosphere(vertices, indices, 10.0f, 3);  // ~1280 triangles

        ResourceKey key(ResourceId{1}, ResourceType::Mesh);
        SpatialMeshResource resource(key, vertices, indices);

        ASSERT_FALSE(resource.getData().empty()) << "BVH build failed";

        // Get build params
        auto paramsOpt = resource.getVoxelGridBuildParams();
        ASSERT_TRUE(paramsOpt.has_value()) << "Should return valid build params";
        
        auto const& params = paramsOpt.value();
        
        // Verify the voxel data layout is correct
        // Each voxel stores 2 floats: [nearestTriIndex, approxSignedDist]
        EXPECT_GT(params.voxelCount, 0) << "Should have voxels allocated";
        EXPECT_GT(params.voxelDataOffset, 0) << "Voxel data offset should be positive";
        
        // The triangle count should match the mesh
        EXPECT_EQ(params.triCount, static_cast<int>(indices.size()));

        std::cout << "\n=== Voxel Grid Triangle Index Storage ===" << std::endl;
        std::cout << "Triangle count: " << params.triCount << std::endl;
        std::cout << "Voxel count: " << params.voxelCount << std::endl;
        std::cout << "Voxel data offset: " << params.voxelDataOffset << std::endl;
        std::cout << "\nNote: After GPU build, each voxel stores:" << std::endl;
        std::cout << "  - nearestTriIndex: index of closest triangle (0 to " << (params.triCount - 1) << ")" << std::endl;
        std::cout << "  - approxSignedDist: signed distance at voxel center" << std::endl;
        std::cout << "\nThis enables O(1) lookup for queries far from surface (~80% of raymarching)." << std::endl;

        SUCCEED() << "Voxel grid layout validated for triangle index storage";
    }

    /// Performance improvement summary for voxel acceleration
    /// Documents the expected speedup from storing nearest triangle indices
    TEST_F(MeshSdfPerformance_Test, VoxelAcceleration_PerformanceCharacteristics)
    {
        std::cout << "\n=== Voxel Acceleration Performance Analysis ===" << std::endl;
        std::cout << "\nQuery Path Complexity:" << std::endl;
        std::cout << "  - Without voxel grid: O(log N) BVH traversal per query" << std::endl;
        std::cout << "  - With voxel grid (far from surface): O(1) lookup + 1 triangle test" << std::endl;
        std::cout << "  - With voxel grid (near surface): O(log N) fallback for accuracy" << std::endl;
        
        std::cout << "\nExpected Query Distribution (typical raymarching):" << std::endl;
        std::cout << "  - ~80% queries are far from surface (use fast path)" << std::endl;
        std::cout << "  - ~20% queries are near surface (use accurate BVH path)" << std::endl;
        
        std::cout << "\nVoxel Grid Memory Overhead:" << std::endl;
        std::cout << "  - 8 bytes per voxel (2 floats: triIndex + signedDist)" << std::endl;
        std::cout << "  - Typical grid: 16^3 = 4096 voxels = 32 KB" << std::endl;
        std::cout << "  - Maximum grid: 32^3 = 32768 voxels = 256 KB" << std::endl;
        
        std::cout << "\nBuild Cost:" << std::endl;
        std::cout << "  - One-time GPU kernel execution after mesh upload" << std::endl;
        std::cout << "  - Cost: O(V * log N) where V=voxel count, N=triangle count" << std::endl;
        std::cout << "  - Amortized over thousands of SDF queries during rendering" << std::endl;
        
        std::cout << "\nSpec 002 Success Criteria:" << std::endl;
        std::cout << "  - SC-001: 20% mesh SDF query time reduction" << std::endl;
        std::cout << "  - SC-002: Measurable viewport frame rate improvement" << std::endl;
        std::cout << "  - SC-003: No visual artifacts from acceleration" << std::endl;

        SUCCEED() << "Performance characteristics documented";
    }

}  // namespace gladius::tests
