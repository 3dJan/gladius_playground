#include "opencl_test_helper.h"
#include "testhelper.h"

#include <Document.h>
#include <compute/ComputeCore.h>
#include <io/3mf/Importer3mf.h>

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

} // namespace gladius_tests
