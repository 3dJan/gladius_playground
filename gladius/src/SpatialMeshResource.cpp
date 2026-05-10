/// @file SpatialMeshResource.cpp
/// @brief Implementation of SpatialMeshResource for mesh SDF computation
/// @see SpatialMeshResource.h

#include "SpatialMeshResource.h"
#include "MeshVoxelGrid.h"
#include "Profiling.h"
#include "io/vdb.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <limits>

namespace gladius
{
    namespace
    {
                constexpr std::size_t MEBIBYTE = 1024u * 1024u;
                constexpr std::size_t DEFAULT_NANOVDB_BUDGET_BYTES = 1024u * MEBIBYTE;
                constexpr double NANOVDB_FLOAT_GRID_BYTES_PER_VOXEL = 12.0;
                constexpr double NANOVDB_INDEX_GRID_BYTES_PER_VOXEL = 10.0;
                constexpr double NANOVDB_ACTIVE_VOXEL_SAFETY_FACTOR = 1.35;
                constexpr std::size_t NANOVDB_GRID_FIXED_OVERHEAD_BYTES = 8u * MEBIBYTE;
                constexpr float NANOVDB_MIN_VOXEL_SIZE_MM = 0.01f;
                constexpr float NANOVDB_MAX_SUGGESTED_VOXEL_SIZE_MM = 2.0f;
                constexpr float NANOVDB_SUGGESTION_GROWTH_FACTOR = 1.25f;
                constexpr int NANOVDB_MAX_SUGGESTION_STEPS = 32;

                struct NanoVdbWorkingSetEstimate
                {
                        std::size_t totalBytes = 0u;
                        std::size_t flatMeshBytes = 0u;
                        std::size_t nearSdfBytes = 0u;
                        std::size_t farFaceIndexBytes = 0u;
                        std::size_t nearFaceIndexBytes = 0u;
                };

        /// Convert int to float preserving bit pattern (for GPU interop)
        inline float intBitsToFloat(int value)
        {
            float result;
            std::memcpy(&result, &value, sizeof(float));
            return result;
        }

                inline double bytesToMiB(std::size_t const bytes)
                {
                        return static_cast<double>(bytes) / static_cast<double>(MEBIBYTE);
                }

                inline std::size_t saturatingByteEstimate(long double const value)
                {
                        if (!std::isfinite(value) || value <= 0.0L)
                        {
                                return 0u;
                        }

                        long double const clamped =
                            std::min(value, static_cast<long double>(std::numeric_limits<std::size_t>::max()));
                        return static_cast<std::size_t>(std::ceil(clamped));
                }

                inline long double bboxExtent(float const minValue, float const maxValue)
                {
                        return std::max<long double>(static_cast<long double>(maxValue) -
                                                                                     static_cast<long double>(minValue),
                                                                                 0.0L);
                }

                inline long double bboxSurfaceAreaMm2(BoundingBox const & boundingBox)
                {
                        auto const dx = bboxExtent(boundingBox.min.x, boundingBox.max.x);
                        auto const dy = bboxExtent(boundingBox.min.y, boundingBox.max.y);
                        auto const dz = bboxExtent(boundingBox.min.z, boundingBox.max.z);
                        return 2.0L * (dx * dy + dx * dz + dy * dz);
                }

                inline long double triangleAreaMm2(MeshTriangle const & tri)
                {
                        long double const ax = static_cast<long double>(tri.v1.x) - tri.v0.x;
                        long double const ay = static_cast<long double>(tri.v1.y) - tri.v0.y;
                        long double const az = static_cast<long double>(tri.v1.z) - tri.v0.z;
                        long double const bx = static_cast<long double>(tri.v2.x) - tri.v0.x;
                        long double const by = static_cast<long double>(tri.v2.y) - tri.v0.y;
                        long double const bz = static_cast<long double>(tri.v2.z) - tri.v0.z;

                        long double const crossX = ay * bz - az * by;
                        long double const crossY = az * bx - ax * bz;
                        long double const crossZ = ax * by - ay * bx;

                        return 0.5L * std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
                }

                inline long double estimateMeshSurfaceAreaMm2(SpatialMeshData const & data)
                {
                        long double surfaceArea = 0.0L;
                        for (auto const & tri : data.triangles)
                        {
                                surfaceArea += triangleAreaMm2(tri);
                        }

                        if (surfaceArea <= 0.0L)
                        {
                                surfaceArea = bboxSurfaceAreaMm2(data.boundingBox);
                        }

                        return surfaceArea;
                }

                std::size_t estimateNanoVdbGridWorkingSetBytes(SpatialMeshData const & data,
                                                                                                             long double const meshSurfaceAreaMm2,
                                                                                                             float const voxelSize_mm,
                                                                                                             float const halfBandVoxels,
                                                                                                             double const bytesPerVoxel)
                {
                        long double const voxelSize =
                            std::max<long double>(static_cast<long double>(voxelSize_mm), NANOVDB_MIN_VOXEL_SIZE_MM);
                        long double const bandWorld = static_cast<long double>(halfBandVoxels) * voxelSize;

                        long double const dx = bboxExtent(data.boundingBox.min.x, data.boundingBox.max.x) +
                                                                     2.0L * bandWorld;
                        long double const dy = bboxExtent(data.boundingBox.min.y, data.boundingBox.max.y) +
                                                                     2.0L * bandWorld;
                        long double const dz = bboxExtent(data.boundingBox.min.z, data.boundingBox.max.z) +
                                                                     2.0L * bandWorld;

                        long double const cellsX = std::max<long double>(std::ceil(dx / voxelSize), 1.0L);
                        long double const cellsY = std::max<long double>(std::ceil(dy / voxelSize), 1.0L);
                        long double const cellsZ = std::max<long double>(std::ceil(dz / voxelSize), 1.0L);
                        long double const denseVoxelCount = cellsX * cellsY * cellsZ;

                        long double const voxelVolumeMm3 = voxelSize * voxelSize * voxelSize;
                        long double const shellVolumeMm3 = std::max(
                            meshSurfaceAreaMm2 * (2.0L * bandWorld + voxelSize),
                            bboxSurfaceAreaMm2(data.boundingBox) * voxelSize);
                        long double const shellVoxelCount =
                            std::max<long double>((shellVolumeMm3 / voxelVolumeMm3) *
                                                                            NANOVDB_ACTIVE_VOXEL_SAFETY_FACTOR,
                                                                        1.0L);
                        long double const activeVoxelCount = std::min(denseVoxelCount, shellVoxelCount);

                        return saturatingByteEstimate(activeVoxelCount * bytesPerVoxel +
                                                                                    static_cast<long double>(
                                                                                        NANOVDB_GRID_FIXED_OVERHEAD_BYTES));
                }

                NanoVdbWorkingSetEstimate estimateNanoVdbWorkingSet(SpatialMeshData const & data,
                                                                                                                        float const voxelSize_mm,
                                                                                                                        std::size_t const existingPayloadBytes)
                {
                        NanoVdbWorkingSetEstimate estimate{};
                        long double const meshSurfaceAreaMm2 = estimateMeshSurfaceAreaMm2(data);

                        estimate.flatMeshBytes = data.triangles.size() * 9u * sizeof(float);
                        estimate.nearSdfBytes = estimateNanoVdbGridWorkingSetBytes(
                            data, meshSurfaceAreaMm2, voxelSize_mm, 8.0f, NANOVDB_FLOAT_GRID_BYTES_PER_VOXEL);
                        estimate.farFaceIndexBytes = estimateNanoVdbGridWorkingSetBytes(
                            data, meshSurfaceAreaMm2, 1.0f, 150.0f, NANOVDB_INDEX_GRID_BYTES_PER_VOXEL);
                        estimate.nearFaceIndexBytes = estimateNanoVdbGridWorkingSetBytes(
                            data, meshSurfaceAreaMm2, 0.2f, 50.0f, NANOVDB_INDEX_GRID_BYTES_PER_VOXEL);
                        estimate.totalBytes = existingPayloadBytes + estimate.flatMeshBytes +
                                                                    estimate.nearSdfBytes + estimate.farFaceIndexBytes +
                                                                    estimate.nearFaceIndexBytes;
                        return estimate;
                }

