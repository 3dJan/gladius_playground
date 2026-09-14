#include "ui/BrowserFileDropInbox.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace gladius::tests
{
    namespace
    {
        void createTestFile(std::filesystem::path const & path)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary);
            output << "test";
        }
    }

    class BrowserFileDropInbox_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_directory = std::filesystem::temp_directory_path() /
                          "gladius_browser_file_drop_inbox_test";
            std::filesystem::remove_all(m_directory);
        }

        void TearDown() override
        {
            std::filesystem::remove_all(m_directory);
        }

        std::filesystem::path m_directory;
    };

    TEST_F(BrowserFileDropInbox_Test,
           ClaimNext_WithMixedFiles_ReturnsCompleted3mfFilesInOrder)
    {
        createTestFile(m_directory / "b.3MF");
        createTestFile(m_directory / "a.3mf");
        createTestFile(m_directory / "waiting.3mf.part");
        createTestFile(m_directory / "notes.txt");
        std::filesystem::create_directories(m_directory / "folder.3mf");

        ui::BrowserFileDropInbox inbox(m_directory);

        EXPECT_EQ(inbox.claimNext(), m_directory / "a.3mf");
        EXPECT_EQ(inbox.claimNext(), m_directory / "b.3MF");
        EXPECT_FALSE(inbox.claimNext().has_value());
    }

    TEST_F(BrowserFileDropInbox_Test,
           ClaimNext_DoesNotReturnTheSameFileTwice)
    {
        createTestFile(m_directory / "model.3mf");
        ui::BrowserFileDropInbox inbox(m_directory);

        auto const firstClaim = inbox.claimNext();

        ASSERT_TRUE(firstClaim.has_value());
        EXPECT_EQ(firstClaim.value(), m_directory / "model.3mf");
        EXPECT_FALSE(inbox.claimNext().has_value());
    }

    TEST_F(BrowserFileDropInbox_Test,
           IsManagedPath_OnlyAcceptsPathsInsideInbox)
    {
        ui::BrowserFileDropInbox inbox(m_directory);

        EXPECT_TRUE(inbox.isManagedPath(m_directory / "model.3mf"));
        EXPECT_TRUE(inbox.isManagedPath(m_directory / "nested" / "model.3mf"));
        EXPECT_FALSE(inbox.isManagedPath(m_directory));
        EXPECT_FALSE(inbox.isManagedPath(m_directory.parent_path() / "model.3mf"));
        EXPECT_FALSE(inbox.isManagedPath(m_directory.parent_path() /
                                         (m_directory.filename().string() + "-other") /
                                         "model.3mf"));
    }
}
