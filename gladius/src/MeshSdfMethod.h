#pragma once

/// @file MeshSdfMethod.h
/// @brief Enumeration of available signed-distance-field evaluation strategies
///        for triangle meshes, plus runtime configuration parameters.
///
/// The selected method determines which acceleration structure the
/// @ref SpatialMeshResource builds and which kernel branch is dispatched in
/// `sdf.cl::SDF_SPATIAL_MESH_ROOT`.

#include "MeshRepair.h"

#include <cstdint>
#include <cstdio>
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
        /// Reserved for a future NanoVDB-based path. Not implemented in any
        /// kernel today; the UI surfaces this value as disabled.
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
        /// Typical range: 1.5 – 4.0. **Runtime only**: forwarded into
        /// `RenderingSettings.meshFwnBeta`.
        float fwnBeta = 2.0f;
    };

    /// Determine whether changing @p oldCfg → @p newCfg requires acceleration
    /// structures to be rebuilt. Pure runtime changes (inflation, early-exit
    /// toggle) return false.
    inline bool requiresMeshRebuild(MeshSdfEvaluationConfig const & oldCfg,
                                    MeshSdfEvaluationConfig const & newCfg) noexcept
    {
        return oldCfg.method != newCfg.method ||
               oldCfg.voxelGridResolution != newCfg.voxelGridResolution;
    }

} // namespace gladius
