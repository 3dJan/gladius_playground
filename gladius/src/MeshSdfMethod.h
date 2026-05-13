#pragma once

/// @file MeshSdfMethod.h
/// @brief Enumeration of available signed-distance-field evaluation strategies
///        for triangle meshes, plus runtime configuration parameters.
///
/// The selected method determines which acceleration structure the
/// @ref SpatialMeshResource builds and which kernel branch is dispatched in
/// `sdf.cl::SDF_SPATIAL_MESH_ROOT`.

#include "MeshRepair.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gladius
{
    /// Available mesh SDF evaluation strategies.
    enum class MeshSdfMethod : std::uint8_t
    {
        /// Pure BVH traversal (`spatialMeshSDFWithEarlyExit`). Lowest memory,
        /// works on every device. Uses optional early-exit hint from the raymarcher.
        PureBVH = 0,
        /// BVH with an additional voxel-grid acceleration cache
        /// (`spatialMeshSDF_VoxelAccelerated`). Higher memory cost; skips most
        /// of the BVH for far-from-surface queries.
        VoxelAccelerated = 1,
        /// NanoVDB-backed signed distance field. The mesh is rasterised into a
        /// narrow-band NanoVDB float grid at import time. GPU queries use trilinear
        /// interpolation inside the band and fall back to the voxel boundary value
        /// outside. Requires a device that supports NanoVDB (isVdbSupported()).
        NanoVDB = 2,
        /// Fast Winding Number (Barill et al., SIGGRAPH 2018). Uses a
        /// hierarchical Barnes-Hut sum of per-node multipole aggregates on
        /// the BVH for the sign and the standard closest-point traversal for
        /// the magnitude. Robust against open / non-manifold / inconsistently
        /// oriented triangle soups.
        FastWindingNumber = 3,
    };

    /// Convert a method enum to its persistent string name (used by ConfigManager).
    inline std::string_view toString(MeshSdfMethod m) noexcept
    {
        switch (m)
        {
        case MeshSdfMethod::PureBVH:          return "PureBVH";
        case MeshSdfMethod::VoxelAccelerated: return "VoxelAccelerated";
        case MeshSdfMethod::NanoVDB:          return "NanoVDB";
        case MeshSdfMethod::FastWindingNumber: return "FastWindingNumber";
        }
        return "PureBVH";
    }

    /// Parse a method name written by @ref toString. Unknown values fall back to
    /// @ref MeshSdfMethod::VoxelAccelerated for backwards compatibility and emit
    /// a one-line warning to stderr so misspelled config entries are noticed.
    inline MeshSdfMethod parseMeshSdfMethod(std::string_view s) noexcept
    {
        if (s == "PureBVH")          return MeshSdfMethod::PureBVH;
        if (s == "VoxelAccelerated") return MeshSdfMethod::VoxelAccelerated;
        if (s == "NanoVDB")          return MeshSdfMethod::NanoVDB;
        if (s == "FastWindingNumber") return MeshSdfMethod::FastWindingNumber;
        std::fprintf(stderr,
                     "[MeshSdfMethod] Unknown method '%.*s' in config; falling back to "
                     "VoxelAccelerated.\n",
                     static_cast<int>(s.size()),
                     s.data());
        return MeshSdfMethod::VoxelAccelerated;
    }

    /// Runtime-only policy for handling NanoVDB builds that are predicted to exceed the
    /// configured memory budget. This is intentionally separate from the persisted mesh-SDF
    /// settings: it reflects the caller context (interactive UI vs strict API load).
    enum class NanoVdbFailurePolicy : std::uint8_t
    {
        /// Keep loading and degrade explicitly when NanoVDB cannot be built.
        Degrade = 0,
        /// Treat NanoVDB rejection as a hard load error.
        Fail = 1,
    };

    /// Runtime-only NanoVDB build policy derived from the current caller context and compute
    /// device limits.
    struct NanoVdbBuildPolicy
    {
        /// Deterministic budget used for NanoVDB preflight checks. `0` means that no explicit
        /// device-derived budget was available and the resource should fall back to its internal
        /// conservative default.
        std::size_t budgetBytes = 0u;

        /// Behavior when the preflight rejects the requested NanoVDB build.
        NanoVdbFailurePolicy failurePolicy = NanoVdbFailurePolicy::Degrade;
    };

    /// User-facing aggregate of NanoVDB build issues currently present in loaded mesh resources.
    /// This is runtime-only state used by UI and API layers to present explicit recovery options.
    struct NanoVdbBuildIssueSummary
    {
        bool hasIssue = false;
        std::size_t affectedMeshCount = 0u;
        std::string message;
        float suggestedVoxelSize_mm = 0.0f;
    };

    /// User-facing aggregate of mesh topology diagnostics that can make signed-distance
    /// evaluation ambiguous unless the mesh is repaired or an orientation-independent
    /// sign strategy is selected.
    struct MeshQualityIssueSummary
    {
        bool hasIssue = false;
        std::size_t affectedMeshCount = 0u;
        std::size_t degenerateTriangleCount = 0u;
        std::size_t boundaryEdgeCount = 0u;
        std::size_t nonManifoldEdgeCount = 0u;
        std::string message;
    };

    /// Raised when a strict NanoVDB policy rejects the requested load/build.
    class NanoVdbBuildRejectedError : public std::runtime_error
    {
      public:
        using std::runtime_error::runtime_error;
    };

    /// Runtime parameters for mesh SDF evaluation. Some fields require a
    /// resource rebuild when changed (see @ref SpatialMeshResource), others are
    /// pure runtime knobs forwarded into `RenderingSettings`.
    struct MeshSdfEvaluationConfig
    {
        /// Selected acceleration strategy. **Rebuild trigger**: changing
        /// requires re-loading the affected mesh resources.
        MeshSdfMethod method = MeshSdfMethod::VoxelAccelerated;

        /// Allow the raymarcher to pass an early-exit hint to mesh BVH queries.
        /// **Runtime only**: written into `RenderingSettings.flags`.
        bool useEarlyExit = true;

        /// Distance (in mm) subtracted from every mesh-SDF reading after
        /// evaluation (morphological inflation). Closes pinholes and gaps up
        /// to ~2× this value at the cost of rounding sharp features.
        /// **Runtime only**: written into `RenderingSettings.meshInflationDistance`.
        float inflationDistance = 0.f;

        /// Linear voxel-grid resolution along the longest axis when
        /// @ref method is @ref MeshSdfMethod::VoxelAccelerated. Other axes
        /// scale to keep cubic voxels. Matches `kDefaultVoxelGridResolution`
        /// from `MeshVoxelGrid.h`. **Rebuild trigger**.
        int voxelGridResolution = 32;

        /// Barnes-Hut acceptance threshold for the Fast-Winding-Number method.
        /// Higher values are more accurate, lower values are faster.
        /// Near-surface robustness is additionally handled by a bounded
        /// exact-integration band in the kernel.
        /// Typical range: 1.5 – 4.0. **Runtime only**: forwarded into
        /// `RenderingSettings.meshFwnBeta`.
        float fwnBeta = 2.0f;

        /// Far-field skip factor for the Fast-Winding-Number method.
        /// Multiplied with the mesh's bbox half-diagonal to obtain the
        /// distance threshold beyond which the cheap closest-point sign is
        /// trusted and the expensive winding traversal is skipped.
        /// `0.0` disables the skip (always run winding — exact, slow).
        /// `0.5` is a safe default. Larger values are faster but may show
        /// sign artifacts on thin features. **Runtime only**: forwarded into
        /// `RenderingSettings.meshFwnFarFieldFactor`.
        float fwnFarFieldFactor = 0.5f;

        /// Enable the coarse 64³ FWN sign cache. When true, cells far enough
        /// from the surface are pre-classified inside/outside on the GPU after
        /// the aggregate build, and the kernel skips the winding traversal for
        /// those cells. Disable for debugging sign speckles. Changing this
        /// refreshes the primitive-buffer header but does not rebuild the mesh
        /// BVH payload.
        bool fwnUseSignCache = true;

        /// Voxel size (in mm) for the NanoVDB near-field SDF grid when
        /// @ref method is @ref MeshSdfMethod::NanoVDB. Smaller values resolve
        /// thinner features but use more memory. For L-PBF with 200 µm walls,
        /// 0.1 mm is recommended. **Rebuild trigger**.
        float nanovdbVoxelSize_mm = 0.1f;
    };

    /// Determine whether changing @p oldCfg → @p newCfg requires acceleration
    /// structures to be rebuilt. Pure runtime changes (inflation, early-exit
    /// toggle) return false.
    inline bool requiresMeshRebuild(MeshSdfEvaluationConfig const & oldCfg,
                                    MeshSdfEvaluationConfig const & newCfg) noexcept
    {
        return oldCfg.method != newCfg.method ||
               oldCfg.voxelGridResolution != newCfg.voxelGridResolution ||
               oldCfg.nanovdbVoxelSize_mm != newCfg.nanovdbVoxelSize_mm;
    }

} // namespace gladius