                std::size_t effectiveNanoVdbBudgetBytes(NanoVdbBuildPolicy const & buildPolicy)
                {
                        return buildPolicy.budgetBytes != 0u ? buildPolicy.budgetBytes
                                                                                                 : DEFAULT_NANOVDB_BUDGET_BYTES;
                }

                float suggestNanoVdbVoxelSizeMm(SpatialMeshData const & data,
                                                                                float const requestedVoxelSize_mm,
                                                                                std::size_t const existingPayloadBytes,
                                                                                std::size_t const budgetBytes)
                {
                        if (budgetBytes == 0u)
                        {
                                return 0.0f;
                        }

                        float suggestedVoxelSize =
                            std::max(requestedVoxelSize_mm, NANOVDB_MIN_VOXEL_SIZE_MM);

                        for (int step = 0; step < NANOVDB_MAX_SUGGESTION_STEPS; ++step)
                        {
                                auto const estimate =
                                    estimateNanoVdbWorkingSet(data, suggestedVoxelSize, existingPayloadBytes);
                                if (estimate.totalBytes <= budgetBytes)
                                {
                                        return suggestedVoxelSize;
                                }

                                if (suggestedVoxelSize >= NANOVDB_MAX_SUGGESTED_VOXEL_SIZE_MM)
                                {
                                        break;
                                }

                                suggestedVoxelSize = std::min(NANOVDB_MAX_SUGGESTED_VOXEL_SIZE_MM,
                                                                                            suggestedVoxelSize * NANOVDB_SUGGESTION_GROWTH_FACTOR);
                        }

                        auto const maxEstimate = estimateNanoVdbWorkingSet(
                            data, NANOVDB_MAX_SUGGESTED_VOXEL_SIZE_MM, existingPayloadBytes);
                        if (maxEstimate.totalBytes <= budgetBytes)
                        {
                                return NANOVDB_MAX_SUGGESTED_VOXEL_SIZE_MM;
                        }

                        return 0.0f;
                }
    }  // namespace
    // ========================================================================
    // Constructors
    // ========================================================================

    SpatialMeshResource::SpatialMeshResource(ResourceKey key,
                                             SpatialMeshData && data,
                                             NanoVdbBuildPolicy const & nanovdbBuildPolicy)
        : MeshResourceBase(std::move(key))
        , m_data(std::move(data))
        , m_nanovdbBuildPolicy(nanovdbBuildPolicy)
    {
        ResourceBase::load();
    }

    SpatialMeshResource::SpatialMeshResource(ResourceKey key,
                                             SpatialMeshData && data,
                                             MeshSdfEvaluationConfig const & evaluationConfig,
                                             NanoVdbBuildPolicy const & nanovdbBuildPolicy)
        : MeshResourceBase(std::move(key))
        , m_data(std::move(data))
        , m_nanovdbBuildPolicy(nanovdbBuildPolicy)
        , m_evaluationConfig(evaluationConfig)
    {
        ResourceBase::load();
    }

    SpatialMeshResource::SpatialMeshResource(ResourceKey key,
                                             std::span<float4 const> vertices,
                                             std::span<TriangleIndices const> indices,
                                             NanoVdbBuildPolicy const & nanovdbBuildPolicy)
        : MeshResourceBase(std::move(key))
        , m_nanovdbBuildPolicy(nanovdbBuildPolicy)
    {
        MeshBVHBuilder builder;
        MeshBVHBuildParams params;
        params.maxPrimitivesPerLeaf = 4;
        params.maxDepth = 32;
        params.traversalCost = 1.0f;
        params.intersectionCost = 1.0f;

        m_data = builder.build(vertices, indices, params);
        ResourceBase::load();
    }

    std::string SpatialMeshResource::formatNanoVdbBuildMessage(
      std::string const & displayName) const
    {
        if (!hasNanoVdbBuildIssue())
        {
            return {};
        }

        auto const subject = displayName.empty() ? std::string{"mesh resource"}
                                                 : fmt::format("mesh '{}'", displayName);
        auto message = fmt::format("NanoVDB unavailable for {}: {} at voxel size {:.3f} mm.",
                                   subject,
                                   m_nanovdbBuildInfo.reason,
                                   m_nanovdbBuildInfo.requestedVoxelSize_mm);

        if (m_nanovdbBuildInfo.suggestedVoxelSize_mm >
            m_nanovdbBuildInfo.requestedVoxelSize_mm)
        {
            message += fmt::format(" Suggested NanoVDB voxel size: {:.3f} mm.",
                                   m_nanovdbBuildInfo.suggestedVoxelSize_mm);
        }
        else if (m_nanovdbBuildInfo.result == NanoVdbBuildResult::PreflightRejected)
        {
            message +=
              " Try a larger NanoVDB voxel size or switch to Voxel-Accelerated, PureBVH, or FastWindingNumber.";
        }

        return message;
    }

    // ========================================================================
    // Public Methods
    // ========================================================================

    void SpatialMeshResource::invalidate()
    {
        m_needsRebuild = true;
        m_needsVoxelGridBuild = true;
        resetSignCacheBuildProgress();
    }

