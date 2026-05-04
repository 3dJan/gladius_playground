/// @file SpatialMeshResource.cpp
/// @brief Implementation of SpatialMeshResource for mesh SDF computation
/// @see SpatialMeshResource.h

#include "SpatialMeshResource.h"
#include "MeshVoxelGrid.h"
#include "Profiling.h"

#include <algorithm>
#include <cstring>

namespace gladius
{
    namespace
    {
        /// Convert int to float preserving bit pattern (for GPU interop)
        inline float intBitsToFloat(int value)
        {
            float result;
            std::memcpy(&result, &value, sizeof(float));
            return result;
        }
    }  // namespace
    // ========================================================================
    // Constructors
    // ========================================================================

    SpatialMeshResource::SpatialMeshResource(ResourceKey key, SpatialMeshData && data)
        : MeshResourceBase(std::move(key))
        , m_data(std::move(data))
    {
        ResourceBase::load();
    }

    SpatialMeshResource::SpatialMeshResource(ResourceKey key,
                                             SpatialMeshData && data,
                                             MeshSdfEvaluationConfig const & evaluationConfig)
        : MeshResourceBase(std::move(key))
        , m_data(std::move(data))
        , m_evaluationConfig(evaluationConfig)
    {
        ResourceBase::load();
    }

    SpatialMeshResource::SpatialMeshResource(ResourceKey key,
                                             std::span<float4 const> vertices,
                                             std::span<TriangleIndices const> indices)
        : MeshResourceBase(std::move(key))
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
            switch (cfg.method)
            {
            case MeshSdfMethod::PureBVH:
                return true;
            case MeshSdfMethod::FastWindingNumber:
                return payloadHasFwnSupport;
            case MeshSdfMethod::VoxelAccelerated:
                return m_voxelCount > 0;
            case MeshSdfMethod::NanoVDB:
                return false;
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
        // Header Layout (33 floats total):
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

        metaData.end = static_cast<int>(m_payloadData.data.size());
        m_payloadData.meta.push_back(metaData);
    }

}  // namespace gladius
