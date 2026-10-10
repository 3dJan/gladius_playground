#pragma once

#include "IndexedTriangleMesh.h"

#include <filesystem>
#include <iosfwd>

namespace gladius::io
{
    /// @brief Write an indexed mesh as a standards-compliant binary STL stream.
    void writeIndexedMeshToBinaryStl(
        std::ostream & output,
        IndexedTriangleMesh const & mesh,
        IndexedTriangleMeshValidationPolicy policy = IndexedTriangleMeshValidationPolicy::Strict);

    /// @brief Write an indexed mesh as a standards-compliant binary STL file.
    void writeIndexedMeshToBinaryStl(
        std::filesystem::path const & filePath,
        IndexedTriangleMesh const & mesh,
        IndexedTriangleMeshValidationPolicy policy = IndexedTriangleMeshValidationPolicy::Strict);
} // namespace gladius::io