    bool SpatialMeshResource::setEvaluationConfig(MeshSdfEvaluationConfig const & cfg)
    {
        ProfileFunction;
        bool const touchesFwn = cfg.method == MeshSdfMethod::FastWindingNumber ||
                                m_evaluationConfig.method == MeshSdfMethod::FastWindingNumber;
        GLADIUS_FWN_PREP_SCOPE_IF("SpatialMeshResource::setEvaluationConfig", touchesFwn);

        bool const rebuildPotentiallyRequired = requiresMeshRebuild(m_evaluationConfig, cfg);
        bool const fwnBetaChanged = m_evaluationConfig.fwnBeta != cfg.fwnBeta;
        bool const fwnSignCacheUsageChanged =
            m_evaluationConfig.fwnUseSignCache != cfg.fwnUseSignCache;

        // Even when the *method* changed, the existing payload may already
        // contain everything the new method needs:
        //   - PureBVH:           always satisfied (BVH is always present).
        //   - FastWindingNumber: needs reserved FWN aggregate/sign-cache payload,
        //                        which is populated on the GPU after upload.
        //   - VoxelAccelerated:  needs a populated voxel grid (m_voxelCount>0).
        // The voxel-grid resolution is the only setting that *requires* a
        // payload rebuild. Detecting payload sufficiency here avoids a costly
        // re-serialise on the auto-apply that fires immediately after a fresh
        // 3MF import (settings persist across sessions, but the freshly
        // constructed resource starts with the default VoxelAccelerated
        // method).
        bool const resolutionChanged =
            m_evaluationConfig.voxelGridResolution != cfg.voxelGridResolution;
        bool const payloadHasFwnSupport = m_fwnAggregatesOffset != 0u && m_signCacheDataOffset != 0u;
        GLADIUS_FWN_PREP_LOG_IF(touchesFwn,
                                "SpatialMeshResource::setEvaluationConfig method=" +
                                    std::string(toString(cfg.method)) +
                                    " payloadHasFwnSupport=" +
                                    std::to_string(payloadHasFwnSupport) +
                                    " fwnBetaChanged=" + std::to_string(fwnBetaChanged));
        bool const newMethodSatisfied = [&]() {
            // When the current payload has a NanoVDB companion (SDF_VDB meta entry) but the
            // new method does not need one, the companion would cause the kernel to dispatch
            // through the NanoVDB path even after the method switch. Force a rebuild to remove it.
            bool const hasNanoVdbCompanion = (m_nanovdbGridOffset != 0u);
            bool const newMethodNeedsNanoVdb = (cfg.method == MeshSdfMethod::NanoVDB);
            if (hasNanoVdbCompanion && !newMethodNeedsNanoVdb)
            {
                return false;
            }

            switch (cfg.method)
            {
            case MeshSdfMethod::PureBVH:
                return true;
            case MeshSdfMethod::FastWindingNumber:
                return payloadHasFwnSupport;
            case MeshSdfMethod::VoxelAccelerated:
                return m_voxelCount > 0;
            case MeshSdfMethod::NanoVDB:
                return m_nanovdbGridOffset != 0u || isNanoVdbSatisfiedWithoutGrid();
            }
            return false;
        }();
        bool const rebuildRequired = resolutionChanged || !newMethodSatisfied;

        m_evaluationConfig = cfg;
        if (cfg.method == MeshSdfMethod::VoxelAccelerated)
        {
            m_needsVoxelGridBuild = m_voxelCount > 0u;
        }
        else
        {
            m_needsVoxelGridBuild = false;
        }

        if (cfg.method == MeshSdfMethod::FastWindingNumber && cfg.fwnUseSignCache &&
            (fwnBetaChanged || fwnSignCacheUsageChanged))
        {
            // Existing GPU sign caches were generated for the previous beta. Keep
            // them disabled by the kernel-side beta check and queue a rebuild the
            // next time post-upload cache work is collected.
            resetSignCacheBuildProgress();
        }
        else if (cfg.method == MeshSdfMethod::FastWindingNumber && !cfg.fwnUseSignCache)
        {
            m_needsSignCacheBuild = false;
            m_signCacheNextWord = 0;
        }
        else if (cfg.method != MeshSdfMethod::FastWindingNumber)
        {
            m_needsFwnAggregateBuild = false;
            m_needsSignCacheBuild = false;
            m_signCacheNextWord = 0;
        }
        if (rebuildRequired)
        {
            // Drop the cached payload and re-serialise with the new method
            // (e.g. allocating or skipping the voxel grid). reload() resets the
            // base class' "already loaded" flag and then re-runs loadImpl().
            m_payloadData.data.clear();
            m_payloadData.meta.clear();
            m_needsRebuild = true;
            m_needsVoxelGridBuild = (cfg.method == MeshSdfMethod::VoxelAccelerated);
            reload();
        }
        // Report rebuildPotentiallyRequired so callers see "the config changed"
        // even when we skipped the heavy reload — they may still need to
        // refresh other state (e.g. RenderingSettings flags).
        return rebuildPotentiallyRequired || fwnSignCacheUsageChanged;
    }

    void SpatialMeshResource::rebuild(std::span<float4 const> vertices,
                                      std::span<TriangleIndices const> indices)
    {
        ProfileFunction;

        MeshBVHBuilder builder;
        MeshBVHBuildParams params;
        params.maxPrimitivesPerLeaf = 4;
        params.maxDepth = 32;
        params.traversalCost = 1.0f;
        params.intersectionCost = 1.0f;

        m_data = builder.build(vertices, indices, params);
        m_needsRebuild = false;

        // Clear and reload payload data
        m_payloadData.data.clear();
        m_payloadData.meta.clear();
        loadImpl();
        m_needsVoxelGridBuild = m_voxelCount > 0u &&
                                m_evaluationConfig.method == MeshSdfMethod::VoxelAccelerated;
    }
    
    // ========================================================================
    // Header Layout Constants
    // ========================================================================
    static constexpr size_t kBboxFloats = 8;           // min.xyzw + max.xyzw
    static constexpr size_t kCountsFloats = 4;         // nodeCount, triCount, vertexNormalCount, reserved
    static constexpr size_t kBvhOffsetsFloats = 4;     // nodesOffset, trianglesOffset, normalsOffset, indicesOffset
    static constexpr size_t kVoxelHeaderFloats = 10;   // origin.xyz, dims.xyz, voxelSize, invVoxelSize, threshold, padding
    static constexpr size_t kVoxelInfoFloats = 2;      // voxelDataOffset, voxelCount
    static constexpr size_t kEdgeNeighborsSlot = 1;    // edgeNeighborsOffset
    static constexpr size_t kFwnAggregatesSlot = 1;    // fwnAggregatesOffset
    static constexpr size_t kSignCacheOffsetSlot = 1;      // signCacheDataOffset (FWN coarse sign cache; 0 = not ready)
    static constexpr size_t kSignCacheResolutionSlot = 1;  // sign cache resolution per axis
    static constexpr size_t kSignCacheBetaSlot = 1;        // beta used to build the ready sign cache
    static constexpr size_t kNanoVdbOffsetSlot = 1;        // local float offset of the NanoVDB grid (0 = none)

    /// Coarse sign-cache resolution per axis (FWN acceleration). 64^3 = 262144 cells.
    /// Each cell stores a conservative 2-bit state (unknown/outside/inside), so
    /// this uses 16384 ints = 64 KB per mesh. Hard-coded; no UI knob.
    static constexpr int kSignCacheResolution = 64;
    static constexpr size_t kSignCacheBitsPerCell = 2;
    static constexpr size_t kSignCacheBitsPerWord = 32;
    static constexpr size_t kSignCacheCellsPerWord = kSignCacheBitsPerWord / kSignCacheBitsPerCell;
    static constexpr size_t kSignCacheWordCount =
        (static_cast<size_t>(kSignCacheResolution) * kSignCacheResolution * kSignCacheResolution +
         kSignCacheCellsPerWord - 1) /
        kSignCacheCellsPerWord;
    static constexpr int kSignCacheWordsPerBuildStep = 512;

    static constexpr size_t kBvhOffsetsOffset = kBboxFloats + kCountsFloats;  // 12
    static constexpr size_t kVoxelInfoOffset = kBvhOffsetsOffset + kBvhOffsetsFloats + kVoxelHeaderFloats;  // 26
    static constexpr size_t kEdgeNeighborsOffsetIndex = kVoxelInfoOffset + kVoxelInfoFloats;                // 28
    static constexpr size_t kFwnAggregatesOffsetIndex = kEdgeNeighborsOffsetIndex + kEdgeNeighborsSlot;     // 29
    static constexpr size_t kSignCacheOffsetIndex = kFwnAggregatesOffsetIndex + kFwnAggregatesSlot;         // 30
    static constexpr size_t kSignCacheResolutionIndex = kSignCacheOffsetIndex + kSignCacheOffsetSlot;       // 31
    static constexpr size_t kSignCacheBetaIndex = kSignCacheResolutionIndex + kSignCacheResolutionSlot;     // 32
    static constexpr size_t kNanoVdbOffsetIndex = kSignCacheBetaIndex + kSignCacheBetaSlot;                 // 33

    void SpatialMeshResource::resetSignCacheBuildProgress() noexcept
    {
        m_needsSignCacheBuild = true;
        m_signCacheNextWord = 0;
    }

