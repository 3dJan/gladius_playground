#include "io/3mf/LibraryMetadata.h"
#include "io/3mf/Lib3mfLoader.h"

#include <gtest/gtest.h>
#include <lib3mf_implicit.hpp>

#include <algorithm>
#include <filesystem>

namespace gladius_tests
{
    using namespace gladius;

    /// @brief Test fixture for selective import functionality.
    ///
    /// Creates a minimal 3MF source model with:
    /// - Function B (simple, no dependencies)
    /// - Function A (depends on B via ResourceIdNode)
    /// - Function C (unrelated, no dependencies)
    /// - Mesh object + build item (unrelated)
    class SelectiveImport_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_wrapper = gladius::io::loadLib3mfScoped();
            ASSERT_TRUE(m_wrapper);
            m_logger = events::SharedLogger();
        }

        /// Identifiers for resources created in the source model
        struct TestModelIds
        {
            Lib3MF_uint32 funcA_modelId{0};
            Lib3MF_uint32 funcB_modelId{0};
            Lib3MF_uint32 funcC_modelId{0};
            Lib3MF_uint32 mesh_modelId{0};
        };

        /// Creates a source model with functions A (depends on B), B, C, and a mesh.
        std::pair<Lib3MF::PModel, TestModelIds> createSourceModel()
        {
            auto model = m_wrapper->CreateModel();

            // Function B — simple, no dependencies
            auto funcB = model->AddImplicitFunction();
            funcB->SetDisplayName("LibFuncB");
            funcB->AddInput("pos", "position", Lib3MF::eImplicitPortType::Vector);
            funcB->AddOutput("shape", "output", Lib3MF::eImplicitPortType::Scalar);

            // Function A — depends on B via ResourceIdNode
            auto funcA = model->AddImplicitFunction();
            funcA->SetDisplayName("LibFuncA");
            funcA->AddInput("pos", "position", Lib3MF::eImplicitPortType::Vector);
            funcA->AddOutput("shape", "output", Lib3MF::eImplicitPortType::Scalar);
            auto resIdNode = funcA->AddResourceIdNode("ref_b", "Reference to B", "");
            resIdNode->SetResource(funcB.get());

            // Function C — unrelated, no dependencies
            auto funcC = model->AddImplicitFunction();
            funcC->SetDisplayName("LibFuncC");
            funcC->AddInput("pos", "position", Lib3MF::eImplicitPortType::Vector);
            funcC->AddOutput("shape", "output", Lib3MF::eImplicitPortType::Scalar);

            // Mesh + build item — unrelated
            auto mesh = model->AddMeshObject();
            auto identity = Lib3MF::sTransform{};
            identity.m_Fields[0][0] = 1.0f;
            identity.m_Fields[1][1] = 1.0f;
            identity.m_Fields[2][2] = 1.0f;
            model->AddBuildItem(mesh.get(), identity);

            TestModelIds ids;
            ids.funcA_modelId = funcA->GetModelResourceID();
            ids.funcB_modelId = funcB->GetModelResourceID();
            ids.funcC_modelId = funcC->GetModelResourceID();
            ids.mesh_modelId = mesh->GetModelResourceID();

            return {model, ids};
        }

        /// Creates a source model and stamps it with library metadata tagging funcA.
        std::pair<Lib3MF::PModel, TestModelIds> createSourceModelWithMetadata()
        {
            auto [model, ids] = createSourceModel();
            io::LibraryMetadata metadata;
            metadata.libraryFunctions = std::to_string(ids.funcA_modelId);
            metadata.libraryDescription = "Test library";
            io::writeLibraryMetadata(model, metadata);
            return {model, ids};
        }

        /// Creates a source model with metadata tagging funcA AND funcC.
        std::pair<Lib3MF::PModel, TestModelIds> createSourceModelWithMultipleFunctionsTagged()
        {
            auto [model, ids] = createSourceModel();
            io::LibraryMetadata metadata;
            metadata.libraryFunctions =
              std::to_string(ids.funcA_modelId) + ";" + std::to_string(ids.funcC_modelId);
            metadata.libraryDescription = "Multi-function library";
            io::writeLibraryMetadata(model, metadata);
            return {model, ids};
        }

        Lib3MF::PWrapper m_wrapper;
        events::SharedLogger m_logger;
    };

    // ---- computeSelectiveImportClosure tests ----

    TEST_F(SelectiveImport_Test, ComputeClosure_WithTaggedFunction_ContainsTaggedAndDependencies)
    {
        // Arrange
        auto [model, ids] = createSourceModel();
        std::vector<Lib3MF_uint32> taggedIds = {ids.funcA_modelId};

        // Act
        auto closure = io::computeSelectiveImportClosure(model, taggedIds, m_logger);

        // Assert
        ASSERT_TRUE(closure.has_value());
        EXPECT_TRUE(closure->count(ids.funcA_modelId) > 0)
          << "Closure should contain tagged function A";
        EXPECT_TRUE(closure->count(ids.funcB_modelId) > 0)
          << "Closure should contain dependency function B";
    }

    TEST_F(SelectiveImport_Test, ComputeClosure_WithTaggedFunction_ExcludesUnrelatedResources)
    {
        // Arrange
        auto [model, ids] = createSourceModel();
        std::vector<Lib3MF_uint32> taggedIds = {ids.funcA_modelId};

        // Act
        auto closure = io::computeSelectiveImportClosure(model, taggedIds, m_logger);

        // Assert
        ASSERT_TRUE(closure.has_value());
        EXPECT_FALSE(closure->count(ids.funcC_modelId) > 0)
          << "Closure should NOT contain unrelated function C";
        EXPECT_FALSE(closure->count(ids.mesh_modelId) > 0)
          << "Closure should NOT contain unrelated mesh";
    }

    TEST_F(SelectiveImport_Test, ComputeClosure_WithInvalidId_ReturnsNullopt)
    {
        // Arrange
        auto [model, ids] = createSourceModel();
        std::vector<Lib3MF_uint32> taggedIds = {99999}; // nonexistent

        // Act
        auto closure = io::computeSelectiveImportClosure(model, taggedIds, m_logger);

        // Assert
        EXPECT_FALSE(closure.has_value()) << "Should return nullopt for invalid resource ID";
    }

    TEST_F(SelectiveImport_Test, ComputeClosure_WithMultipleFunctions_ContainsAll)
    {
        // Arrange
        auto [model, ids] = createSourceModel();
        std::vector<Lib3MF_uint32> taggedIds = {ids.funcA_modelId, ids.funcC_modelId};

        // Act
        auto closure = io::computeSelectiveImportClosure(model, taggedIds, m_logger);

        // Assert
        ASSERT_TRUE(closure.has_value());
        EXPECT_TRUE(closure->count(ids.funcA_modelId) > 0) << "Should contain A";
        EXPECT_TRUE(closure->count(ids.funcB_modelId) > 0) << "Should contain B (dependency of A)";
        EXPECT_TRUE(closure->count(ids.funcC_modelId) > 0) << "Should contain C (explicitly tagged)";
    }

    TEST_F(SelectiveImport_Test, ComputeClosure_WithEmptyTagList_ReturnsNullopt)
    {
        // Arrange
        auto [model, ids] = createSourceModel();
        std::vector<Lib3MF_uint32> taggedIds = {};

        // Act
        auto closure = io::computeSelectiveImportClosure(model, taggedIds, m_logger);

        // Assert
        EXPECT_FALSE(closure.has_value()) << "Should return nullopt for empty tag list";
    }

    // ---- pruneSourceForImport tests ----

    /// Helper: writes a model with library metadata to a temp file.
    static std::filesystem::path writeTestLibraryFile(
      Lib3MF::PModel model,
      std::string const & name,
      Lib3MF_uint32 taggedId)
    {
        auto const path = std::filesystem::temp_directory_path() / name;
        io::LibraryMetadata metadata;
        metadata.libraryFunctions = io::serializeResourceIds({taggedId});
        metadata.libraryDescription = "test";
        io::writeLibraryMetadata(model, metadata);

        auto writer = model->QueryWriter("3mf");
        writer->WriteToFile(path.string());

        io::removeLibraryMetadata(model);
        return path;
    }

    /// Helper: writes a model with multiple tagged IDs.
    static std::filesystem::path writeTestLibraryFileMulti(
      Lib3MF::PModel model,
      std::string const & name,
      std::vector<Lib3MF_uint32> const & taggedIds)
    {
        auto const path = std::filesystem::temp_directory_path() / name;
        io::LibraryMetadata metadata;
        metadata.libraryFunctions = io::serializeResourceIds(taggedIds);
        metadata.libraryDescription = "test";
        io::writeLibraryMetadata(model, metadata);

        auto writer = model->QueryWriter("3mf");
        writer->WriteToFile(path.string());

        io::removeLibraryMetadata(model);
        return path;
    }

    TEST_F(SelectiveImport_Test, PruneSourceForImport_WithClosure_RemovesNonClosureResources)
    {
        // Arrange
        auto [model, ids] = createSourceModelWithMetadata();
        auto const path = writeTestLibraryFile(model, "prune_removes_nonclosure.3mf",
                                               ids.funcA_modelId);

        // Act
        auto result = io::pruneSourceForImport(path, m_logger);

        // Assert
        ASSERT_TRUE(result.has_value());

        // Count remaining implicit functions
        auto resources = (*result)->GetResources();
        int funcCount = 0;
        while (resources->MoveNext())
        {
            auto res = resources->GetCurrent();
            if (std::dynamic_pointer_cast<Lib3MF::CImplicitFunction>(res))
            {
                ++funcCount;
            }
        }
        EXPECT_EQ(funcCount, 2) << "Should have exactly 2 implicit functions (A and B)";

        std::filesystem::remove(path);
    }

    TEST_F(SelectiveImport_Test, PruneSourceForImport_WithClosure_RemovesBuildItems)
    {
        // Arrange
        auto [model, ids] = createSourceModelWithMetadata();
        auto const path = writeTestLibraryFile(model, "prune_removes_builditems.3mf",
                                               ids.funcA_modelId);

        // Act
        auto result = io::pruneSourceForImport(path, m_logger);

        // Assert
        ASSERT_TRUE(result.has_value());

        auto buildItems = (*result)->GetBuildItems();
        int itemCount = 0;
        while (buildItems->MoveNext())
        {
            ++itemCount;
        }
        EXPECT_EQ(itemCount, 0) << "All build items should be removed during pruning";

        std::filesystem::remove(path);
    }

    // ---- Full workflow: pruneSourceForImport → merge ----

    TEST_F(SelectiveImport_Test, MergeSelective_WithMetadata_ImportsOnlyTaggedFunction)
    {
        // Arrange — create source with metadata, create empty target
        auto [source, ids] = createSourceModelWithMetadata();
        auto const path = writeTestLibraryFile(source, "prune_merge_tagged.3mf",
                                               ids.funcA_modelId);

        auto target = m_wrapper->CreateModel();
        auto targetFunc = target->AddImplicitFunction();
        targetFunc->SetDisplayName("ExistingFunc");

        // Act — prune via file round-trip, then merge
        auto prunedModel = io::pruneSourceForImport(path, m_logger);
        ASSERT_TRUE(prunedModel.has_value());
        target->MergeFromModel(prunedModel->get());

        // Assert — target should have ExistingFunc + LibFuncA + LibFuncB (3 functions)
        auto resources = target->GetResources();
        std::vector<std::string> funcNames;
        while (resources->MoveNext())
        {
            auto res = resources->GetCurrent();
            auto implicitFunc = std::dynamic_pointer_cast<Lib3MF::CImplicitFunction>(res);
            if (implicitFunc)
            {
                funcNames.push_back(implicitFunc->GetDisplayName());
            }
        }

        EXPECT_EQ(funcNames.size(), 3u) << "Should have 3 functions total";
        EXPECT_TRUE(std::find(funcNames.begin(), funcNames.end(), "ExistingFunc") !=
                    funcNames.end())
          << "Existing function should be preserved";
        EXPECT_TRUE(std::find(funcNames.begin(), funcNames.end(), "LibFuncA") != funcNames.end())
          << "Tagged function A should be imported";
        EXPECT_TRUE(std::find(funcNames.begin(), funcNames.end(), "LibFuncB") != funcNames.end())
          << "Dependency function B should be imported";
        EXPECT_TRUE(std::find(funcNames.begin(), funcNames.end(), "LibFuncC") == funcNames.end())
          << "Unrelated function C should NOT be imported";

        std::filesystem::remove(path);
    }

    TEST_F(SelectiveImport_Test, MergeSelective_WithoutMetadata_FallsToFullMerge)
    {
        // Arrange — source without metadata, written to file
        auto [source, ids] = createSourceModel();
        auto const path = std::filesystem::temp_directory_path() / "prune_no_metadata.3mf";
        {
            auto writer = source->QueryWriter("3mf");
            writer->WriteToFile(path.string());
        }

        // Act
        auto result = io::pruneSourceForImport(path, m_logger);

        // Assert — no metadata means fallback (nullopt)
        EXPECT_FALSE(result.has_value()) << "Model without metadata should return nullopt";

        std::filesystem::remove(path);
    }

    TEST_F(SelectiveImport_Test, MergeSelective_WithInvalidFunctionId_FallsToFullMerge)
    {
        // Arrange — source with metadata pointing to nonexistent IDs
        auto [source, ids] = createSourceModel();
        auto const path = std::filesystem::temp_directory_path() / "prune_invalid_id.3mf";
        {
            io::LibraryMetadata metadata;
            metadata.libraryFunctions = "99999";
            io::writeLibraryMetadata(source, metadata);
            auto writer = source->QueryWriter("3mf");
            writer->WriteToFile(path.string());
            io::removeLibraryMetadata(source);
        }

        // Act
        auto result = io::pruneSourceForImport(path, m_logger);

        // Assert — invalid IDs → nullopt → full merge fallback
        EXPECT_FALSE(result.has_value()) << "Invalid ID should cause fallback to full merge";

        std::filesystem::remove(path);
    }

    TEST_F(SelectiveImport_Test, MergeSelective_WithMultipleFunctions_ImportsAll)
    {
        // Arrange
        auto [source, ids] = createSourceModelWithMultipleFunctionsTagged();
        auto const path = writeTestLibraryFileMulti(
          source, "prune_multi_func.3mf",
          {ids.funcA_modelId, ids.funcC_modelId});

        auto target = m_wrapper->CreateModel();

        // Act
        auto prunedModel = io::pruneSourceForImport(path, m_logger);
        ASSERT_TRUE(prunedModel.has_value());
        target->MergeFromModel(prunedModel->get());

        // Assert — target should have A, B, C (all tagged + deps)
        auto resources = target->GetResources();
        std::vector<std::string> funcNames;
        while (resources->MoveNext())
        {
            auto res = resources->GetCurrent();
            auto implicitFunc = std::dynamic_pointer_cast<Lib3MF::CImplicitFunction>(res);
            if (implicitFunc)
            {
                funcNames.push_back(implicitFunc->GetDisplayName());
            }
        }

        EXPECT_EQ(funcNames.size(), 3u) << "Should have A, B, and C";
        EXPECT_TRUE(std::find(funcNames.begin(), funcNames.end(), "LibFuncA") != funcNames.end());
        EXPECT_TRUE(std::find(funcNames.begin(), funcNames.end(), "LibFuncB") != funcNames.end());
        EXPECT_TRUE(std::find(funcNames.begin(), funcNames.end(), "LibFuncC") != funcNames.end());

        std::filesystem::remove(path);
    }

} // namespace gladius_tests
