/**
 * @file LogView_tests.cpp
 * @brief Unit tests for LogView event formatting functions
 */

#include "EventLogger.h"

#include <gtest/gtest.h>
#include <chrono>
#include <regex>
#include <string>

namespace gladius::ui::tests
{
    /// Forward declaration of the formatting function.
    /// This is implemented in LogView.cpp as a namespace-level function for testability.
    std::string formatEventForClipboard(events::Event const& event);

    class LogViewFormatTest : public ::testing::Test
    {
      protected:
        /// Helper to create an event with known properties
        static events::Event createEvent(std::string const& msg, events::Severity severity)
        {
            return events::Event(msg, severity);
        }
    };

    TEST_F(LogViewFormatTest, FormatEventForClipboard_WithInfoSeverity_ContainsInfoLabel)
    {
        // Arrange
        auto event = createEvent("Test info message", events::Severity::Info);

        // Act
        auto result = formatEventForClipboard(event);

        // Assert
        EXPECT_TRUE(result.find("[INFO]") != std::string::npos)
            << "Expected [INFO] label in: " << result;
        EXPECT_TRUE(result.find("Test info message") != std::string::npos)
            << "Expected message in: " << result;
    }

    TEST_F(LogViewFormatTest, FormatEventForClipboard_WithWarningSeverity_ContainsWarningLabel)
    {
        // Arrange
        auto event = createEvent("Test warning message", events::Severity::Warning);

        // Act
        auto result = formatEventForClipboard(event);

        // Assert
        EXPECT_TRUE(result.find("[WARNING]") != std::string::npos)
            << "Expected [WARNING] label in: " << result;
        EXPECT_TRUE(result.find("Test warning message") != std::string::npos)
            << "Expected message in: " << result;
    }

    TEST_F(LogViewFormatTest, FormatEventForClipboard_WithErrorSeverity_ContainsErrorLabel)
    {
        // Arrange
        auto event = createEvent("Test error message", events::Severity::Error);

        // Act
        auto result = formatEventForClipboard(event);

        // Assert
        EXPECT_TRUE(result.find("[ERROR]") != std::string::npos)
            << "Expected [ERROR] label in: " << result;
        EXPECT_TRUE(result.find("Test error message") != std::string::npos)
            << "Expected message in: " << result;
    }

    TEST_F(LogViewFormatTest, FormatEventForClipboard_WithFatalSeverity_ContainsFatalLabel)
    {
        // Arrange
        auto event = createEvent("Test fatal message", events::Severity::FatalError);

        // Act
        auto result = formatEventForClipboard(event);

        // Assert
        EXPECT_TRUE(result.find("[FATAL]") != std::string::npos)
            << "Expected [FATAL] label in: " << result;
        EXPECT_TRUE(result.find("Test fatal message") != std::string::npos)
            << "Expected message in: " << result;
    }

    TEST_F(LogViewFormatTest, FormatEventForClipboard_HasIso8601TimestampFormat)
    {
        // Arrange
        auto event = createEvent("Test message", events::Severity::Info);

        // Act
        auto result = formatEventForClipboard(event);

        // Assert - Should match [YYYY-MM-DD HH:MM:SS] pattern
        std::regex timestampPattern(R"(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\])");
        EXPECT_TRUE(std::regex_search(result, timestampPattern))
            << "Expected ISO 8601 timestamp format in: " << result;
    }

    TEST_F(LogViewFormatTest, FormatEventForClipboard_MessagePreservesSpecialCharacters)
    {
        // Arrange - message with special characters
        auto event = createEvent("Error: file \"test.txt\" not found (code=42)", events::Severity::Error);

        // Act
        auto result = formatEventForClipboard(event);

        // Assert
        EXPECT_TRUE(result.find("Error: file \"test.txt\" not found (code=42)") != std::string::npos)
            << "Expected special characters preserved in: " << result;
    }

    TEST_F(LogViewFormatTest, FormatEventForClipboard_EmptyMessage_StillFormatsCorrectly)
    {
        // Arrange
        auto event = createEvent("", events::Severity::Warning);

        // Act
        auto result = formatEventForClipboard(event);

        // Assert - should have timestamp and severity, just empty message part
        EXPECT_TRUE(result.find("[WARNING]") != std::string::npos)
            << "Expected [WARNING] label even with empty message in: " << result;
        std::regex timestampPattern(R"(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\])");
        EXPECT_TRUE(std::regex_search(result, timestampPattern))
            << "Expected timestamp even with empty message in: " << result;
    }

} // namespace gladius::ui::tests
