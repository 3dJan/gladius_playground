/**
 * @file MeshWriter3mf.h
 * @brief 3MF mesh exporter that uses only the core 3MF specification
 *
 * This writer exports meshes to 3MF format using only features from the core specification,
 * making the files compatible with any 3MF-compliant software. It does not use extensions
 * like volumetric or implicit functions.
 *
 * Supports optional per-face color assignment via the 3MF materials extension.
 */

#pragma once

#include "EventLogger.h"
#include "Mesh.h"
#include "io/3mf/FaceColors.h"
#include "io/3mf/Writer3mfBase.h"

#include <lib3mf_implicit.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gladius
{
    class Document;
    class ResourceKey;
} // namespace gladius

namespace gladius::io
{

    class MeshWriter3mf : public Writer3mfBase
    {
      public:
        explicit MeshWriter3mf(events::SharedLogger logger);

        void exportMesh(std::filesystem::path const & filePath,
                        Mesh const & mesh,
                        std::string const & meshName,
                        Document const * sourceDocument = nullptr,
                        bool writeThumbnail = false);

        /// Export mesh with per-face colors
        void exportMeshWithColors(std::filesystem::path const & filePath,
                                  Mesh const & mesh,
                                  std::string const & meshName,
                                  FaceColors const & faceColors,
                                  Document const * sourceDocument = nullptr,
                                  bool writeThumbnail = false);

        /// Export mesh with per-vertex colors (smooth color interpolation)
        void exportMeshWithVertexColors(std::filesystem::path const & filePath,
                                        Mesh const & mesh,
                                        std::string const & meshName,
                                        VertexColors const & vertexColors,
                                        Document const * sourceDocument = nullptr,
                                        bool writeThumbnail = false);

        void exportMeshes(std::filesystem::path const & filePath,
                          std::vector<std::pair<std::shared_ptr<Mesh>, std::string>> const & meshes,
                          Document const * sourceDocument = nullptr,
                          bool writeThumbnail = false);

        /// Export multiple meshes, each painted a solid color (material-based shell export)
        void exportMeshesWithMaterialColors(
            std::filesystem::path const & filePath,
            std::vector<std::tuple<std::shared_ptr<Mesh>, std::string, Eigen::Vector3f>> const & meshesWithColors,
            Document const * sourceDocument = nullptr,
            bool writeThumbnail = false);

        void exportMeshFromDocument(std::filesystem::path const & filePath,
                                    Document & document,
                                    ResourceKey const & resourceKey,
                                    bool writeThumbnail = true);

        bool validateMesh(Mesh const & mesh);

      private:
        Lib3MF::PMeshObject
        addMeshToModel(Lib3MF::PModel model3mf, Mesh const & mesh, std::string const & meshName);

        /// Add mesh with colors and return the mesh object and color group resource ID
        std::pair<Lib3MF::PMeshObject, Lib3MF_uint32>
        addMeshWithColorsToModel(Lib3MF::PModel model3mf,
                                 Mesh const & mesh,
                                 std::string const & meshName,
                                 FaceColors const & faceColors);

        /// Add mesh with per-vertex colors and return the mesh object and color group resource ID
        std::pair<Lib3MF::PMeshObject, Lib3MF_uint32>
        addMeshWithVertexColorsToModel(Lib3MF::PModel model3mf,
                                       Mesh const & mesh,
                                       std::string const & meshName,
                                       VertexColors const & vertexColors);

        void createBuildItem(Lib3MF::PModel model3mf,
                             Lib3MF::PMeshObject meshObject,
                             std::string const & partNumber);
    };

    void exportMeshTo3mfCore(std::filesystem::path const & filePath,
                             Mesh const & mesh,
                             std::string const & meshName,
                             Document const * sourceDocument,
                             events::SharedLogger logger);

} // namespace gladius::io
