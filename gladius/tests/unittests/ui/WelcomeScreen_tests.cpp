/**
 * @file WelcomeScreen_tests.cpp
 * @brief Unit tests for WelcomeScreen file selection state machine
 *
 * Note: Full testing of trySetPendingFileOpen requires ImGui context for button clicks.
 * These tests verify the public API behavior and state management.
 */

#include "ui/WelcomeScreen.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace gladius::ui::tests
{
    class WelcomeScreenTest : public ::testing::Test
    {
      protected:
        WelcomeScreen m_welcomeScreen;

        void SetUp() override
        {
            // Welcome screen starts visible by default
        }

        /// Helper to create a temporary test file
        static std::filesystem::path createTempFile()
        {
            auto tempPath = std::filesystem::temp_directory_path() / "welcome_screen_test.3mf";
            std::ofstream file(tempPath);
            file << "test content";
            file.close();
            return tempPath;
        }

        /// Helper to cleanup temp file
        static void removeTempFile(std::filesystem::path const & path)
        {
            std::filesystem::remove(path);
        }
    };

    TEST_F(WelcomeScreenTest, InitialState_IsVisible)
    {
        // Arrange & Act - default state

        // Assert
        EXPECT_TRUE(m_welcomeScreen.isVisible());
    }

    TEST_F(WelcomeScreenTest, InitialState_HasNoPendingFileOpen)
    {
        // Arrange & Act - default state

        // Assert
        EXPECT_FALSE(m_welcomeScreen.hasPendingFileOpen());
    }

    TEST_F(WelcomeScreenTest, ProcessFileOpen_WhenNoPending_ReturnsEmpty)
    {
        // Arrange - default state, no pending file

        // Act
        auto result = m_welcomeScreen.processFileOpen();

        // Assert
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(WelcomeScreenTest, Hide_SetsNotVisible)
    {
        // Arrange
        EXPECT_TRUE(m_welcomeScreen.isVisible());

        // Act
        m_welcomeScreen.hide();

        // Assert
        EXPECT_FALSE(m_welcomeScreen.isVisible());
    }

    TEST_F(WelcomeScreenTest, Show_AfterHide_SetsVisible)
    {
        // Arrange
        m_welcomeScreen.hide();
        EXPECT_FALSE(m_welcomeScreen.isVisible());

        // Act
        m_welcomeScreen.show();

        // Assert
        EXPECT_TRUE(m_welcomeScreen.isVisible());
    }

    TEST_F(WelcomeScreenTest, ProcessFileOpen_ClearsValue_WhenCalledTwice)
    {
        // Arrange - Note: we can't set pending file without ImGui,
        // but we can verify that calling processFileOpen twice doesn't crash
        // and both calls return empty when nothing is pending

        // Act
        auto result1 = m_welcomeScreen.processFileOpen();
        auto result2 = m_welcomeScreen.processFileOpen();

        // Assert
        EXPECT_FALSE(result1.has_value());
        EXPECT_FALSE(result2.has_value());
    }

    TEST_F(WelcomeScreenTest, Hide_DoesNotAffectPendingFileOpen)
    {
        // Arrange - no pending file set

        // Act
        m_welcomeScreen.hide();

        // Assert - hide() should not affect pending file state
        EXPECT_FALSE(m_welcomeScreen.hasPendingFileOpen());
    }

    // Note: Testing trySetPendingFileOpen directly would require making it public
    // or creating an ImGui context. The core bug fix adds logging and validation
    // inside that private method, which is best verified through manual testing
    // or integration tests with the full UI.

} // namespace gladius::ui::tests