    void SpatialMeshResource::markSignCacheBuildQueued(MeshSignCacheBuildParams const & params)
    {
        if (!m_needsSignCacheBuild || params.signCacheReadyOffset != signCacheReadyHostOffset())
        {
            return;
        }

        int const nextWord = std::max(m_signCacheNextWord, params.baseWord + params.wordsToBuild);
        m_signCacheNextWord = std::min(nextWord, static_cast<int>(kSignCacheWordCount));
        if (params.completesBuild || m_signCacheNextWord >= static_cast<int>(kSignCacheWordCount))
        {
            m_needsSignCacheBuild = false;
            m_signCacheNextWord = 0;
        }
    }
    
    void SpatialMeshResource::write(Primitives & primitives)
    {
        ProfileFunction;
        GLADIUS_FWN_PREP_SCOPE_IF("SpatialMeshResource::write FWN payload", usesFastWindingNumber());

        // Track the base offset before adding our data
        m_dataBaseOffset = static_cast<int>(primitives.data.getSize());
        
        // Patch the offsets in the header to be absolute
        // These were stored as local offsets during loadImpl()
        size_t const bvhOffsetsIndex = m_headerStart + kBvhOffsetsOffset;
        size_t const voxelInfoIndex = m_headerStart + kVoxelInfoOffset;
        
        m_payloadData.data[bvhOffsetsIndex + 0] = static_cast<float>(m_dataBaseOffset + m_nodesOffset);
        m_payloadData.data[bvhOffsetsIndex + 1] = static_cast<float>(m_dataBaseOffset + m_trianglesOffset);
        m_payloadData.data[bvhOffsetsIndex + 2] = static_cast<float>(m_dataBaseOffset + m_normalsOffset);
        m_payloadData.data[bvhOffsetsIndex + 3] = static_cast<float>(m_dataBaseOffset + m_indicesOffset);
        m_payloadData.data[voxelInfoIndex] = static_cast<float>(m_dataBaseOffset + m_voxelDataOffset);
        m_payloadData.data[m_headerStart + kEdgeNeighborsOffsetIndex] =
            static_cast<float>(m_dataBaseOffset + m_edgeNeighborsOffset);
        m_payloadData.data[m_headerStart + kFwnAggregatesOffsetIndex] =
            (m_fwnAggregatesOffset != 0u)
                ? static_cast<float>(m_dataBaseOffset + m_fwnAggregatesOffset)
                : 0.0f;
        // The sign cache is GPU-built after upload. Keep the ready offset at 0
        // until the queued build kernel patches it on the device.
        m_payloadData.data[m_headerStart + kSignCacheOffsetIndex] = 0.0f;
        m_payloadData.data[m_headerStart + kSignCacheResolutionIndex] =
            static_cast<float>(kSignCacheResolution);
        m_payloadData.data[m_headerStart + kSignCacheBetaIndex] = m_evaluationConfig.fwnBeta;
        // Patch NanoVDB header slot to absolute offset (0 when NanoVDB is not active).
        m_payloadData.data[m_headerStart + kNanoVdbOffsetIndex] =
            (m_nanovdbGridOffset != 0u)
                ? static_cast<float>(m_dataBaseOffset + m_nanovdbGridOffset)
                : 0.0f;
        
        // Call base implementation to add data to primitives
        ResourceBase::write(primitives);
        
        // Flag that we need GPU-side post-upload builds. Local payload data only
        // stores zero-filled cache buffers; every primitive upload invalidates the
        // previously built GPU caches and they must be rebuilt/re-enabled.
        m_needsVoxelGridBuild = m_voxelCount > 0u &&
                                m_evaluationConfig.method == MeshSdfMethod::VoxelAccelerated;
        m_needsFwnAggregateBuild = m_fwnAggregatesOffset != 0u && usesFastWindingNumber();
        if (m_signCacheDataOffset != 0u && usesFwnSignCache())
        {
            resetSignCacheBuildProgress();
            GLADIUS_FWN_PREP_LOG("SpatialMeshResource::write FWN offsets nodes=" +
                                 std::to_string(m_dataBaseOffset + static_cast<int>(m_nodesOffset)) +
                                 " triangles=" +
                                 std::to_string(m_dataBaseOffset + static_cast<int>(m_trianglesOffset)) +
                                 " aggregates=" +
                                 std::to_string(m_dataBaseOffset + static_cast<int>(m_fwnAggregatesOffset)) +
                                 " signCache=" +
                                 std::to_string(m_dataBaseOffset + static_cast<int>(m_signCacheDataOffset)));
        }
        else
        {
            m_needsSignCacheBuild = false;
            m_signCacheNextWord = 0;
        }
    }
    
    std::optional<MeshVoxelGridBuildParams> SpatialMeshResource::getVoxelGridBuildParams() const
    {
        if (m_data.empty() || m_voxelCount == 0)
        {
            return std::nullopt;
        }
        
        MeshVoxelGridBuildParams params{};
        params.headerStart = m_dataBaseOffset + static_cast<int>(m_headerStart);
        params.voxelDataOffset = m_dataBaseOffset + static_cast<int>(m_voxelDataOffset);
        params.nodesOffset = m_dataBaseOffset + static_cast<int>(m_nodesOffset);
        params.trianglesOffset = m_dataBaseOffset + static_cast<int>(m_trianglesOffset);
        params.normalsOffset = m_dataBaseOffset + static_cast<int>(m_normalsOffset);
        params.indicesOffset = m_dataBaseOffset + static_cast<int>(m_indicesOffset);
        params.edgeNeighborsOffset = m_dataBaseOffset + static_cast<int>(m_edgeNeighborsOffset);
        params.nodeCount = static_cast<int>(m_data.nodes.size());
        params.triCount = static_cast<int>(m_data.triangles.size());
        params.vertexNormalCount = static_cast<int>(m_data.vertexNormals.size());
        params.voxelCount = static_cast<int>(m_voxelCount);
        
        return params;
    }

    std::optional<MeshFwnAggregateBuildParams> SpatialMeshResource::getFwnAggregateBuildParams() const
    {
        if (m_data.empty() || m_data.nodes.empty() || m_fwnAggregatesOffset == 0u)
        {
            return std::nullopt;
        }

        MeshFwnAggregateBuildParams params{};
        params.nodesOffset = m_dataBaseOffset + static_cast<int>(m_nodesOffset);
        params.trianglesOffset = m_dataBaseOffset + static_cast<int>(m_trianglesOffset);
        params.fwnAggregatesOffset = m_dataBaseOffset + static_cast<int>(m_fwnAggregatesOffset);
        params.nodeCount = static_cast<int>(m_data.nodes.size());
        params.triCount = static_cast<int>(m_data.triangles.size());

        return params;
    }

