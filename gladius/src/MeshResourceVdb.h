#pragma once

#include "MeshResourceBase.h"
#include "io/VdbImporter.h"
#include "kernel/types.h"

namespace gladius
{
    /// Mesh resource using VDB triangle mesh representation
    class MeshResourceVdb : public MeshResourceBase
    {
      public:
        MeshResourceVdb(ResourceKey key, vdb::TriangleMesh && mesh)
            : MeshResourceBase(std::move(key))
            , m_mesh(std::move(mesh))
        {
        }

        /// Get the underlying VDB triangle mesh
        [[nodiscard]] vdb::TriangleMesh const & getMesh() const
        {
            return m_mesh;
        }

        // MeshResourceBase interface
        [[nodiscard]] BoundingBox getBoundingBox() const override
        {
            auto const & min = m_mesh.getMin();
            auto const & max = m_mesh.getMax();
            
            float4 minVec{};
            minVec.s[0] = min.x;
            minVec.s[1] = min.y;
            minVec.s[2] = min.z;
            minVec.s[3] = 0.0f;
            
            float4 maxVec{};
            maxVec.s[0] = max.x;
            maxVec.s[1] = max.y;
            maxVec.s[2] = max.z;
            maxVec.s[3] = 0.0f;
            
            return BoundingBox{minVec, maxVec};
        }

        [[nodiscard]] size_t getTriangleCount() const override
        {
            return m_mesh.polygonCount();
        }

        [[nodiscard]] std::string getMeshTypeName() const override
        {
            return "TriangleMesh";
        }

      private:
        vdb::TriangleMesh m_mesh;

        void loadImpl() override;
    };

    // Type alias for backward compatibility during migration
    using MeshResource = MeshResourceVdb;
} // namespace gladius
