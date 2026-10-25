#include "Document.h"
#include "opencl_test_helper.h"

#include <compute/ComputeCore.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

namespace gladius_tests
{
    using namespace gladius;

    class DocumentSaveIdentityTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            SKIP_IF_OPENCL_UNAVAILABLE();

            m_logger = std::make_shared<events::Logger>();
            m_context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
            if (!m_context->isValid())
            {
                GTEST_SKIP() << "OpenCL context not available";
            }

            m_core =
              std::make_shared<ComputeCore>(m_context, RequiredCapabilities::ComputeOnly, m_logger);
            m_document = std::make_shared<Document>(m_core);
        }

        events::SharedLogger m_logger;
        std::shared_ptr<ComputeContext> m_context;
        std::shared_ptr<ComputeCore> m_core;
        std::shared_ptr<Document> m_document;
    };

    TEST_F(DocumentSaveIdentityTest, RevisionChangeRejectsStaleSaveWithoutChangingFilename)
    {
        auto const snapshot = m_document->createSaveSnapshot();
        auto const target = std::filesystem::temp_directory_path() / "gladius_stale_save.3mf";

        m_document->markFileAsChanged();

        EXPECT_FALSE(m_document->completeSave(target,
                                              snapshot.documentIdentity,
                                              snapshot.version));
        EXPECT_FALSE(m_document->getCurrentAssemblyFilename().has_value());
        EXPECT_TRUE(m_document->isFileChanged());
    }

    TEST_F(DocumentSaveIdentityTest, ReplacedDocumentRejectsSaveFromPreviousIdentity)
    {
        auto const snapshot = m_document->createSaveSnapshot();
        auto const target = std::filesystem::temp_directory_path() / "gladius_replaced_save.3mf";
        auto const previousIdentity = snapshot.documentIdentity;

        m_document->newEmptyModel();

        EXPECT_NE(m_document->documentIdentity(), previousIdentity);
        EXPECT_FALSE(m_document->completeSave(target,
                                              snapshot.documentIdentity,
                                              snapshot.version));
        EXPECT_FALSE(m_document->getCurrentAssemblyFilename().has_value());
    }

    TEST_F(DocumentSaveIdentityTest, CurrentIdentityAndRevisionAcceptSave)
    {
        auto const snapshot = m_document->createSaveSnapshot();
        auto const target = std::filesystem::temp_directory_path() / "gladius_current_save.3mf";

        m_document->markFileAsChanged();
        auto const editedSnapshot = m_document->createSaveSnapshot();

        EXPECT_TRUE(m_document->completeSave(target,
                                             editedSnapshot.documentIdentity,
                                             editedSnapshot.version));
        ASSERT_TRUE(m_document->getCurrentAssemblyFilename().has_value());
        EXPECT_EQ(*m_document->getCurrentAssemblyFilename(), target);
        EXPECT_FALSE(m_document->isFileChanged());
    }

} // namespace gladius_tests