    std::optional<MeshSignCacheBuildParams> SpatialMeshResource::getSignCacheBuildParams() const
    {
        if (m_data.empty() || m_data.nodes.empty() || m_fwnAggregatesOffset == 0u ||
            m_signCacheDataOffset == 0u || !usesFwnSignCache() || m_needsFwnAggregateBuild)
        {
            return std::nullopt;
        }

        MeshSignCacheBuildParams params{};
        params.headerStart = m_dataBaseOffset + static_cast<int>(m_headerStart);
        params.signCacheDataOffset = m_dataBaseOffset + static_cast<int>(m_signCacheDataOffset);
        params.signCacheReadyOffset = signCacheReadyHostOffset();
        params.signCacheBetaOffset = signCacheBetaHostOffset();
        params.nodesOffset = m_dataBaseOffset + static_cast<int>(m_nodesOffset);
        params.trianglesOffset = m_dataBaseOffset + static_cast<int>(m_trianglesOffset);
        params.normalsOffset = m_dataBaseOffset + static_cast<int>(m_normalsOffset);
        params.indicesOffset = m_dataBaseOffset + static_cast<int>(m_indicesOffset);
        params.edgeNeighborsOffset = m_dataBaseOffset + static_cast<int>(m_edgeNeighborsOffset);
        params.fwnAggregatesOffset = m_dataBaseOffset + static_cast<int>(m_fwnAggregatesOffset);
        params.nodeCount = static_cast<int>(m_data.nodes.size());
        params.triCount = static_cast<int>(m_data.triangles.size());
        params.vertexNormalCount = static_cast<int>(m_data.vertexNormals.size());
        params.resolution = kSignCacheResolution;
        params.wordCount = static_cast<int>(kSignCacheWordCount);
        params.baseWord = std::clamp(m_signCacheNextWord, 0, params.wordCount);
        params.wordsToBuild = std::min(kSignCacheWordsPerBuildStep, params.wordCount - params.baseWord);
        params.completesBuild = (params.baseWord + params.wordsToBuild) >= params.wordCount;
        params.fwnBeta = m_evaluationConfig.fwnBeta;

        return params;
    }

    int SpatialMeshResource::signCacheReadyHostOffset() const
    {
        return m_dataBaseOffset + static_cast<int>(m_headerStart + kSignCacheOffsetIndex);
    }

    int SpatialMeshResource::signCacheBetaHostOffset() const
    {
        return m_dataBaseOffset + static_cast<int>(m_headerStart + kSignCacheBetaIndex);
    }

    // ========================================================================
    // ResourceBase Interface
    // ========================================================================

