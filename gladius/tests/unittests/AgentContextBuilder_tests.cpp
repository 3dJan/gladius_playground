#include "agent/ContextBuilder.h"
#include "agent/Conversation.h"
#include "agent/PermissionPolicy.h"
#include "tools/ToolRegistry.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace agent::tests
{
    namespace
    {
        using gladius::agent::AutonomyLevel;
        using gladius::agent::ContextBuilder;
        using gladius::agent::Conversation;
        using gladius::agent::DesignContextSnapshot;
        using gladius::agent::PrivacySettings;
        using gladius::tools::ToolRegistry;

        [[nodiscard]] DesignContextSnapshot makeSnapshot(std::string const & todoState)
        {
            DesignContextSnapshot snap;
            snap.autonomy = AutonomyLevel::WorkAutonomously;
            snap.designPhase = "BuildModel";
            snap.requirementsSummary = "- r1 [geometry, mandatory] \"sphere r=5mm\"\n";
            snap.designGraphSummary = "functions: helper_sphere_9\n";
            snap.todoSummary = todoState;
            snap.knowledgeSummary = "remember: root is main_3\n";
            return snap;
        }

        [[nodiscard]] std::string liveContextText(
          gladius::agent::CompletionRequest const & request, std::size_t index)
        {
            auto const & msg = request.messages[index];
            std::string out;
            for (auto const & part : msg.parts)
            {
                if (auto const * text = std::get_if<gladius::agent::TextContent>(&part))
                {
                    out += text->text;
                }
            }
            return out;
        }
    } // namespace

    namespace
{
gladius::agent::ToolCall makeCall(std::string id)
{
    gladius::agent::ToolCall call;
    call.id = std::move(id);
    call.name = "tool";
    return call;
}
} // namespace

TEST(ContextBuilder, Build_VolatileSnapshotChanges_SystemPromptIsByteIdentical)
    {
        // Arrange: two snapshots that differ only in state that changes every turn
        ContextBuilder builder;
        Conversation conversation;
        conversation.appendUserText("Make a sphere with a 5mm radius.");
        ToolRegistry registry;
        PrivacySettings privacy;

        // Act
        auto const first = builder.build(conversation, registry, privacy, makeSnapshot("0/5 done"));
        auto const second
          = builder.build(conversation, registry, privacy, makeSnapshot("5/5 done, phase Complete"));

        // Assert: the cached prefix must not move, otherwise the provider re-prefills everything
        EXPECT_EQ(first.systemPrompt, second.systemPrompt);
        EXPECT_EQ(first.messages.size(), second.messages.size());
    }

    TEST(ContextBuilder, Build_VolatileSnapshot_LandsInFinalUserMessage)
    {
        // Arrange
        ContextBuilder builder;
        Conversation conversation;
        conversation.appendUserText("Make a sphere with a 5mm radius.");
        ToolRegistry registry;
        PrivacySettings privacy;

        // Act
        auto const request = builder.build(conversation, registry, privacy, makeSnapshot("2/5 done"));

        // Assert: the live state is the last message, so it extends the cache instead of breaking it
        auto const lastIndex = request.messages.size() - 1;
        ASSERT_EQ(request.messages[lastIndex].role, gladius::agent::MessageRole::User);
        auto const live = liveContextText(request, lastIndex);
        EXPECT_NE(live.find("2/5 done"), std::string::npos);
        EXPECT_NE(live.find("BuildModel"), std::string::npos);
        EXPECT_NE(live.find("sphere r=5mm"), std::string::npos);
        EXPECT_NE(live.find("helper_sphere_9"), std::string::npos);
        EXPECT_EQ(request.systemPrompt.find("2/5 done"), std::string::npos);
        EXPECT_EQ(request.systemPrompt.find("helper_sphere_9"), std::string::npos);
    }

    TEST(ContextBuilder, Build_NoSnapshot_LiveContextMessageIsStillLast)
    {
        // Arrange
        ContextBuilder builder;
        Conversation conversation;
        conversation.appendUserText("Make a sphere.");
        conversation.appendAssistant("done", {});
        ToolRegistry registry;
        PrivacySettings privacy;

        // Act
        auto const request = builder.build(conversation, registry, privacy, {});

        // Assert
        EXPECT_EQ(request.messages.size(), 3u);
        EXPECT_EQ(request.messages.back().role, gladius::agent::MessageRole::User);
    }

    TEST(ContextBuilder, Build_MultipleTurns_SystemPromptNeverChanges)
    {
        // Arrange: mimic a run where todos, phase and knowledge all grow
        ContextBuilder builder;
        Conversation conversation;
        ToolRegistry registry;
        PrivacySettings privacy;
        std::string systemPrompt;

        // Act / Assert
        for (std::size_t turn = 0; turn < 6; ++turn)
        {
            conversation.appendUserText("step " + std::to_string(turn));
            conversation.appendAssistant("did step " + std::to_string(turn), {});
            auto const snap = makeSnapshot(std::to_string(turn) + "/6 done");
            auto const request = builder.build(conversation, registry, privacy, snap);
            if (turn == 0)
            {
                systemPrompt = request.systemPrompt;
            }
            else
            {
                EXPECT_EQ(request.systemPrompt, systemPrompt) << "turn " << turn;
            }
        }
    }

    TEST(ContextBuilder, ImagePolicy_OlderCapturesBecomeTextStubs)
    {
        // Arrange: three viewport captures, the only cost driver being how many are kept
        ContextBuilder builder;
        Conversation conversation;
        conversation.appendUserText("Make a sphere.");
        std::vector<std::byte> const tiny{std::byte{1}, std::byte{2}};
        conversation.appendUserImage(std::vector<std::byte>(tiny), "image/png");
        conversation.appendUserText("first view");
        conversation.appendUserImage(std::vector<std::byte>(tiny), "image/png");
        conversation.appendUserText("second view");
        conversation.appendUserImage(std::vector<std::byte>(tiny), "image/png");
        ToolRegistry registry;
        PrivacySettings privacy;

        // Act
        gladius::agent::ContextBuildStats stats;
        auto const request = builder.build(conversation, registry, privacy, {}, &stats);

        // Assert: only the newest capture survives; older ones leave a marker so the model still
        // knows it looked at a view that is no longer attached
        EXPECT_EQ(stats.imagesKept, 1u);
        EXPECT_EQ(stats.imagesDropped, 2u);
        std::size_t imageParts = 0;
        std::size_t stubParts = 0;
        for (auto const & msg : request.messages)
        {
            for (auto const & part : msg.parts)
            {
                if (std::holds_alternative<gladius::agent::ImageContent>(part))
                {
                    ++imageParts;
                }
                if (auto const * text = std::get_if<gladius::agent::TextContent>(&part))
                {
                    stubParts += text->text.find("capture dropped") != std::string::npos ? 1u : 0u;
                }
            }
        }
        EXPECT_EQ(imageParts, 1u);
        EXPECT_EQ(stubParts, 2u);
    }

    TEST(ContextBuilder, ImagePolicy_NewestCaptureIsTheOneKept)
    {
        // Arrange
        ContextBuilder builder;
        Conversation conversation;
        std::vector<std::byte> const oldImage{std::byte{0xAA}, std::byte{0xAA}};
        std::vector<std::byte> const newImage{std::byte{0xBB}};
        conversation.appendUserImage(std::vector<std::byte>(oldImage), "image/png");
        conversation.appendUserImage(std::vector<std::byte>(newImage), "image/png");
        ToolRegistry registry;
        PrivacySettings privacy;

        // Act
        auto const request = builder.build(conversation, registry, privacy, {});

        // Assert
        std::vector<std::byte> kept;
        for (auto const & msg : request.messages)
        {
            for (auto const & part : msg.parts)
            {
                if (auto const * image = std::get_if<gladius::agent::ImageContent>(&part))
                {
                    kept = image->data;
                }
            }
        }
        ASSERT_EQ(kept.size(), 1u);
        EXPECT_EQ(kept[0], std::byte{0xBB});
    }

    TEST(ContextBuilder, ToolResultPolicy_OlderResultsCompactedFailuresKept)
    {
        // Arrange: more successful results than the keep window, plus one failure
        ContextBuilder builder;
        Conversation conversation;
        for (int i = 0; i < 9; ++i)
        {
            conversation.appendAssistant("call", {makeCall("id" + std::to_string(i))});
            conversation.appendToolResult("id" + std::to_string(i),
                                          {{"success", true}, {"payload", std::string(400, 'x')}});
        }
        conversation.appendAssistant("call", {makeCall("bad")});
        conversation.appendToolResult("bad", {{"success", false}, {"error", "bad argument"}});
        ToolRegistry registry;
        PrivacySettings privacy;

        // Act
        gladius::agent::ContextBuildStats stats;
        auto const request = builder.build(conversation, registry, privacy, {}, &stats);

        // Assert: an error stays verbatim because it is the only thing that tells the model what to fix
        std::size_t compacted = 0;
        bool failureKept = false;
        for (auto const & msg : request.messages)
        {
            for (auto const & part : msg.parts)
            {
                auto const * result = std::get_if<gladius::agent::ToolResult>(&part);
                if (result == nullptr)
                {
                    continue;
                }
                compacted += result->value.contains("compacted") ? 1u : 0u;
                failureKept = failureKept || result->callId == "bad";
            }
        }
        EXPECT_GT(stats.toolResultsCompacted, 0u);
        EXPECT_EQ(stats.toolResultsCompacted, compacted);
        EXPECT_TRUE(failureKept);

        auto const failureIt = std::find_if(
          request.messages.begin(), request.messages.end(), [](auto const & msg)
          {
              return std::any_of(msg.parts.begin(), msg.parts.end(),
                                 [](auto const & part)
                                 {
                                     auto const * r = std::get_if<gladius::agent::ToolResult>(&part);
                                     return r != nullptr && r->callId == "bad";
                                 });
          });
        ASSERT_NE(failureIt, request.messages.end());
        for (auto const & part : failureIt->parts)
        {
            if (auto const * r = std::get_if<gladius::agent::ToolResult>(&part);
                r != nullptr && r->callId == "bad")
            {
                EXPECT_EQ(r->value.value("error", std::string{}), "bad argument");
            }
        }
    }

    TEST(ContextBuilder, ToolResultPolicy_RecentResultsAreUntouched)
    {
        // Arrange: fewer results than the keep window
        ContextBuilder builder;
        Conversation conversation;
        for (int i = 0; i < 2; ++i)
        {
            conversation.appendAssistant("call", {makeCall("id" + std::to_string(i))});
            conversation.appendToolResult("id" + std::to_string(i), {{"success", true}, {"v", i}});
        }
        ToolRegistry registry;
        PrivacySettings privacy;

        // Act
        gladius::agent::ContextBuildStats stats;
        (void)builder.build(conversation, registry, privacy, {}, &stats);

        // Assert
        EXPECT_EQ(stats.toolResultsCompacted, 0u);
        EXPECT_EQ(stats.toolResultsKept, 2u);
    }

    TEST(ContextBuilder, PrivacyStillApplies_ImagesFilteredOut)
    {
        // Arrange
        ContextBuilder builder;
        Conversation conversation;
        conversation.appendUserImage(std::vector<std::byte>{std::byte{1}}, "image/png");
        ToolRegistry registry;
        PrivacySettings privacy;
        privacy.allowViewportImages = false;
        privacy.allowReferenceImages = false;

        // Act
        gladius::agent::ContextBuildStats stats;
        auto const request = builder.build(conversation, registry, privacy, {}, &stats);

        // Assert
        EXPECT_EQ(stats.imagesKept, 0u);
        for (auto const & msg : request.messages)
        {
            for (auto const & part : msg.parts)
            {
                EXPECT_FALSE(std::holds_alternative<gladius::agent::ImageContent>(part));
            }
        }
    }
} // namespace agent::tests
