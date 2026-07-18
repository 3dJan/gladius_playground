#include "opencl_test_helper.h"
#include "testhelper.h"

#include <Document.h>
#include <SpatialMeshResource.h>
#include <compute/ComputeCore.h>
#include <io/3mf/Importer3mf.h>
#include <io/VdbImporter.h>

#include <chrono>
#include <fmt/core.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <string>

namespace gladius_tests
{
    using namespace gladius;

    /// @brief Test fixture for Document-level merge functionality.
    ///
    /// Reproduces the bug where merging a library file into an existing document
    /// silently fails to add new functions to the assembly, even though
    /// processImplicitFunction is called.
    class Importer3mfMerge_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            SKIP_IF_OPENCL_UNAVAILABLE();

            m_logger = std::make_shared<events::Logger>();
            auto context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
            if (!context->isValid())
            {
                GTEST_SKIP() << "OpenCL context not available";
            }

            m_core =
              std::make_shared<ComputeCore>(context, RequiredCapabilities::ComputeOnly, m_logger);
            m_doc = std::make_shared<Document>(m_core);
        }

        [[nodiscard]] std::filesystem::path createLargeBox3mf(float const width_mm,
                                                              float const height_mm,
                                                              float const depth_mm) const
        {
            auto sourceDoc = std::make_shared<Document>(m_core);
            sourceDoc->newEmptyModel();
            sourceDoc->addCustomBoxMesh(width_mm, height_mm, depth_mm);

            auto const uniqueSuffix =
              std::chrono::steady_clock::now().time_since_epoch().count();
            auto const tempPath = std::filesystem::temp_directory_path() /
                                  fmt::format("gladius_nanovdb_preflight_{}.3mf", uniqueSuffix);
            sourceDoc->saveAs(tempPath, false);
            return tempPath;
        }

          [[nodiscard]] std::filesystem::path createOpenQuad3mf() const
          {
            auto sourceDoc = std::make_shared<Document>(m_core);
            sourceDoc->newEmptyModel();

            vdb::TriangleMesh mesh;
            mesh.vertices = {
              openvdb::Vec3s{0.0f, 0.0f, 0.0f},
              openvdb::Vec3s{10.0f, 0.0f, 0.0f},
              openvdb::Vec3s{10.0f, 10.0f, 0.0f},
              openvdb::Vec3s{0.0f, 10.0f, 0.0f},
            };
            mesh.indices = {
              openvdb::Vec3I{0, 1, 2},
              openvdb::Vec3I{0, 2, 3},
            };

            sourceDoc->addMeshResource(std::move(mesh), "open quad");

            auto const uniqueSuffix =
              std::chrono::steady_clock::now().time_since_epoch().count();
            auto const tempPath = std::filesystem::temp_directory_path() /
                        fmt::format("gladius_nanovdb_open_quad_{}.3mf", uniqueSuffix);
            sourceDoc->saveAs(tempPath, false);
            return tempPath;
          }

        std::shared_ptr<Document> m_doc;
        std::shared_ptr<ComputeCore> m_core;
        events::SharedLogger m_logger;
    };

    TEST_F(Importer3mfMerge_Test, Merge_IntoLoadedDocument_AddsNewFunctions)
    {
        // Arrange: load a base document (the default template)
        m_doc->load("examples/template.3mf");
        auto assembly = m_doc->getAssembly();
        ASSERT_TRUE(assembly);

        auto const functionsBefore = assembly->getFunctions();
        auto const countBefore = functionsBefore.size();
        fmt::print("Functions BEFORE merge: {}\n", countBefore);
        for (auto const & [id, model] : functionsBefore)
        {
            auto name = model ? model->getDisplayName() : std::nullopt;
            fmt::print("  id={} name='{}'\n", id, name.value_or("(none)"));
        }

        // Record existing function IDs
        std::set<nodes::ResourceId> existingIds;
        for (auto const & [id, _] : functionsBefore)
        {
            existingIds.insert(id);
        }

        // Act: merge RadialRadiator.3mf (contains a "heatexchanger" function)
        m_doc->merge("testdata/RadialRadiator.3mf");

        // Assert: the assembly should have MORE functions than before
        auto const functionsAfter = assembly->getFunctions();
        auto const countAfter = functionsAfter.size();
        fmt::print("Functions AFTER merge: {}\n", countAfter);

        std::vector<std::string> newFunctionNames;
        for (auto const & [id, model] : functionsAfter)
        {
            auto name = model ? model->getDisplayName() : std::nullopt;
            bool isNew = existingIds.count(id) == 0;
            fmt::print("  id={} name='{}' isNew={}\n", id, name.value_or("(none)"), isNew);
            if (isNew && name.has_value())
            {
                newFunctionNames.push_back(*name);
            }
        }

        EXPECT_GT(countAfter, countBefore)
          << "Merge should add new functions to the assembly. "
          << "Before: " << countBefore << ", After: " << countAfter;

        // The heatexchanger function should be among the new functions
        bool hasHeatExchanger =
          std::find(newFunctionNames.begin(), newFunctionNames.end(), "heatexchanger") !=
          newFunctionNames.end();
        EXPECT_TRUE(hasHeatExchanger)
          << "The 'heatexchanger' function should be present after merge";
    }

    TEST_F(Importer3mfMerge_Test, Merge_IntoEmptyDocument_AddsFunctions)
    {
        // Arrange: new empty document
        m_doc->newModel();
        auto assembly = m_doc->getAssembly();
        ASSERT_TRUE(assembly);

        auto const countBefore = assembly->getFunctions().size();
        fmt::print("Functions before merge (empty doc): {}\n", countBefore);

        // Act: merge RadialRadiator.3mf
        m_doc->merge("testdata/RadialRadiator.3mf");

        // Assert
        auto const countAfter = assembly->getFunctions().size();
        fmt::print("Functions after merge (empty doc): {}\n", countAfter);
        for (auto const & [id, model] : assembly->getFunctions())
        {
            auto name = model ? model->getDisplayName() : std::nullopt;
            fmt::print("  id={} name='{}'\n", id, name.value_or("(none)"));
        }

        EXPECT_GT(countAfter, countBefore)
          << "Merge into empty document should add functions. "
          << "Before: " << countBefore << ", After: " << countAfter;
    }

      TEST_F(Importer3mfMerge_Test, Load_WithNanoVdb_BboxOnlyLevelSetMesh_IsNotLoadedAsResource)
      {
        MeshSdfEvaluationConfig meshEvaluationConfig;
        meshEvaluationConfig.method = MeshSdfMethod::NanoVDB;
        m_doc->setMeshSdfEvaluationConfig(meshEvaluationConfig);

        ASSERT_NO_THROW(m_doc->load("examples/template.3mf"));

        auto const model = m_doc->get3mfModel();
        ASSERT_TRUE(model);

        std::set<Lib3MF_uint32> bboxOnlyMeshIds;
        auto objectIterator = model->GetObjects();
        while (objectIterator->MoveNext())
        {
          auto const object = objectIterator->GetCurrentObject();
          if (!object->IsLevelSetObject())
          {
            continue;
          }

          auto const levelSet = model->GetLevelSetByID(object->GetUniqueResourceID());
          ASSERT_TRUE(levelSet);
          if (!levelSet->GetMeshBBoxOnly())
          {
            continue;
          }

          auto const mesh = levelSet->GetMesh();
          ASSERT_TRUE(mesh);
          bboxOnlyMeshIds.insert(mesh->GetModelResourceID());
        }

        ASSERT_FALSE(bboxOnlyMeshIds.empty())
          << "Test fixture expects the template to contain at least one bbox-only level set mesh";

        auto const & resourceManager = m_doc->getResourceManager();
        for (auto const meshId : bboxOnlyMeshIds)
        {
          EXPECT_FALSE(resourceManager.hasResource(ResourceKey{meshId, ResourceType::Mesh}))
            << "bbox-only level set mesh " << meshId
            << " must not be loaded as a SpatialMeshResource when NanoVDB is selected";
        }
      }

        TEST_F(Importer3mfMerge_Test,
               ImporterLoad_WithTinyNanoVdbBudget_KeepsMeshButMarksNanoVdbRejected)
        {
            auto const tempFile = createLargeBox3mf(600.0f, 600.0f, 600.0f);

            MeshSdfEvaluationConfig meshEvaluationConfig;
            meshEvaluationConfig.method = MeshSdfMethod::NanoVDB;
            meshEvaluationConfig.nanovdbVoxelSize_mm = 0.1f;

            io::Importer3mf importer(m_logger);
            importer.setMeshSdfEvaluationConfig(meshEvaluationConfig);
            importer.setNanoVdbBuildPolicy(
              NanoVdbBuildPolicy{1u * 1024u * 1024u, NanoVdbFailurePolicy::Degrade});

            m_doc->newEmptyModel();
            ASSERT_NO_THROW(importer.load(tempFile, *m_doc));

            bool foundRejectedMesh = false;
            for (auto const & [key, resource] : m_doc->getResourceManager().getResourceMap())
            {
                if (key.getResourceType() != ResourceType::Mesh)
                {
                    continue;
                }

                auto * spatialMesh = dynamic_cast<SpatialMeshResource *>(resource.get());
                ASSERT_NE(spatialMesh, nullptr);
                EXPECT_TRUE(spatialMesh->hasNanoVdbBuildIssue());
                EXPECT_EQ(spatialMesh->getNanoVdbBuildInfo().result,
                          SpatialMeshResource::NanoVdbBuildResult::PreflightRejected);
                EXPECT_EQ(spatialMesh->getNanoVdbBuildInfo().budgetBytes, 1u * 1024u * 1024u);
                foundRejectedMesh = true;
            }

            EXPECT_TRUE(foundRejectedMesh)
              << "Expected the synthetic large box mesh to trigger NanoVDB preflight rejection";

            auto const summary = m_doc->getNanoVdbBuildIssueSummary();
            EXPECT_TRUE(summary.hasIssue);
            EXPECT_EQ(summary.affectedMeshCount, 1u);
            EXPECT_NE(summary.message.find("NanoVDB unavailable"), std::string::npos);

            MeshSdfEvaluationConfig pureBvhConfig = meshEvaluationConfig;
            pureBvhConfig.method = MeshSdfMethod::PureBVH;
            for (auto const & [key, resource] : m_doc->getResourceManager().getResourceMap())
            {
                if (key.getResourceType() != ResourceType::Mesh)
                {
                    continue;
                }

                auto * spatialMesh = dynamic_cast<SpatialMeshResource *>(resource.get());
                ASSERT_NE(spatialMesh, nullptr);
                spatialMesh->setEvaluationConfig(pureBvhConfig);
            }

            auto const recoveredSummary = m_doc->getNanoVdbBuildIssueSummary();
            EXPECT_FALSE(recoveredSummary.hasIssue);
            EXPECT_EQ(recoveredSummary.affectedMeshCount, 0u);
            EXPECT_TRUE(recoveredSummary.message.empty());

            std::filesystem::remove(tempFile);
        }

        TEST_F(Importer3mfMerge_Test,
               ImporterLoad_WithOpenNanoVdbMesh_DegradeReportsMeshQualityIssue)
        {
            auto const tempFile = createOpenQuad3mf();

            MeshSdfEvaluationConfig meshEvaluationConfig;
            meshEvaluationConfig.method = MeshSdfMethod::NanoVDB;
            meshEvaluationConfig.nanovdbVoxelSize_mm = 0.1f;

            io::Importer3mf importer(m_logger);
            importer.setMeshSdfEvaluationConfig(meshEvaluationConfig);
            importer.setNanoVdbBuildPolicy(
              NanoVdbBuildPolicy{0u, NanoVdbFailurePolicy::Degrade});

            m_doc->newEmptyModel();
            ASSERT_NO_THROW(importer.load(tempFile, *m_doc));

            auto const summary = m_doc->getMeshQualityIssueSummary();
            EXPECT_TRUE(summary.hasIssue);
            EXPECT_EQ(summary.affectedMeshCount, 1u);
            EXPECT_GT(summary.boundaryEdgeCount, 0u);
            EXPECT_NE(summary.message.find("No repair or fallback was applied silently"),
                      std::string::npos);

            std::filesystem::remove(tempFile);
        }

        TEST_F(Importer3mfMerge_Test, DocumentLoad_WithNanoVdbHugeMesh_ThrowsRejectedError)
        {
            auto const tempFile = createLargeBox3mf(4000.0f, 4000.0f, 4000.0f);

            MeshSdfEvaluationConfig meshEvaluationConfig;
            meshEvaluationConfig.method = MeshSdfMethod::NanoVDB;
            meshEvaluationConfig.nanovdbVoxelSize_mm = 0.1f;
            m_doc->setMeshSdfEvaluationConfig(meshEvaluationConfig);

            EXPECT_THROW(m_doc->load(tempFile), NanoVdbBuildRejectedError);

            std::filesystem::remove(tempFile);
        }

    TEST_F(Importer3mfMerge_Test, DocumentLoad_WithNanoVdbOpenMesh_ThrowsRejectedError)
    {
      auto const tempFile = createOpenQuad3mf();

      MeshSdfEvaluationConfig meshEvaluationConfig;
      meshEvaluationConfig.method = MeshSdfMethod::NanoVDB;
      meshEvaluationConfig.nanovdbVoxelSize_mm = 0.1f;
      m_doc->setMeshSdfEvaluationConfig(meshEvaluationConfig);

      EXPECT_THROW(m_doc->load(tempFile), NanoVdbBuildRejectedError);

      std::filesystem::remove(tempFile);
    }

} // namespace gladius_tests