    void SpatialMeshResource::loadImpl()
    {
        ProfileFunction;

        if (m_data.empty())
        {
            return;
        }

        m_nanovdbBuildInfo = {};

        bool const useFwn = m_evaluationConfig.method == MeshSdfMethod::FastWindingNumber;
        GLADIUS_FWN_PREP_SCOPE_IF("SpatialMeshResource::loadImpl FWN payload", useFwn);
        GLADIUS_FWN_PREP_LOG_IF(useFwn,
                                "SpatialMeshResource::loadImpl prepare nodes=" +
                                    std::to_string(m_data.nodes.size()) +
                                    " triangles=" + std::to_string(m_data.triangles.size()) +
                                    " vertexNormals=" + std::to_string(m_data.vertexNormals.size()));

        // Clear previous payload
        m_payloadData.meta.clear();

        // Create primitive metadata for the spatial mesh root
        PrimitiveMeta metaData{};
        metaData.primitiveType = SDF_SPATIAL_MESH_ROOT;
        metaData.start = static_cast<int>(m_payloadData.data.size());

        // ====================================================================
        // Header Layout (34 floats total):
        // [0-7]:   Bounding box (8 floats: min.xyzw, max.xyzw)
        // [8-11]:  Counts (4 floats: nodeCount, triCount, vertexNormalCount, reserved)
        // [12-15]: BVH offsets (4 floats: nodesOffset, trianglesOffset, normalsOffset, indicesOffset)
        // [16-25]: Voxel grid header (10 floats: origin.xyz, dims.xyz, voxelSize, invVoxelSize, threshold, padding)
        // [26-27]: Voxel grid info (2 floats: voxelDataOffset, voxelCount)
        // [28]:    Edge-neighbour face normals offset (per-edge adjacent face normals)
        // [29]:    Fast-winding-number aggregate offset
        // [30]:    FWN coarse sign-cache data offset (0 = not ready)
        // [31]:    FWN coarse sign-cache resolution per axis
        // [32]:    FWN beta used to build the ready sign cache
        // [33]:    NanoVDB grid local float offset (0 = not built)
        // ====================================================================
        
        m_headerStart = m_payloadData.data.size();

        // Serialize bounding box (8 floats)
        m_payloadData.data.push_back(m_data.boundingBox.min.x);
        m_payloadData.data.push_back(m_data.boundingBox.min.y);
        m_payloadData.data.push_back(m_data.boundingBox.min.z);
        m_payloadData.data.push_back(m_data.boundingBox.min.w);
        m_payloadData.data.push_back(m_data.boundingBox.max.x);
        m_payloadData.data.push_back(m_data.boundingBox.max.y);
        m_payloadData.data.push_back(m_data.boundingBox.max.z);
        m_payloadData.data.push_back(m_data.boundingBox.max.w);

        // Serialize counts (4 floats)
        m_payloadData.data.push_back(static_cast<float>(m_data.nodes.size()));
        m_payloadData.data.push_back(static_cast<float>(m_data.triangles.size()));
        m_payloadData.data.push_back(static_cast<float>(m_data.vertexNormals.size()));
        m_payloadData.data.push_back(0.0f);  // Reserved

        // Placeholder for BVH offsets (4 floats) - will be patched later
        size_t const bvhOffsetsIndex = m_payloadData.data.size();
        m_payloadData.data.push_back(0.0f);  // nodesOffset
        m_payloadData.data.push_back(0.0f);  // trianglesOffset
        m_payloadData.data.push_back(0.0f);  // normalsOffset
        m_payloadData.data.push_back(0.0f);  // indicesOffset

        // Compute voxel grid header from bounding box. Resolution and whether the
        // grid is actually populated are governed by the active evaluation config.
        bool const useVoxelGrid =
            (m_evaluationConfig.method == MeshSdfMethod::VoxelAccelerated);
        int const voxelResolution = (m_evaluationConfig.voxelGridResolution > 0)
                                        ? m_evaluationConfig.voxelGridResolution
                                        : kDefaultVoxelGridResolution;
        MeshVoxelGridHeader const voxelHeader = createVoxelGridHeader(
            m_data.boundingBox.min.x, m_data.boundingBox.min.y, m_data.boundingBox.min.z,
            m_data.boundingBox.max.x, m_data.boundingBox.max.y, m_data.boundingBox.max.z,
            voxelResolution);
        
        // Serialize voxel grid header (10 floats)
        m_payloadData.data.push_back(voxelHeader.originX);
        m_payloadData.data.push_back(voxelHeader.originY);
        m_payloadData.data.push_back(voxelHeader.originZ);
        m_payloadData.data.push_back(voxelHeader.dimX);
        m_payloadData.data.push_back(voxelHeader.dimY);
        m_payloadData.data.push_back(voxelHeader.dimZ);
        m_payloadData.data.push_back(voxelHeader.voxelSize);
        m_payloadData.data.push_back(voxelHeader.invVoxelSize);
        m_payloadData.data.push_back(voxelHeader.threshold);
        m_payloadData.data.push_back(voxelHeader.padding);

        // Placeholder for voxel grid info (2 floats) - will be patched later
        size_t const voxelInfoIndex = m_payloadData.data.size();
        m_payloadData.data.push_back(0.0f);  // voxelDataOffset
        // When the chosen method does not need a voxel grid, report zero voxels
        // so the kernel dispatch in sdf.cl falls through to pure-BVH evaluation.
        m_voxelCount = useVoxelGrid ? computeVoxelCount(voxelHeader) : 0u;
        m_payloadData.data.push_back(static_cast<float>(m_voxelCount));

        // FWN auxiliary offsets/resolution (patched below)
        m_payloadData.data.push_back(0.0f);  // Edge-neighbour face normals offset
        m_payloadData.data.push_back(0.0f);  // FWN aggregate offset
        m_payloadData.data.push_back(0.0f);  // FWN sign-cache data offset (0 = not ready)
        m_payloadData.data.push_back(static_cast<float>(kSignCacheResolution));
        m_payloadData.data.push_back(m_evaluationConfig.fwnBeta);
        m_payloadData.data.push_back(0.0f);  // NanoVDB grid local float offset (0 = not built)

        // Serialize BVH nodes
        // Each node: bboxMin (4), bboxMax (4), leftChild, rightChild, primStart, primCount = 12 floats
        m_nodesOffset = m_payloadData.data.size();
        {
            GLADIUS_FWN_PREP_SCOPE_IF("SpatialMeshResource::loadImpl serialize BVH nodes", useFwn);
            for (auto const & node : m_data.nodes)
            {
                m_payloadData.data.push_back(node.bboxMin.x);
                m_payloadData.data.push_back(node.bboxMin.y);
                m_payloadData.data.push_back(node.bboxMin.z);
                m_payloadData.data.push_back(node.bboxMin.w);
                m_payloadData.data.push_back(node.bboxMax.x);
                m_payloadData.data.push_back(node.bboxMax.y);
                m_payloadData.data.push_back(node.bboxMax.z);
                m_payloadData.data.push_back(node.bboxMax.w);
                m_payloadData.data.push_back(intBitsToFloat(node.leftChild));
                m_payloadData.data.push_back(intBitsToFloat(node.rightChild));
                m_payloadData.data.push_back(intBitsToFloat(node.primStart));
                m_payloadData.data.push_back(intBitsToFloat(node.primCount));
            }
        }

        // Serialize triangles
        // Each triangle: v0 (4), v1 (4), v2 (4), faceNormal (4) = 16 floats
        m_trianglesOffset = m_payloadData.data.size();
        {
            GLADIUS_FWN_PREP_SCOPE_IF("SpatialMeshResource::loadImpl serialize triangles", useFwn);
            for (auto const & tri : m_data.triangles)
            {
                m_payloadData.data.push_back(tri.v0.x);
                m_payloadData.data.push_back(tri.v0.y);
                m_payloadData.data.push_back(tri.v0.z);
                m_payloadData.data.push_back(tri.v0.w);
                m_payloadData.data.push_back(tri.v1.x);
                m_payloadData.data.push_back(tri.v1.y);
                m_payloadData.data.push_back(tri.v1.z);
                m_payloadData.data.push_back(tri.v1.w);
                m_payloadData.data.push_back(tri.v2.x);
                m_payloadData.data.push_back(tri.v2.y);
                m_payloadData.data.push_back(tri.v2.z);
                m_payloadData.data.push_back(tri.v2.w);
                m_payloadData.data.push_back(tri.faceNormal.x);
                m_payloadData.data.push_back(tri.faceNormal.y);
                m_payloadData.data.push_back(tri.faceNormal.z);
                m_payloadData.data.push_back(tri.faceNormal.w);
            }
        }

        // Serialize vertex normals
        // Each normal: xyz + w (vertex index) = 4 floats
        m_normalsOffset = m_payloadData.data.size();
        {
            GLADIUS_FWN_PREP_SCOPE_IF("SpatialMeshResource::loadImpl serialize vertex normals", useFwn);
            for (auto const & vn : m_data.vertexNormals)
            {
                m_payloadData.data.push_back(vn.normal.x);
                m_payloadData.data.push_back(vn.normal.y);
                m_payloadData.data.push_back(vn.normal.z);
                m_payloadData.data.push_back(vn.normal.w);
            }
        }

        // Serialize triangle indices for normal lookup
        // Each triangle: 3 vertex indices = 4 ints (padded)
        m_indicesOffset = m_payloadData.data.size();
        {
            GLADIUS_FWN_PREP_SCOPE_IF("SpatialMeshResource::loadImpl serialize triangle indices", useFwn);
            for (auto const & idx : m_data.triangleIndices)
            {
                m_payloadData.data.push_back(intBitsToFloat(idx.i0));
                m_payloadData.data.push_back(intBitsToFloat(idx.i1));
                m_payloadData.data.push_back(intBitsToFloat(idx.i2));
                m_payloadData.data.push_back(0.0f);  // Padding for alignment
            }
        }

        // Serialize per-edge adjacent face normals (3 entries per triangle, 4 floats each).
        // Used by computePseudoNormalFast in mesh_sdf.cl for robust sign on edge features.
        m_edgeNeighborsOffset = m_payloadData.data.size();
        {
            GLADIUS_FWN_PREP_SCOPE_IF("SpatialMeshResource::loadImpl serialize edge neighbours", useFwn);
            for (auto const & en : m_data.edgeNeighborNormals)
            {
                m_payloadData.data.push_back(en.normal.x);
                m_payloadData.data.push_back(en.normal.y);
                m_payloadData.data.push_back(en.normal.z);
                m_payloadData.data.push_back(en.normal.w);
            }
        }

        // Reserve per-node Fast-Winding-Number aggregates (8 floats per node).
        // Layout: [weightedNormal.xyz, radius] [areaCentroid.xyz, totalArea].
        // These slots are filled by buildMeshFwnAggregates on the GPU after the
        // primitive buffer upload. Keeping this CPU-side pass empty avoids the
        // O(nodeCount) aggregate prep cost on file open / method switch.
        m_fwnAggregatesOffset = 0u;
        if (useFwn)
        {
            GLADIUS_FWN_PREP_SCOPE("SpatialMeshResource::loadImpl reserve FWN aggregate slots");
            m_fwnAggregatesOffset = m_payloadData.data.size();
            size_t const aggregateFloatCount = m_data.nodes.size() * 8u;
            for (size_t i = 0; i < aggregateFloatCount; ++i)
            {
                m_payloadData.data.push_back(0.0f);
            }
        }

        // Reserve space for voxel grid data (2 floats per voxel: nearestTriIdx, signedDist)
        // This space will be filled by the buildMeshVoxelGrid kernel on GPU
        m_voxelDataOffset = m_payloadData.data.size();
        size_t const voxelDataSize = m_voxelCount * 2;  // 2 floats per voxel
        for (size_t i = 0; i < voxelDataSize; ++i)
        {
            m_payloadData.data.push_back(0.0f);  // Will be filled by GPU kernel
        }

        // Reserve space for the FWN coarse sign cache only when FWN is active.
        // Non-FWN loads never read this cache, so skipping it avoids a 64 KB
        // primitive-payload allocation/upload per mesh and prevents unnecessary
        // post-upload build bookkeeping on the first frame.
        m_signCacheDataOffset = 0u;
        if (useFwn)
        {
            GLADIUS_FWN_PREP_SCOPE("SpatialMeshResource::loadImpl reserve FWN sign-cache slots");
            m_signCacheDataOffset = m_payloadData.data.size();
            for (size_t i = 0; i < kSignCacheWordCount; ++i)
            {
                m_payloadData.data.push_back(0.0f);
            }
        }

        // Patch offsets in header (local offsets, will be adjusted with base offset when read)
        m_payloadData.data[bvhOffsetsIndex + 0] = static_cast<float>(m_nodesOffset);
        m_payloadData.data[bvhOffsetsIndex + 1] = static_cast<float>(m_trianglesOffset);
        m_payloadData.data[bvhOffsetsIndex + 2] = static_cast<float>(m_normalsOffset);
        m_payloadData.data[bvhOffsetsIndex + 3] = static_cast<float>(m_indicesOffset);
        m_payloadData.data[voxelInfoIndex] = static_cast<float>(m_voxelDataOffset);
        m_payloadData.data[m_headerStart + kEdgeNeighborsOffsetIndex] = static_cast<float>(m_edgeNeighborsOffset);
        m_payloadData.data[m_headerStart + kFwnAggregatesOffsetIndex] = static_cast<float>(m_fwnAggregatesOffset);
        // Sign cache offset stays 0 ("not ready") until the queued
        // markMeshSignCacheReady kernel patches it on the GPU buffer.
        m_payloadData.data[m_headerStart + kSignCacheOffsetIndex] = 0.0f;
        m_payloadData.data[m_headerStart + kSignCacheResolutionIndex] = static_cast<float>(kSignCacheResolution);
        m_payloadData.data[m_headerStart + kSignCacheBetaIndex] = m_evaluationConfig.fwnBeta;

        // Sign cache build is needed only when a fresh FWN cache allocation is present.
        if (useFwn)
        {
            m_needsFwnAggregateBuild = m_fwnAggregatesOffset != 0u;
            if (m_evaluationConfig.fwnUseSignCache)
            {
                resetSignCacheBuildProgress();
            }
            else
            {
                m_needsSignCacheBuild = false;
                m_signCacheNextWord = 0;
            }
        }
        else
        {
            m_needsFwnAggregateBuild = false;
            m_needsSignCacheBuild = false;
            m_signCacheNextWord = 0;
        }

        // NanoVDB path: build a 3-layer VDB acceleration structure mirroring the proven
        // VdbImporter::writeMesh() approach. Emits 4 companion primitives after this
        // SDF_SPATIAL_MESH_ROOT entry (kernel uses companion types to detect the layout):
        //   i+1: SDF_MESH_TRIANGLES  – flat 9-float/tri buffer for face-index triangle lookup
        //   i+2: SDF_VDB             – near signed-distance field (nanovdbVoxelSize_mm)
        //   i+3: SDF_VDB_FACE_INDICES – far face-index (fixed 1mm voxels, 150mm band)
        //   i+4: SDF_VDB_FACE_INDICES – near face-index (fixed 0.2mm voxels, 50-voxel band)
        //
        // IMPORTANT: face-index grids use FIXED voxel sizes (1mm/0.2mm) regardless of the
        // user-configured nanovdbVoxelSize_mm. Using the SDF voxel size for face-index grids
        // explodes memory: at 0.1mm with a 50mm band, a benchy-sized part produces a ~3GB
        // sparse grid. Only the near SDF (i+2) uses the user-configured resolution.
        // Fixed sizes match VdbImporter::writeMesh() which is the proven reference.
        m_nanovdbGridOffset = 0u;
        bool const useNanovdb = (m_evaluationConfig.method == MeshSdfMethod::NanoVDB);
        if (useNanovdb && !m_data.triangles.empty())
        {
            float const voxelSize_mm =
              std::max(m_evaluationConfig.nanovdbVoxelSize_mm, NANOVDB_MIN_VOXEL_SIZE_MM);
            std::size_t const existingPayloadBytes = m_payloadData.data.size() * sizeof(float);
            auto const workingSetEstimate =
              estimateNanoVdbWorkingSet(m_data, voxelSize_mm, existingPayloadBytes);
            std::size_t const budgetBytes = effectiveNanoVdbBudgetBytes(m_nanovdbBuildPolicy);

            m_nanovdbBuildInfo.requestedVoxelSize_mm = voxelSize_mm;
            m_nanovdbBuildInfo.estimatedBytes = workingSetEstimate.totalBytes;
            m_nanovdbBuildInfo.budgetBytes = budgetBytes;
            m_nanovdbBuildInfo.suggestedVoxelSize_mm = suggestNanoVdbVoxelSizeMm(
              m_data, voxelSize_mm, existingPayloadBytes, budgetBytes);

            auto finalizeRootOnly = [&]() {
                metaData.end = static_cast<int>(m_payloadData.data.size());
                m_payloadData.meta.push_back(metaData);
            };

            if (workingSetEstimate.totalBytes > budgetBytes)
            {
                m_nanovdbBuildInfo.result = NanoVdbBuildResult::PreflightRejected;
                m_nanovdbBuildInfo.reason = fmt::format(
                  "estimated working set {:.1f} MiB exceeds NanoVDB budget {:.1f} MiB",
                  bytesToMiB(workingSetEstimate.totalBytes),
                  bytesToMiB(budgetBytes));
                finalizeRootOnly();
                return;
            }

            std::size_t const nanoPayloadStart = m_payloadData.data.size();

            // Fixed face-index voxel sizes — match VdbImporter::writeMesh() exactly.
            float constexpr kFarFiVoxelSize_mm  = 1.0f;   // far  face-index: 1mm voxels
            float constexpr kNearFiVoxelSize_mm = 0.2f;   // near face-index: 0.2mm voxels (= 1/5 mm)
            float constexpr kFarFiBandVoxels    = 150.0f; // 150mm world band
            float constexpr kNearFiBandVoxels   = 50.0f;  // 50 voxels = 10mm world band

            try
            {
                // Build flat (non-indexed) world-space vertex and triangle lists for OpenVDB.
                std::vector<openvdb::Vec3s> verts;
                std::vector<openvdb::Vec3I> tris;
                verts.reserve(m_data.triangles.size() * 3u);
                tris.reserve(m_data.triangles.size());
                for (auto const & tri : m_data.triangles)
                {
                    auto const base = static_cast<openvdb::Index32>(verts.size());
                    verts.emplace_back(tri.v0.x, tri.v0.y, tri.v0.z);
                    verts.emplace_back(tri.v1.x, tri.v1.y, tri.v1.z);
                    verts.emplace_back(tri.v2.x, tri.v2.y, tri.v2.z);
                    tris.emplace_back(base, base + 1u, base + 2u);
                }

                // Helper: convert an OpenVDB grid to NanoVDB, append bytes (32-byte aligned)
                // to m_payloadData.data, and return a PrimitiveMeta (not pushed yet).
                constexpr size_t NANOVDB_ALIGN = 32u;
                auto appendVdbGrid = [&](openvdb::GridBase::Ptr grid, float scaling,
                                         PrimitiveType primType) -> PrimitiveMeta
                {
                    auto handle = nanovdb::openToNanoVDB(grid);
                    size_t byteOffset = m_payloadData.data.size() * sizeof(float);
                    size_t pad = (NANOVDB_ALIGN - (byteOffset % NANOVDB_ALIGN)) % NANOVDB_ALIGN;
                    for (size_t k = 0; k < pad / sizeof(float); ++k)
                        m_payloadData.data.push_back(0.0f);

                    int const gridStart = static_cast<int>(m_payloadData.data.size());
                    size_t const nFloats = static_cast<size_t>(
                        std::ceil(static_cast<double>(handle.size()) / 4.0));
                    m_payloadData.data.resize(m_payloadData.data.size() + nFloats);
                    std::memcpy(&m_payloadData.data[gridStart], handle.data(), handle.size());

                    byteOffset = m_payloadData.data.size() * sizeof(float);
                    pad = (NANOVDB_ALIGN - (byteOffset % NANOVDB_ALIGN)) % NANOVDB_ALIGN;
                    for (size_t k = 0; k < pad / sizeof(float); ++k)
                        m_payloadData.data.push_back(0.0f);

                    PrimitiveMeta meta{};
                    meta.primitiveType = primType;
                    meta.scaling = scaling;
                    meta.start = gridStart;
                    meta.end = static_cast<int>(m_payloadData.data.size());
                    return meta;
                };

                // Mesh adaptor: getIndexSpacePoint returns index-space coordinates
                // (world_mm * invVoxelSize) as required by OpenVDB's meshToVolume.
                struct IndexSpaceAdapter
                {
                    std::vector<openvdb::Vec3s> const & vertices;
                    std::vector<openvdb::Vec3I> const & indices;
                    float invVoxelSize;
                    size_t polygonCount() const { return indices.size(); }
                    size_t pointCount() const { return vertices.size(); }
                    static size_t vertexCount(size_t) { return 3; }
                    void getIndexSpacePoint(size_t faceIdx, size_t v, openvdb::Vec3d & pos) const
                    {
                        auto const idx = indices[faceIdx][v];
                        auto const & pt = vertices[idx];
                        pos = {static_cast<double>(pt.x()) * invVoxelSize,
                               static_cast<double>(pt.y()) * invVoxelSize,
                               static_cast<double>(pt.z()) * invVoxelSize};
                    }
                };

                // -- Companion i+1: SDF_MESH_TRIANGLES (flat vertex buffer) --------------------
                PrimitiveMeta flatMeshCompanion{};
                flatMeshCompanion.primitiveType = SDF_MESH_TRIANGLES;
                flatMeshCompanion.start = static_cast<int>(m_payloadData.data.size());
                for (auto const & tri : m_data.triangles)
                {
                    m_payloadData.data.push_back(tri.v0.x);
                    m_payloadData.data.push_back(tri.v0.y);
                    m_payloadData.data.push_back(tri.v0.z);
                    m_payloadData.data.push_back(tri.v1.x);
                    m_payloadData.data.push_back(tri.v1.y);
                    m_payloadData.data.push_back(tri.v1.z);
                    m_payloadData.data.push_back(tri.v2.x);
                    m_payloadData.data.push_back(tri.v2.y);
                    m_payloadData.data.push_back(tri.v2.z);
                }
                flatMeshCompanion.end = static_cast<int>(m_payloadData.data.size());

                // -- Companion i+2: SDF_VDB (near signed-distance field, user-configured res) --
                auto const nearSdfTransform = openvdb::math::Transform::createLinearTransform(
                    static_cast<double>(voxelSize_mm));
                float constexpr kNearHalfBandVoxels = 8.0f;
                auto nearSdfGrid = openvdb::tools::meshToLevelSet<openvdb::FloatGrid>(
                    *nearSdfTransform, verts, tris, kNearHalfBandVoxels);
                openvdb::tools::changeBackground(nearSdfGrid->tree(),
                                                 std::numeric_limits<float>::max());
                nearSdfGrid->pruneGrid();
                PrimitiveMeta sdfVdbCompanion = appendVdbGrid(
                    openvdb::GridBase::Ptr(nearSdfGrid), 1.0f / voxelSize_mm, SDF_VDB);
                m_nanovdbGridOffset = static_cast<size_t>(sdfVdbCompanion.start);

                // -- Companion i+3: SDF_VDB_FACE_INDICES (far face-index, fixed 1mm / 150 voxels) --
                auto const farFiTransform = openvdb::math::Transform::createLinearTransform(
                    static_cast<double>(kFarFiVoxelSize_mm));
                IndexSpaceAdapter farAdaptor{verts, tris, 1.0f / kFarFiVoxelSize_mm};
                openvdb::Int32Grid::Ptr farFaceIdxGrid = std::make_shared<openvdb::Int32Grid>();
                farFaceIdxGrid->setTransform(farFiTransform);
                openvdb::tools::meshToVolume<openvdb::FloatGrid, IndexSpaceAdapter>(
                    farAdaptor, *farFiTransform, kFarFiBandVoxels, kFarFiBandVoxels, 0,
                    farFaceIdxGrid.get());
                openvdb::tools::changeBackground(farFaceIdxGrid->tree(), -1);
                farFaceIdxGrid->setTransform(farFiTransform);
                farFaceIdxGrid->pruneGrid();
                PrimitiveMeta farFaceIdxCompanion = appendVdbGrid(
                    openvdb::GridBase::Ptr(farFaceIdxGrid), 1.0f / kFarFiVoxelSize_mm,
                    SDF_VDB_FACE_INDICES);

                // -- Companion i+4: SDF_VDB_FACE_INDICES (near face-index, fixed 0.2mm / 50 voxels) --
                auto const nearFiTransform = openvdb::math::Transform::createLinearTransform(
                    static_cast<double>(kNearFiVoxelSize_mm));
                IndexSpaceAdapter nearFiAdaptor{verts, tris, 1.0f / kNearFiVoxelSize_mm};
                openvdb::Int32Grid::Ptr nearFaceIdxGrid = std::make_shared<openvdb::Int32Grid>();
                nearFaceIdxGrid->setTransform(nearFiTransform);
                openvdb::tools::meshToVolume<openvdb::FloatGrid, IndexSpaceAdapter>(
                    nearFiAdaptor, *nearFiTransform, kNearFiBandVoxels, kNearFiBandVoxels, 0,
                    nearFaceIdxGrid.get());
                openvdb::tools::changeBackground(nearFaceIdxGrid->tree(), -1);
                nearFaceIdxGrid->setTransform(nearFiTransform);
                nearFaceIdxGrid->pruneGrid();
                PrimitiveMeta nearFaceIdxCompanion = appendVdbGrid(
                    openvdb::GridBase::Ptr(nearFaceIdxGrid), 1.0f / kNearFiVoxelSize_mm,
                    SDF_VDB_FACE_INDICES);

                m_payloadData.data[m_headerStart + kNanoVdbOffsetIndex] =
                    static_cast<float>(m_nanovdbGridOffset);
                m_nanovdbBuildInfo.result = NanoVdbBuildResult::Built;

                metaData.end = static_cast<int>(m_payloadData.data.size());
                m_payloadData.meta.push_back(metaData);             // i+0: SDF_SPATIAL_MESH_ROOT
                m_payloadData.meta.push_back(flatMeshCompanion);    // i+1: SDF_MESH_TRIANGLES
                m_payloadData.meta.push_back(sdfVdbCompanion);      // i+2: SDF_VDB
                m_payloadData.meta.push_back(farFaceIdxCompanion);  // i+3: SDF_VDB_FACE_INDICES (far)
                m_payloadData.meta.push_back(nearFaceIdxCompanion); // i+4: SDF_VDB_FACE_INDICES (near)
                return;
            }
            catch (std::bad_alloc const & e)
            {
                m_payloadData.data.resize(nanoPayloadStart);
                m_nanovdbGridOffset = 0u;
                m_payloadData.data[m_headerStart + kNanoVdbOffsetIndex] = 0.0f;
                m_nanovdbBuildInfo.result = NanoVdbBuildResult::BuildFailed;
                m_nanovdbBuildInfo.reason =
                  fmt::format("host allocation failed while building NanoVDB grids: {}",
                              e.what());
                finalizeRootOnly();
                return;
            }
            catch (std::exception const & e)
            {
                m_payloadData.data.resize(nanoPayloadStart);
                m_nanovdbGridOffset = 0u;
                m_payloadData.data[m_headerStart + kNanoVdbOffsetIndex] = 0.0f;
                m_nanovdbBuildInfo.result = NanoVdbBuildResult::BuildFailed;
                m_nanovdbBuildInfo.reason =
                  fmt::format("OpenVDB/NanoVDB build failed: {}", e.what());
                finalizeRootOnly();
                return;
            }
        }

        metaData.end = static_cast<int>(m_payloadData.data.size());
        m_payloadData.meta.push_back(metaData);
    }

}  // namespace gladius
