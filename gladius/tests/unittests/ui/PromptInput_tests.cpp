/**
 * @file PromptInput_tests.cpp
 * @brief Unit tests for the ImGui-free PromptInput helpers
 */

#include "ui/PromptInput.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace gladius::ui::tests
{
    namespace
    {
        /// @brief Writes text to a temporary file and returns its path
        std::filesystem::path writeTempFile(std::string const & name, std::string const & content)
        {
            auto const path = std::filesystem::temp_directory_path() / ("gladius_prompt_" + name);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << content;
            out.close();
            return path;
        }
    } // namespace

    TEST(CountVisualLines, ShortTextWithHardNewlines_CountsEachLine)
    {
        std::string const text = "alpha\nbeta\ngamma";

        EXPECT_EQ(PromptInput::countVisualLines(text, 40u), 3u);
    }

    TEST(CountVisualLines, TextEndingWithNewline_CountsTrailingEmptyLine)
    {
        std::string const text = "alpha\n";

        EXPECT_EQ(PromptInput::countVisualLines(text, 40u), 2u);
    }

    TEST(CountVisualLines, LongSingleLine_EstimatesWrappedLines)
    {
        std::string const text(90u, 'x');

        EXPECT_EQ(PromptInput::countVisualLines(text, 40u), 3u);
    }

    TEST(CountVisualLines, EmptyText_CountsNoLines)
    {
        EXPECT_EQ(PromptInput::countVisualLines("", 40u), 0u);
    }

    TEST(ComputeFieldHeight, TextBeyondMaxLines_ClampsToMaxHeight)
    {
        float const height = PromptInput::computeFieldHeight(50u, 16.f, 3.f, 52.f, 200.f);

        EXPECT_FLOAT_EQ(height, 200.f);
    }

    TEST(ComputeFieldHeight, FewLines_ClampsToMinHeight)
    {
        float const height = PromptInput::computeFieldHeight(1u, 16.f, 3.f, 52.f, 200.f);

        EXPECT_FLOAT_EQ(height, 52.f);
    }

    TEST(ComputeFieldHeight, ModerateLineCount_UsesContentHeight)
    {
        float const height = PromptInput::computeFieldHeight(5u, 16.f, 3.f, 52.f, 200.f);

        EXPECT_FLOAT_EQ(height, 5u * 16.f + 3.f * 2.f);
    }

    TEST(AppendTextFile, UnsupportedExtension_LeavesDraftUnchangedAndReportsError)
    {
        PromptInput prompt;
        prompt.setDraft("keep me");
        auto const path = writeTempFile("model.3mf", "binary-ish content");
        std::string error;

        bool const ok = prompt.appendTextFile(path, error);
        std::filesystem::remove(path);

        EXPECT_FALSE(ok);
        EXPECT_EQ(prompt.draft(), "keep me");
        EXPECT_NE(error.find("Unsupported file type"), std::string::npos);
    }

    TEST(AppendTextFile, OversizedFile_RejectsAndKeepsDraft)
    {
        PromptInput::Config config;
        config.maxImportBytes = 16u;
        PromptInput prompt(config);
        prompt.setDraft("keep me");
        auto const path = writeTempFile("huge.md", std::string(64u, 'x'));
        std::string error;

        bool const ok = prompt.appendTextFile(path, error);
        std::filesystem::remove(path);

        EXPECT_FALSE(ok);
        EXPECT_EQ(prompt.draft(), "keep me");
        EXPECT_NE(error.find("larger than"), std::string::npos);
    }

    TEST(AppendTextFile, BinaryFile_RejectedAsNotPlainText)
    {
        PromptInput prompt;
        auto const path = writeTempFile("binary.md", std::string("abc\0def", 7u));
        std::string error;

        bool const ok = prompt.appendTextFile(path, error);
        std::filesystem::remove(path);

        EXPECT_FALSE(ok);
        EXPECT_NE(error.find("not plain text"), std::string::npos);
    }

    TEST(AppendTextFile, MarkdownFile_AppendsWithSeparator)
    {
        PromptInput prompt;
        prompt.setDraft("Build a keycap.");
        auto const path = writeTempFile("instructions.md", "Use a 12mm base.\nRound the corners.\n\n");
        std::string error;

        bool const ok = prompt.appendTextFile(path, error);
        std::filesystem::remove(path);

        EXPECT_TRUE(ok);
        EXPECT_TRUE(error.empty());
        EXPECT_EQ(prompt.draft(), "Build a keycap.\nUse a 12mm base.\nRound the corners.");
    }

    TEST(AppendTextFile, EmptyDraft_UsesFileContentVerbatim)
    {
        PromptInput prompt;
        auto const path = writeTempFile("notes.txt", "first line\nsecond line");
        std::string error;

        bool const ok = prompt.appendTextFile(path, error);
        std::filesystem::remove(path);

        EXPECT_TRUE(ok);
        EXPECT_EQ(prompt.draft(), "first line\nsecond line");
    }

    TEST(AppendTextFile, MissingFile_ReportsReadError)
    {
        PromptInput prompt;
        std::string error;

        bool const ok = prompt.appendTextFile("/no/such/instructions.md", error);

        EXPECT_FALSE(ok);
        EXPECT_NE(error.find("Cannot read file"), std::string::npos);
    }

    TEST(TakeDraft, NonEmptyDraft_MovesOutAndClears)
    {
        PromptInput prompt;
        prompt.setDraft("hello");

        std::string const taken = prompt.takeDraft();

        EXPECT_EQ(taken, "hello");
        EXPECT_TRUE(prompt.draft().empty());
        EXPECT_EQ(prompt.draftBytes(), 0u);
    }

    TEST(IsSupportedTextFile, KnownInstructionExtensions_Accepted)
    {
        EXPECT_TRUE(PromptInput::isSupportedTextFile("notes.txt"));
        EXPECT_TRUE(PromptInput::isSupportedTextFile("notes.TXT"));
        EXPECT_TRUE(PromptInput::isSupportedTextFile("spec.markdown"));
        EXPECT_FALSE(PromptInput::isSupportedTextFile("model.3mf"));
        EXPECT_FALSE(PromptInput::isSupportedTextFile("photo.png"));
    }

    TEST(CountUtf8Characters, MultiByteText_CountsCodePoints)
    {
        EXPECT_EQ(PromptInput::countUtf8Characters("abc"), 3u);
        EXPECT_EQ(PromptInput::countUtf8Characters("\xc3\xa4\xc3\xb6"), 2u);
        EXPECT_EQ(PromptInput::countUtf8Characters("😀"), 1u);
    }
} // namespace gladius::ui::tests
