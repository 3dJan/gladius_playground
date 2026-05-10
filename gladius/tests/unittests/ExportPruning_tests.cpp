#include "io/3mf/ResourceDependencyGraph.h"
#include "io/3mf/Lib3mfLoader.h"
#include "io/3mf/LibraryMetadata.h"

#include <gtest/gtest.h>
#include <lib3mf_implicit.hpp>

#include <algorithm>
#include <filesystem>
#include <unordered_set>

namespace gladius_tests
{
    using namespace gladius;
    namespace fs = std::filesystem;

    /// @brief Test fixture for export pruning and dependency graph behaviour
    /// with implicit functions that use FunctionCall nodes.
    ///
    /// Two groups of tests:
    /// 1. ResourceDependencyGraph correctly traces FunctionCall node references.
    /// 2. End-to-end export pruning via write → load → prune → reload cycle.
    class ExportPruning_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_wrapper = gladius::io::loadLib3mfScoped();
            ASSERT_TRUE(m_wrapper);
            m_logger = events::SharedLogger();

            m_tempDir = fs::temp_directory_path() / "gladius_export_pruning_tests";
            fs::create_directories(m_tempDir);
        }

        void TearDown() override
        {
            std::error_code ec;
            fs::remove_all(m_tempDir, ec);
        }

        /// Identifiers for resources created in the source model
        struct TestModelIds
        {
            Lib3MF_uint32 mainFunc{0};
            Lib3MF_uint32 helperFunc{0};
            Lib3MF_uint32 unrelatedFunc{0};
            Lib3MF_uint32 levelSet{0};
            Lib3MF_uint32 mesh{0};
        };

        /// Helper to create an identity transform for build items
        static Lib3MF::sTransform identityTransform()
        {
            Lib3MF::sTransform t{};
            t.m_Fields[0][0] = 1.0f;
            t.m_Fields[1][1] = 1.0f;
            t.m_Fields[2][2] = 1.0f;
            return t;
        }

        /// Creates a model with a FunctionCall dependency: mainFunc calls helperFunc.
        /// Also has an unrelated function and a mesh + build item chain.
        ///
        /// Dependency structure:
        ///   BuildItem → LevelSet → mainFunc --FunctionCall-→ helperFunc
        ///   unrelatedFunc (no references)
        std::pair<Lib3MF::PModel, TestModelIds> createModelWithFunctionCall()
        {
            auto model = m_wrapper->CreateModel();

            // helperFunc — simple, no dependencies
            auto helperFunc = model->AddImplicitFunction();
            helperFunc->SetDisplayName("HelperFunc");
            auto helperIn = helperFunc->AddInput("pos", "position",
                                                  Lib3MF::eImplicitPortType::Vector);
            auto helperOut = helperFunc->AddOutput("shape", "output",
                                                    Lib3MF::eImplicitPortType::Scalar);
            // Add a simple length node for a valid function body
            auto lengthNode = helperFunc->AddLengthNode("len", "length", "");
            helperFunc->AddLink(helperIn.get(), lengthNode->GetInputA().get());
            helperFunc->AddLink(lengthNode->GetOutputResult().get(), helperOut.get());

            // mainFunc — depends on helperFunc via FunctionCall
            auto mainFunc = model->AddImplicitFunction();
            mainFunc->SetDisplayName("MainFunc");
            auto mainIn = mainFunc->AddInput("pos", "position",
                                              Lib3MF::eImplicitPortType::Vector);
            auto mainOut = mainFunc->AddOutput("shape", "output",
                                                Lib3MF::eImplicitPortType::Scalar);

            // Reference helper via ResourceIdNode
            auto resIdNode = mainFunc->AddResourceIdNode("ref_helper",
                                                          "reference to helper", "");
            resIdNode->SetResource(helperFunc.get());

            // FunctionCall node that calls helper
            auto funcCallNode = mainFunc->AddFunctionCallNode("call_helper",
                                                               "call helper", "");
            // Connect the ResourceIdNode output → FunctionCallNode's function ID input
            auto funcIdInput = funcCallNode->GetInputFunctionID();
            auto resIdOutput = resIdNode->GetOutputValue();
            mainFunc->AddLink(resIdOutput.get(), funcIdInput.get());

            // unrelatedFunc — standalone, no references to or from
            auto unrelatedFunc = model->AddImplicitFunction();
            unrelatedFunc->SetDisplayName("UnrelatedFunc");
            unrelatedFunc->AddInput("pos", "position", Lib3MF::eImplicitPortType::Vector);
            unrelatedFunc->AddOutput("shape", "output", Lib3MF::eImplicitPortType::Scalar);

            // Mesh + LevelSet + BuildItem chain for mainFunc
            auto mesh = model->AddMeshObject();
            auto levelSet = model->AddLevelSet();
            levelSet->SetFunction(mainFunc.get());
            levelSet->SetMesh(mesh.get());
            model->AddBuildItem(levelSet.get(), identityTransform());

            TestModelIds ids;
            ids.mainFunc = mainFunc->GetModelResourceID();
            ids.helperFunc = helperFunc->GetModelResourceID();
            ids.unrelatedFunc = unrelatedFunc->GetModelResourceID();
            ids.levelSet = levelSet->GetResourceID();
            ids.mesh = mesh->GetResourceID();

            return {model, ids};
        }

        /// Writes a model to a temp file and returns the path.
        fs::path writeModelToTemp(Lib3MF::PModel const & model, std::string const & name)
        {
            auto path = m_tempDir / (name + ".3mf");
            auto writer = model->QueryWriter("3mf");
            writer->WriteToFile(path.string());
            return path;
        }

        /// Loads a 3MF file from disk into a new model.
        Lib3MF::PModel loadModel(fs::path const & path)
        {
            auto wrapper = Lib3MF::CWrapper::loadLibrary();
            auto model = wrapper->CreateModel();
            auto reader = model->QueryReader("3mf");
            reader->ReadFromFile(path.string());
            return model;
        }

        /// Counts implicit functions in a model.
        std::size_t countImplicitFunctions(Lib3MF::PModel const & model) const
        {
            std::size_t count = 0;
            auto funcIter = model->GetFunctions();
            while (funcIter->MoveNext())
            {
                ++count;
            }
            return count;
        }

        /// Collects display names of all implicit functions in a model.
        std::vector<std::string> getFunctionNames(Lib3MF::PModel const & model) const
        {
            std::vector<std::string> names;
            auto funcIter = model->GetFunctions();
            while (funcIter->MoveNext())
            {
                auto func = funcIter->GetCurrentFunction();
                auto implicitFunc = std::dynamic_pointer_cast<Lib3MF::CImplicitFunction>(func);
                if (implicitFunc)
                {
                    names.push_back(implicitFunc->GetDisplayName());
                }
            }
            return names;
        }

        /// Counts build items in a model.
        std::size_t countBuildItems(Lib3MF::PModel const & model) const
        {
            std::size_t count = 0;
            auto iter = model->GetBuildItems();
            while (iter->MoveNext())
            {
                ++count;
            }
            return count;
        }

        /// Counts total resources (all types) in a model.
        std::size_t countResources(Lib3MF::PModel const & model) const
        {
            std::size_t count = 0;
            auto iter = model->GetResources();
            while (iter->MoveNext())
            {
                ++count;
            }
            return count;
        }

        Lib3MF::PWrapper m_wrapper;
        events::SharedLogger m_logger;
        fs::path m_tempDir;
    };

    // ========================================================================
    // Fix A tests — ResourceDependencyGraph tracing FunctionCall references
    // ========================================================================

    TEST_F(ExportPruning_Test,
           BuildGraph_FunctionWithFunctionCallNode_DependencyOnCalledFunctionExists)
    {
        // Arrange: model where mainFunc calls helperFunc via FunctionCall node
        auto [model, ids] = createModelWithFunctionCall();

        io::ResourceDependencyGraph depGraph(model, m_logger);

        // Act
        depGraph.buildGraph();

        // Assert: mainFunc should depend on helperFunc
        auto const & graph = depGraph.getGraph();
        EXPECT_TRUE(graph.isDirectlyDependingOn(ids.mainFunc, ids.helperFunc))
          << "mainFunc should directly depend on helperFunc through FunctionCall node";
    }

    TEST_F(ExportPruning_Test,
           FindUnusedResources_FunctionCallDependency_CalledFunctionNotUnused)
    {
        // Arrange: BuildItem → LevelSet → mainFunc --FunctionCall-→ helperFunc
        // unrelatedFunc has no references from build items.
        auto [model, ids] = createModelWithFunctionCall();

        io::ResourceDependencyGraph depGraph(model, m_logger);
        depGraph.buildGraph();

        // Act
        auto unusedResources = depGraph.findUnusedResources();

        // Assert
        std::unordered_set<Lib3MF_uint32> unusedIds;
        for (auto const & res : unusedResources)
        {
            unusedIds.insert(res->GetModelResourceID());
        }

        EXPECT_TRUE(unusedIds.count(ids.unrelatedFunc) > 0)
          << "unrelatedFunc should be unused (not referenced by any build item chain)";
        EXPECT_FALSE(unusedIds.count(ids.mainFunc) > 0)
          << "mainFunc should NOT be unused (referenced transitively by build item)";
        EXPECT_FALSE(unusedIds.count(ids.helperFunc) > 0)
          << "helperFunc should NOT be unused (called by mainFunc via FunctionCall)";
        EXPECT_FALSE(unusedIds.count(ids.levelSet) > 0)
          << "levelSet should NOT be unused (directly referenced by build item)";
        EXPECT_FALSE(unusedIds.count(ids.mesh) > 0)
          << "mesh should NOT be unused (referenced by levelSet)";
    }

    TEST_F(ExportPruning_Test,
           GetAllRequiredResources_FunctionWithFunctionCall_IncludesCalledFunction)
    {
        // Arrange
        auto [model, ids] = createModelWithFunctionCall();
        io::ResourceDependencyGraph depGraph(model, m_logger);
        depGraph.buildGraph();

        auto mainResource = depGraph.getResourceById(ids.mainFunc);
        ASSERT_TRUE(mainResource) << "mainFunc resource should exist in the model";

        // Act
        auto required = depGraph.getAllRequiredResources(mainResource);

        // Assert
        std::unordered_set<Lib3MF_uint32> requiredIds;
        for (auto const & r : required)
        {
            requiredIds.insert(r->GetModelResourceID());
        }

        EXPECT_TRUE(requiredIds.count(ids.helperFunc) > 0)
          << "helperFunc should be a required resource of mainFunc (via FunctionCall)";
        EXPECT_FALSE(requiredIds.count(ids.unrelatedFunc) > 0)
          << "unrelatedFunc should NOT be a required resource of mainFunc";
    }

    // ========================================================================
    // Fix B tests — export pruning round-trip
    // ========================================================================

    TEST_F(ExportPruning_Test,
           ExportPrune_TaggedFunction_WrittenFileOnlyContainsTaggedAndDeps)
    {
        // Arrange: create model, tag mainFunc, write full model, then prune
        auto [model, ids] = createModelWithFunctionCall();

        // Tag mainFunc as the library function
        io::LibraryMetadata metadata;
        metadata.libraryFunctions = io::serializeResourceIds({ids.mainFunc});
        metadata.libraryDescription = "Test export";
        io::writeLibraryMetadata(model, metadata);

        // Write full model
        auto filePath = writeModelToTemp(model, "export_pruning_full");
        ASSERT_TRUE(fs::exists(filePath));

        // Verify full model has all functions
        {
            auto fullModel = loadModel(filePath);
            EXPECT_EQ(countImplicitFunctions(fullModel), 3u)
              << "Full model should have 3 functions before pruning";
        }

        // Act: prune — replicate what pruneExportedLibraryFile does
        {
            auto prunedModel = loadModel(filePath);

            io::ResourceDependencyGraph depGraph(prunedModel, m_logger);
            depGraph.buildGraph();

            // Compute closure for tagged function using UniqueResourceIDs
            std::unordered_set<Lib3MF_uint32> requiredSet;
            auto taggedRes = depGraph.getResourceById(ids.mainFunc);
            ASSERT_TRUE(taggedRes) << "MainFunc should exist in reloaded model";
            requiredSet.insert(taggedRes->GetResourceID());
            auto deps = depGraph.getAllRequiredResources(taggedRes);
            for (auto const & dep : deps)
            {
                requiredSet.insert(dep->GetResourceID());
            }

            // Remove build items not referencing the tagged function chain
            {
                std::vector<Lib3MF::PBuildItem> toRemove;
                auto buildItemIter = prunedModel->GetBuildItems();
                while (buildItemIter->MoveNext())
                {
                    auto bi = buildItemIter->GetCurrent();
                    auto objId = bi->GetObjectResourceID(); // UniqueResourceID
                    bool keep = (requiredSet.count(objId) > 0);
                    if (!keep)
                    {
                        auto objRes = prunedModel->GetResourceByID(objId);
                        if (objRes)
                        {
                            auto objDeps = depGraph.getAllRequiredResources(objRes);
                            for (auto const & d : objDeps)
                            {
                                if (requiredSet.count(d->GetResourceID()) > 0)
                                {
                                    keep = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (!keep)
                    {
                        toRemove.push_back(bi);
                    }
                }
                for (auto const & item : toRemove)
                {
                    prunedModel->RemoveBuildItem(item.get());
                }
            }

            // Remove unused resources
            io::ResourceDependencyGraph depGraph2(prunedModel, m_logger);
            depGraph2.buildGraph();
            auto unused = depGraph2.findUnusedResources();
            for (auto const & res : unused)
            {
                if (requiredSet.count(res->GetResourceID()) == 0)
                {
                    prunedModel->RemoveResource(res.get());
                }
            }

            // Rewrite
            auto writer = prunedModel->QueryWriter("3mf");
            writer->WriteToFile(filePath.string());
        }

        // Assert: reload and verify
        auto result = loadModel(filePath);
        auto funcNames = getFunctionNames(result);

        EXPECT_TRUE(std::find(funcNames.begin(), funcNames.end(), "MainFunc") != funcNames.end())
          << "Pruned file should contain MainFunc (tagged)";
        EXPECT_TRUE(std::find(funcNames.begin(), funcNames.end(), "HelperFunc") != funcNames.end())
          << "Pruned file should contain HelperFunc (dependency of MainFunc)";
        EXPECT_FALSE(
          std::find(funcNames.begin(), funcNames.end(), "UnrelatedFunc") != funcNames.end())
          << "Pruned file should NOT contain UnrelatedFunc (unrelated)";
    }

    TEST_F(ExportPruning_Test,
           ExportPrune_BuildItemRetained_WhenItTransitivelyReferencesTaggedFunction)
    {
        // Arrange: BuildItem → LevelSet → mainFunc → helperFunc
        // The build item should be kept because levelSet's deps include mainFunc.
        auto [model, ids] = createModelWithFunctionCall();
        auto filePath = writeModelToTemp(model, "export_keep_build_item");

        auto prunedModel = loadModel(filePath);
        io::ResourceDependencyGraph depGraph(prunedModel, m_logger);
        depGraph.buildGraph();

        // Build required set using UniqueResourceIDs
        std::unordered_set<Lib3MF_uint32> requiredSet;
        auto taggedRes = depGraph.getResourceById(ids.mainFunc);
        ASSERT_TRUE(taggedRes) << "MainFunc should exist in reloaded model";
        requiredSet.insert(taggedRes->GetResourceID());
        auto deps = depGraph.getAllRequiredResources(taggedRes);
        for (auto const & dep : deps)
        {
            requiredSet.insert(dep->GetResourceID());
        }

        // Act: check which build items would be kept
        std::size_t keptCount = 0;
        auto buildItemIter = prunedModel->GetBuildItems();
        while (buildItemIter->MoveNext())
        {
            auto bi = buildItemIter->GetCurrent();
            auto objId = bi->GetObjectResourceID(); // UniqueResourceID
            bool keep = (requiredSet.count(objId) > 0);
            if (!keep)
            {
                auto objRes = prunedModel->GetResourceByID(objId);
                if (objRes)
                {
                    auto objDeps = depGraph.getAllRequiredResources(objRes);
                    for (auto const & d : objDeps)
                    {
                        if (requiredSet.count(d->GetResourceID()) > 0)
                        {
                            keep = true;
                            break;
                        }
                    }
                }
            }
            if (keep)
            {
                ++keptCount;
            }
        }

        // Assert
        EXPECT_EQ(keptCount, 1u)
          << "The build item referencing levelSet (which depends on mainFunc) should be kept";
    }

    TEST_F(ExportPruning_Test,
           ExportPrune_LibraryMetadataPreserved_AfterPruning)
    {
        // Arrange
        auto [model, ids] = createModelWithFunctionCall();
        io::LibraryMetadata metadata;
        metadata.libraryFunctions = io::serializeResourceIds({ids.mainFunc});
        metadata.libraryDescription = "Preserved metadata test";
        io::writeLibraryMetadata(model, metadata);

        auto filePath = writeModelToTemp(model, "export_metadata_preserved");

        // Act: no pruning of metadata — just verify it survives write/load cycle
        auto reloaded = loadModel(filePath);
        auto readMeta = io::readLibraryMetadata(reloaded);

        // Assert
        ASSERT_TRUE(readMeta.has_value()) << "Library metadata should survive round-trip";
        EXPECT_EQ(readMeta->libraryDescription, "Preserved metadata test");
        auto taggedIds = io::parseResourceIds(readMeta->libraryFunctions);
        EXPECT_EQ(taggedIds.size(), 1u);
        EXPECT_EQ(taggedIds[0], ids.mainFunc);
    }

    TEST_F(ExportPruning_Test,
           ExportPrune_NoUnrelatedBuildItems_ExistInPrunedFile)
    {
        // Arrange: add a second unrelated build item for a plain mesh
        auto [model, ids] = createModelWithFunctionCall();

        // Add a second mesh + build item that is unrelated to mainFunc
        auto extraMesh = model->AddMeshObject();
        model->AddBuildItem(extraMesh.get(), identityTransform());

        // Verify we start with 2 build items
        EXPECT_EQ(countBuildItems(model), 2u) << "Model should have 2 build items initially";

        auto filePath = writeModelToTemp(model, "export_prune_extra_builditem");

        // Act: prune using UniqueResourceIDs consistently (matching the fixed production code)
        {
            auto prunedModel = loadModel(filePath);

            // Find MainFunc's ModelResourceID by name, then look up resource
            Lib3MF_uint32 mainFuncModelId = 0;
            {
                auto funcIter = prunedModel->GetFunctions();
                while (funcIter->MoveNext())
                {
                    auto func = funcIter->GetCurrentFunction();
                    auto implFunc = std::dynamic_pointer_cast<Lib3MF::CImplicitFunction>(func);
                    if (implFunc && implFunc->GetDisplayName() == "MainFunc")
                    {
                        mainFuncModelId = implFunc->GetModelResourceID();
                        break;
                    }
                }
            }
            ASSERT_NE(mainFuncModelId, 0u) << "MainFunc should exist in reloaded model";

            io::ResourceDependencyGraph depGraph(prunedModel, m_logger);
            depGraph.buildGraph();

            // Use UniqueResourceIDs in requiredSet (consistent with graph and build items)
            std::unordered_set<Lib3MF_uint32> requiredSet;
            auto mainFuncRes = depGraph.getResourceById(mainFuncModelId);
            ASSERT_TRUE(mainFuncRes) << "getResourceById should find MainFunc";
            requiredSet.insert(mainFuncRes->GetResourceID());
            auto deps = depGraph.getAllRequiredResources(mainFuncRes);
            for (auto const & dep : deps)
            {
                requiredSet.insert(dep->GetResourceID());
            }

            std::vector<Lib3MF::PBuildItem> toRemove;
            auto buildItemIter = prunedModel->GetBuildItems();
            while (buildItemIter->MoveNext())
            {
                auto bi = buildItemIter->GetCurrent();
                auto objId = bi->GetObjectResourceID(); // UniqueResourceID
                bool keep = requiredSet.count(objId) > 0;
                if (!keep)
                {
                    auto objRes = prunedModel->GetResourceByID(objId);
                    if (objRes)
                    {
                        auto objDeps = depGraph.getAllRequiredResources(objRes);
                        for (auto const & d : objDeps)
                        {
                            if (requiredSet.count(d->GetResourceID()) > 0)
                            {
                                keep = true;
                                break;
                            }
                        }
                    }
                }
                if (!keep)
                {
                    toRemove.push_back(bi);
                }
            }
            for (auto const & item : toRemove)
            {
                prunedModel->RemoveBuildItem(item.get());
            }

            // Remove unused resources
            io::ResourceDependencyGraph depGraph2(prunedModel, m_logger);
            depGraph2.buildGraph();
            auto unused = depGraph2.findUnusedResources();
            for (auto const & res : unused)
            {
                if (requiredSet.count(res->GetResourceID()) == 0)
                {
                    prunedModel->RemoveResource(res.get());
                }
            }

            auto writer = prunedModel->QueryWriter("3mf");
            writer->WriteToFile(filePath.string());
        }

        // Assert: the pruned file should have exactly 1 build item
        auto result = loadModel(filePath);
        EXPECT_EQ(countBuildItems(result), 1u)
          << "Pruned file should have 1 build item (the one referencing mainFunc's chain)";

        // And unrelatedFunc + extraMesh should be removed
        auto funcNames = getFunctionNames(result);
        EXPECT_FALSE(
          std::find(funcNames.begin(), funcNames.end(), "UnrelatedFunc") != funcNames.end())
          << "UnrelatedFunc should be removed";
    }

} // namespace gladius_tests
