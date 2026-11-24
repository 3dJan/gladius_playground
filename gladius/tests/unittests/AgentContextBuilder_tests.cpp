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

        /// @brief Byte-level signature of one message's content parts
        [[nodiscard]] std::string partsSignature(gladius::agent::Message const & msg)
        {
            nlohmann::json arr = nlohmann::json::array();
            for (auto const & part : msg.parts)
            {
                if (auto const * text = std::get_if<gladius::agent::TextContent>(&part))
                {
                    arr.push_back({{"t", "text"}, {"v", text->text}});
                }
                else if (auto const * image = std::get_if<gladius::agent::ImageContent>(&part))
                {
                    std::string hex;
                    for (auto const byte : image->data)
                    {
                        auto const value = static_cast<unsigned int>(byte);
                        hex += "0123456789abcdef"[value >> 4];
                        hex += "0123456789abcdef"[value & 0xFu];
                    }
                    arr.push_back({{"t", "image"}, {"v", hex}, {"m", image->mimeType}});
                }
                else if (auto const * call = std::get_if<gladius::agent::ToolCall>(&part))
                {
                    arr.push_back({{"t", "call"}, {"id", call->id}, {"n", call->name},
                                   {"a", call->arguments}});
                }
                else if (auto const * result = std::get_if<gladius::agent::ToolResult>(&part))
                {
                    arr.push_back({{"t", "result"}, {"id", result->callId}, {"v", result->value}});
                }
            }
            return arr.dump();
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

    TEST(ContextBuilder, HistoryPolicy_SentMessagesAreNeverRewritten)
    {
        // Arrange: a growing conversation with tool results and captures, the situation in which a
        // sliding window used to compact the oldest parts in place
        ContextBuilder builder;
        Conversation conversation;
        ToolRegistry registry;
        PrivacySettings privacy;
        std::vector<std::byte> const image{std::byte{0xAA}, std::byte{0xBB}};

        std::vector<std::string> previousSent;
        std::size_t const turns = 20;
        for (std::size_t turn = 0; turn < turns; ++turn)
        {
            conversation.appendAssistant("call", {makeCall("id" + std::to_string(turn))});
            conversation.appendToolResult("id" + std::to_string(turn),
                                          {{"success", true}, {"payload", std::string(300, 'x')}});
            if (turn % 4 == 0)
            {
                conversation.appendUserImage(std::vector<std::byte>(image), "image/png");
            }

            // Act
            auto const request = builder.build(conversation, registry, privacy, {});
            std::vector<std::string> sent;
            sent.reserve(request.messages.size());
            for (auto const & msg : request.messages)
            {
                sent.push_back(partsSignature(msg));
            }

            // Assert: everything the previous turn already sent must still be there, byte for byte,
            // in the same order. Anything else and the endpoint re-prefills the whole conversation.
            ASSERT_GE(sent.size(), previousSent.size()) << "turn " << turn;
            for (std::size_t i = 0; i < previousSent.size(); ++i)
            {
                EXPECT_EQ(sent[i], previousSent[i]) << "message " << i << " of turn " << turn
                                                    << " was rewritten";
            }
            // The live-state message is the varying block and must be last.
            sent.pop_back();
            previousSent = std::move(sent);
        }
    }

    TEST(ContextBuilder, HistoryPolicy_CapturesAndResultsStayVerbatim)
    {
        // Arrange: more captures and results than any keep window would allow
        ContextBuilder builder;
        Conversation conversation;
        std::vector<std::byte> const image{std::byte{1}, std::byte{2}};
        for (int i = 0; i < 4; ++i)
        {
            conversation.appendUserImage(std::vector<std::byte>(image), "image/png");
            conversation.appendAssistant("call", {makeCall("id" + std::to_string(i))});
            conversation.appendToolResult("id" + std::to_string(i), {{"success", true}, {"v", i}});
        }
        ToolRegistry registry;
        PrivacySettings privacy;

        // Act
        gladius::agent::ContextBuildStats stats;
        auto const request = builder.build(conversation, registry, privacy, {}, &stats);

        // Assert: nothing is compacted or stubbed, because rewriting sent bytes costs the prompt cache
        std::size_t images = 0;
        std::size_t stubs = 0;
        for (auto const & msg : request.messages)
        {
            for (auto const & part : msg.parts)
            {
                if (std::holds_alternative<gladius::agent::ImageContent>(part))
                {
                    ++images;
                }
                if (auto const * result = std::get_if<gladius::agent::ToolResult>(&part))
                {
                    EXPECT_FALSE(result->value.contains("compacted"));
                }
                if (auto const * text = std::get_if<gladius::agent::TextContent>(&part))
                {
                    stubs += text->text.find("dropped") != std::string::npos ? 1u : 0u;
                }
            }
        }
        EXPECT_EQ(images, 4u);
        EXPECT_EQ(stats.imagesKept, 4u);
        EXPECT_EQ(stubs, 0u);
    }

    TEST(ContextBuilder, HistoryPolicy_BudgetPrunesWholeExchangesFromTheFront)
    {
        // Arrange: a history well past the budget
        ContextBuilder builder;
        gladius::agent::ContextPolicy policy;
        policy.maxHistoryMessages = 12;
        policy.pruneChunkMessages = 8;
        builder.setContextPolicy(policy);

        Conversation conversation;
        std::vector<std::string> allIds;
        for (int i = 0; i < 10; ++i)
        {
            std::string const id = "id" + std::to_string(i);
            allIds.push_back(id);
            conversation.appendAssistant("call", {makeCall(id)});
            conversation.appendToolResult(id, {{"success", true}, {"v", i}});
        }
        ToolRegistry registry;
        PrivacySettings privacy;

        // Act
        gladius::agent::ContextBuildStats stats;
        auto const request = builder.build(conversation, registry, privacy, {}, &stats);

        // Assert: old exchanges are dropped whole, never half, and never rewritten
        EXPECT_GT(stats.messagesPruned, 0u);
        EXPECT_LT(stats.messagesSent, conversation.size());

        std::vector<std::string> keptResults;
        std::size_t toolCalls = 0;
        for (auto const & msg : request.messages)
        {
            for (auto const & part : msg.parts)
            {
                if (auto const * call = std::get_if<gladius::agent::ToolCall>(&part))
                {
                    ++toolCalls;
                    keptResults.push_back(call->id);
                }
                if (auto const * result = std::get_if<gladius::agent::ToolResult>(&part))
                {
                    EXPECT_FALSE(result->value.contains("compacted"));
                    EXPECT_FALSE(keptResults.empty()) << "tool result sent without its call";
                    EXPECT_EQ(keptResults.back(), result->callId)
                      << "tool result separated from its call";
                }
            }
        }

        // The newest exchange survives, otherwise the model loses what it just did
        ASSERT_FALSE(request.messages.empty());
        EXPECT_TRUE(std::any_of(
          request.messages.begin(), request.messages.end(),
          [&](gladius::agent::Message const & msg)
          {
              return std::any_of(msg.parts.begin(), msg.parts.end(),
                                 [&](gladius::agent::Content const & part)
                                 {
                                     auto const * r = std::get_if<gladius::agent::ToolResult>(&part);
                                     return r != nullptr && r->callId == allIds.back();
                                 });
          }));
    }

    TEST(ContextBuilder, HistoryPolicy_UnlimitedBudgetKeepsEverything)
    {
        // Arrange
        ContextBuilder builder;
        gladius::agent::ContextPolicy policy;
        policy.maxHistoryMessages = 0;
        builder.setContextPolicy(policy);
        Conversation conversation;
        for (int i = 0; i < 40; ++i)
        {
            conversation.appendAssistant("call", {makeCall("id" + std::to_string(i))});
            conversation.appendToolResult("id" + std::to_string(i), {{"success", true}});
        }
        ToolRegistry registry;
        PrivacySettings privacy;

        // Act
        gladius::agent::ContextBuildStats stats;
        (void)builder.build(conversation, registry, privacy, {}, &stats);

        // Assert
        EXPECT_EQ(stats.messagesPruned, 0u);
        EXPECT_EQ(stats.messagesSent, conversation.size());
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
