#pragma once

#include "BBox.h"
#include "ResourceManager.h"

namespace gladius
{
    /// Base class for all mesh resource types
    /// @details Provides common interface for mesh resources regardless of internal representation
    ///          (VDB triangle mesh, BVH spatial mesh, etc.)
    class MeshResourceBase : public ResourceBase
    {
      public:
        explicit MeshResourceBase(ResourceKey key)
            : ResourceBase(std::move(key))
        {
        }

        ~MeshResourceBase() override = default;

        /// Get the mesh bounding box
        /// @return Bounding box encompassing all mesh geometry
        [[nodiscard]] virtual BoundingBox getBoundingBox() const = 0;

        /// Get triangle count
        /// @return Number of triangles in this mesh
        [[nodiscard]] virtual size_t getTriangleCount() const = 0;

        /// Get a display name indicating the mesh type
        /// @return String describing the mesh representation (e.g., "TriangleMesh", "SpatialMesh (BVH)")
        [[nodiscard]] virtual std::string getMeshTypeName() const = 0;
    };
} // namespace gladius
