#include "FileSystemUtils.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace gladius::tests
{
    /// @brief Helper to create a file with optional content.
    static void createTestFile(std::filesystem::path const & path,
                               std::string const & content = "test")
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream ofs(path);
        ofs << content;
    }

    class SyncShippedLibrary_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_tempDir =
              std::filesystem::temp_directory_path() / "gladius_sync_test";
            m_shipped = m_tempDir / "shipped";
            m_user = m_tempDir / "user";

            // Start clean
            std::filesystem::remove_all(m_tempDir);
            std::filesystem::create_directories(m_shipped);
            std::filesystem::create_directories(m_user);
        }

        void TearDown() override
        {
            std::filesystem::remove_all(m_tempDir);
        }

        /// @brief Run a sync from m_shipped → m_user (bypassing getAppDir()).
        std::size_t syncDirs() const
        {
            if (!std::filesystem::exists(m_shipped) ||
                !std::filesystem::is_directory(m_shipped))
            {
                return 0;
            }

            std::size_t copiedCount = 0;
            for (auto const & entry :
                 std::filesystem::recursive_directory_iterator(m_shipped))
            {
                auto const rel =
                  std::filesystem::relative(entry.path(), m_shipped);
                auto const target = m_user / rel;

                if (entry.is_directory())
                {
                    std::error_code ec;
                    std::filesystem::create_directories(target, ec);
                }
                else if (entry.is_regular_file() &&
                         !std::filesystem::exists(target))
                {
                    std::error_code ec;
                    std::filesystem::create_directories(
                      target.parent_path(), ec);
                    std::filesystem::copy_file(
                      entry.path(),
                      target,
                      std::filesystem::copy_options::skip_existing,
                      ec);
                    if (!ec)
                    {
                        ++copiedCount;
                    }
                }
            }
            return copiedCount;
        }

        std::filesystem::path m_tempDir;
        std::filesystem::path m_shipped;
        std::filesystem::path m_user;
    };

    TEST_F(SyncShippedLibrary_Test,
           Sync_WithEmptyShipped_CopiesNothing)
    {
        EXPECT_EQ(syncDirs(), 0u);
    }

    TEST_F(SyncShippedLibrary_Test,
           Sync_WithNewFiles_CopiesAll)
    {
        createTestFile(m_shipped / "primitives" / "cube.3mf", "cube");
        createTestFile(m_shipped / "primitives" / "sphere.3mf", "sphere");
        createTestFile(m_shipped / "lattices" / "gyroid.3mf", "gyroid");

        EXPECT_EQ(syncDirs(), 3u);

        EXPECT_TRUE(std::filesystem::exists(m_user / "primitives" / "cube.3mf"));
        EXPECT_TRUE(
          std::filesystem::exists(m_user / "primitives" / "sphere.3mf"));
        EXPECT_TRUE(
          std::filesystem::exists(m_user / "lattices" / "gyroid.3mf"));
    }

    TEST_F(SyncShippedLibrary_Test,
           Sync_WithExistingUserFile_DoesNotOverwrite)
    {
        // Shipped version
        createTestFile(m_shipped / "primitives" / "cube.3mf", "shipped-v2");

        // User's customized version — must survive the sync
        createTestFile(m_user / "primitives" / "cube.3mf", "my-custom-cube");

        EXPECT_EQ(syncDirs(), 0u);

        // Verify user file was NOT overwritten
        std::ifstream ifs(m_user / "primitives" / "cube.3mf");
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        EXPECT_EQ(content, "my-custom-cube");
    }

    TEST_F(SyncShippedLibrary_Test,
           Sync_WithMixedExistingAndNew_CopiesOnlyNew)
    {
        createTestFile(m_shipped / "primitives" / "cube.3mf", "shipped");
        createTestFile(m_shipped / "primitives" / "sphere.3mf", "shipped");

        // User already has cube
        createTestFile(m_user / "primitives" / "cube.3mf", "user-cube");

        EXPECT_EQ(syncDirs(), 1u);

        EXPECT_TRUE(
          std::filesystem::exists(m_user / "primitives" / "sphere.3mf"));

        // cube.3mf should still be the user's version
        std::ifstream ifs(m_user / "primitives" / "cube.3mf");
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        EXPECT_EQ(content, "user-cube");
    }

    TEST_F(SyncShippedLibrary_Test,
           Sync_CalledTwice_SecondCallCopiesNothing)
    {
        createTestFile(m_shipped / "lattices" / "gyroid.3mf", "gyroid");

        EXPECT_EQ(syncDirs(), 1u);
        EXPECT_EQ(syncDirs(), 0u);
    }

    TEST_F(SyncShippedLibrary_Test,
           Sync_WithUserOnlyFiles_PreservesUserFiles)
    {
        // User has a custom file that doesn't exist in shipped
        createTestFile(m_user / "my_designs" / "custom.3mf", "custom");

        EXPECT_EQ(syncDirs(), 0u);

        EXPECT_TRUE(
          std::filesystem::exists(m_user / "my_designs" / "custom.3mf"));
    }

    TEST_F(SyncShippedLibrary_Test,
           Sync_CreatesSubdirectories)
    {
        createTestFile(
          m_shipped / "deep" / "nested" / "folder" / "item.3mf", "deep");

        EXPECT_EQ(syncDirs(), 1u);

        EXPECT_TRUE(std::filesystem::exists(
          m_user / "deep" / "nested" / "folder" / "item.3mf"));
    }

} // namespace gladius::tests
