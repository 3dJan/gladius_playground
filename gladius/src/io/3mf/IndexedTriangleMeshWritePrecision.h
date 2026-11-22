#pragma once

#include "io/IndexedTriangleMesh.h"

#include <cstddef>

namespace gladius::io
{
    /// @brief Number of decimals lib3mf uses when it serialises mesh coordinates.
    inline constexpr std::size_t k3mfVertexDecimalPrecision = 9U;

    /// @brief Snaps a coordinate to a value lib3mf writes and reads back unchanged.
    /// @details lib3mf quantises every coordinate while formatting it. For the default decimal
    ///          precision it writes the decimal text of @c float32(float32(value * 1e9) * 1e-9), so the
    ///          stored value can move by a whole unit in the last place of @c float32 of the scaled
    ///          value. On the lampbase export that shifted the cap layer by about 5e-7 mm and turned a
    ///          triangle with an area ratio of 1.7e-11 into one with 3.1e-19, which is degenerate. A
    ///          mesh that was verified before the write therefore no longer has to be valid after it.
    ///          Snapping to a fixed point of that conversion makes the written numbers equal to the
    ///          verified ones, so geometry verification describes the file that is produced.
    /// @param value Coordinate to snap. Non finite values are returned unchanged.
    /// @param decimalPrecision Number of decimals lib3mf is configured to write.
    [[nodiscard]] float snapCoordinateTo3mfWritePrecision(
        float value,
        std::size_t decimalPrecision = k3mfVertexDecimalPrecision);

    /// @brief Snaps all mesh positions to the write precision of the 3MF writer.
    /// @details Only positions are touched. Normals are never serialised, and the 3MF writer derives
    ///          shading from the geometry, so they are left alone.
    /// @param mesh Mesh snapped in place.
    /// @param decimalPrecision Number of decimals lib3mf is configured to write.
    /// @return Number of position components that changed.
    std::size_t snapMeshTo3mfWritePrecision(
        IndexedTriangleMesh& mesh,
        std::size_t decimalPrecision = k3mfVertexDecimalPrecision);
}
