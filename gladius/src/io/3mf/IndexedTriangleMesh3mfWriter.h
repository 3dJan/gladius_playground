#pragma once

#include "../IndexedTriangleMesh.h"
#include "ColorQuantizer.h"
#include "ColorRegionizer.h"
#include "FaceColors.h"
#include "Writer3mfBase.h"

#include <filesystem>
#include <string>

namespace gladius::io
{
    /// @brief Writes a backend-neutral mesh as a geometry-only 3MF model.
    class IndexedTriangleMesh3mfWriter final : public Writer3mfBase
    {
      public:
        explicit IndexedTriangleMesh3mfWriter(events::SharedLogger logger = nullptr);

        void exportMesh(
            std::filesystem::path const & filePath,
            IndexedTriangleMesh const & mesh,
            std::string const & meshName = "Gladius Mesh",
            IndexedTriangleMeshValidationPolicy policy = IndexedTriangleMeshValidationPolicy::Strict);

        void exportMeshWithColors(std::filesystem::path const & filePath,
                      IndexedTriangleMesh const & mesh,
                      std::string const & meshName,
                      FaceColors const & faceColors);

        void exportMeshWithRegions(std::filesystem::path const & filePath,
                       IndexedTriangleMesh const & mesh,
                       std::string const & meshName,
                       QuantizedPalette const & palette,
                       std::vector<PrintableRegion> const & regions);

        void exportMeshWithMmuSegmentation(std::filesystem::path const & filePath,
                           IndexedTriangleMesh const & mesh,
                           std::string const & meshName,
                           QuantizedPalette const & palette);
    };
} // namespace gladius::io
