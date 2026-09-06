#pragma once

#if defined(GLADIUS_ENABLE_OPENVDB)
// Backward compatibility header - includes MeshResourceVdb
#include "MeshResourceVdb.h"

// MeshResource is now an alias for MeshResourceVdb (defined in MeshResourceVdb.h)
#else
// Without OpenVDB support, SpatialMeshResource (BVH-based) is the sole mesh
// resource implementation, so MeshResource aliases it instead.
#include "SpatialMeshResource.h"

namespace gladius
{
    using MeshResource = SpatialMeshResource;
} // namespace gladius
#endif