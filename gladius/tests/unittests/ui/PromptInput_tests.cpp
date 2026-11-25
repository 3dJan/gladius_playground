/**
 * @file PromptInput_tests.cpp
 * @brief Unit tests for the ImGui-free PromptInput helpers
 */

#include "ui/PromptInput.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

        /// @brief Item spacing the default theme has in a headless frame
        constexpr float kItemSpacingX = 8.f;
        constexpr float kItemSpacingY = 4.f;
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

    TEST(ShouldShowPlaceholder, EmptyDraftWithPlaceholder_PlaceholderShown)
    {
        char const * placeholder = "What do you want to build?";
        EXPECT_TRUE(PromptInput::shouldShowPlaceholder("", placeholder));
    }

    TEST(ShouldShowPlaceholder, WhitespaceOrDisabledPlaceholder_PlaceholderHidden)
    {
        EXPECT_FALSE(PromptInput::shouldShowPlaceholder(" ", "What do you want to build?"));
        EXPECT_FALSE(PromptInput::shouldShowPlaceholder("", ""));
        EXPECT_FALSE(PromptInput::shouldShowPlaceholder("", nullptr));
    }

    TEST(ComputeToolbarLayout, WidePanel_CombosShareRowWithLeadingControls)
    {
        PromptInput::Config const config;

        auto const layout = PromptInput::computeToolbarLayout(500.f,
                                                              config.iconButtonSize,
                                                              config.leadingToolbarWidth,
                                                              config.minComboWidth,
                                                              kItemSpacingX);

        EXPECT_EQ(layout.rowCount, 1);
        EXPECT_FALSE(layout.wrapped);
        EXPECT_GT(layout.comboWidth, config.minComboWidth);
        // The row must not overflow: leading controls, both combos and their gaps.
        EXPECT_LE(layout.leadingWidth + layout.comboWidth * 2.f + kItemSpacingX * 3.f,
                  layout.hostWidth + kItemSpacingX);
    }

    TEST(ComputeToolbarLayout, NarrowPanel_CombosWrapOntoSecondRow)
    {
        PromptInput::Config const config;

        auto const layout = PromptInput::computeToolbarLayout(300.f,
                                                              config.iconButtonSize,
                                                              config.leadingToolbarWidth,
                                                              config.minComboWidth,
                                                              kItemSpacingX);

        EXPECT_EQ(layout.rowCount, 2);
        EXPECT_TRUE(layout.wrapped);
        // The wrapped row holds nothing but the combos, so they split the full host width.
        EXPECT_EQ(layout.comboWidth,
                  PromptInput::computeComboWidth(layout.hostWidth, 0.f, kItemSpacingX));
        EXPECT_GT(layout.comboWidth,
                  PromptInput::computeComboWidth(layout.hostWidth,
                                                 config.leadingToolbarWidth,
                                                 kItemSpacingX));
    }

    TEST(ComputeToolbarLayout, WidthAtComboMinimumThreshold_CombosStayOnOneRow)
    {
        PromptInput::Config const config;
        // hostWidth = leading + 2 * minComboWidth + 2 gaps -> exactly the minimum usable width.
        float const hostWidth = config.leadingToolbarWidth + config.minComboWidth * 2.f
                              + kItemSpacingX * 2.f;

        auto const atThreshold
          = PromptInput::computeToolbarLayout(hostWidth + config.iconButtonSize * 2.f + kItemSpacingX,
                                              config.iconButtonSize,
                                              config.leadingToolbarWidth,
                                              config.minComboWidth,
                                              kItemSpacingX);
        auto const onePointNarrower
          = PromptInput::computeToolbarLayout(hostWidth + config.iconButtonSize * 2.f + kItemSpacingX - 1.f,
                                              config.iconButtonSize,
                                              config.leadingToolbarWidth,
                                              config.minComboWidth,
                                              kItemSpacingX);

        EXPECT_EQ(atThreshold.rowCount, 1);
        EXPECT_FLOAT_EQ(atThreshold.comboWidth, config.minComboWidth);
        EXPECT_EQ(onePointNarrower.rowCount, 2);
        EXPECT_TRUE(onePointNarrower.wrapped);
    }

    TEST(ComputeComboWidth, NarrowHostWidth_FallsBackToOnePixel)
    {
        EXPECT_GT(PromptInput::computeComboWidth(10.f, 70.f, 12.f), 0.f);
    }

    TEST(ComputeCardHeight, AttachmentsAndWrappedRow_AddTheirRows)
    {
        float const baseline
          = PromptInput::computeCardHeight(52.f, 0.f, 30.f, 1, 8.f, 1.f, 6.f);
        float const withAttachments
          = PromptInput::computeCardHeight(52.f, 34.f, 30.f, 1, 8.f, 1.f, 6.f);
        float const withSecondRow
          = PromptInput::computeCardHeight(52.f, 0.f, 30.f, 2, 8.f, 1.f, 6.f);

        EXPECT_GT(baseline, 52.f);
        // Each extra row costs its own content height plus one vertical spacing.
        EXPECT_FLOAT_EQ(withAttachments - baseline, 34.f + 6.f);
        EXPECT_FLOAT_EQ(withSecondRow - baseline, 30.f + 6.f);
    }

    TEST(ComputeAttachmentStripHeight, ThumbnailsWrap_HeightsAddUpPerRow)
    {
        EXPECT_EQ(PromptInput::computeAttachmentStripHeight(0u, 34.f, 20.f, 130.f, kItemSpacingX), 0.f);
        // A cell is 54 wide: two fit into 130, the third wraps.
        EXPECT_FLOAT_EQ(PromptInput::computeAttachmentStripHeight(3u, 34.f, 20.f, 130.f, kItemSpacingX),
                        34.f * 2.f + kItemSpacingX);
        EXPECT_FLOAT_EQ(PromptInput::computeAttachmentStripHeight(2u, 34.f, 20.f, 130.f, kItemSpacingX),
                        34.f);
    }

    TEST(CountWrappedRows, ItemsWiderThanRow_CountsOneRowPerItem)
    {
        EXPECT_EQ(PromptInput::countWrappedRows({}, 100.f, kItemSpacingX), 0u);
        EXPECT_EQ(PromptInput::countWrappedRows({10.f, 10.f, 10.f}, 100.f, kItemSpacingX), 1u);
        EXPECT_EQ(PromptInput::countWrappedRows({60.f, 60.f, 60.f}, 100.f, kItemSpacingX), 3u);
        EXPECT_EQ(PromptInput::countWrappedRows({40.f, 40.f}, 10.f, kItemSpacingX), 2u);
    }

} // namespace gladius::ui::tests
