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

    TEST(ContextBuilder, AgentRoleName_IsStableLowercaseIdentifier)
    {
        // Arrange / Act
        auto const defaultName = gladius::agent::agentRoleName(gladius::agent::AgentRole::Default);
        auto const componentName = gladius::agent::agentRoleName(gladius::agent::AgentRole::Component);

        // Assert: names are trace fields, so they must be lowercase and distinct
        EXPECT_STREQ(defaultName, "default");
        EXPECT_STREQ(componentName, "component");
        EXPECT_STREQ(gladius::agent::agentRoleName(gladius::agent::AgentRole::Requirements), "requirements");
        EXPECT_STREQ(gladius::agent::agentRoleName(gladius::agent::AgentRole::Decomposition), "decomposition");
        EXPECT_STREQ(gladius::agent::agentRoleName(gladius::agent::AgentRole::Review), "review");
    }

    TEST(ContextBuilder, ScopedSnapshot_RendersSubtreeSummary)
    {
        // Arrange
        ContextBuilder builder;
        Conversation conversation;
        conversation.appendUserText("Model the rim only.");
        ToolRegistry registry;
        PrivacySettings privacy;
        auto scoped = makeSnapshot("0/5 done");
        scoped.scopeNodeId = "n3";
        scoped.subtreeSummary = "- n3 Rim (Function, Proposed)\n  variant v7 Outer spoke\n";

        // Act
        auto const request = builder.build(conversation, registry, privacy, scoped);
        auto const live = liveContextText(request, request.messages.size() - 1);

        // Assert
        EXPECT_NE(live.find("Your component subtree:"), std::string::npos);
        EXPECT_NE(live.find("variant v7 Outer spoke"), std::string::npos);
    }

    TEST(ContextBuilder, ScopedSnapshot_ShowOnlyAssignedChecklist)
    {
        // Arrange
        ContextBuilder builder;
        Conversation conversation;
        conversation.appendUserText("Model the rim only.");
        ToolRegistry registry;
        PrivacySettings privacy;
        auto scoped = makeSnapshot("2/9 done");
        scoped.scopeNodeId = "n3";
        scoped.assignedChecklistSummary = "- [ ] tread period 2mm\n- [x] bore diameter 4.8mm\n";

        // Act
        auto const request = builder.build(conversation, registry, privacy, scoped);
        auto const live = liveContextText(request, request.messages.size() - 1);

        // Assert: own criteria in, orchestrator plan out
        EXPECT_NE(live.find("Assigned acceptance checklist"), std::string::npos);
        EXPECT_NE(live.find("tread period 2mm"), std::string::npos);
        EXPECT_EQ(live.find("2/9 done"), std::string::npos) << "orchestrator todos must be withheld";
    }

    TEST(ContextBuilder, DefaultRolePrompt_LegacyOverloadIsByteIdentical)
    {
        // Arrange / Act: the monolithic prompt is what the existing cache and bench baselines were
        // recorded against, so the role overload must not change it
        auto const legacy = ContextBuilder::defaultSystemPrompt(false);
        auto const viaRole = ContextBuilder::defaultSystemPrompt(gladius::agent::RolePrompt{});
        auto const legacyGeometry = ContextBuilder::defaultSystemPrompt(true);
        auto const geometryViaRole
          = ContextBuilder::defaultSystemPrompt(gladius::agent::RolePrompt{
            gladius::agent::AgentRole::Default, true});

        // Assert
        EXPECT_EQ(legacy, viaRole);
        EXPECT_EQ(legacyGeometry, geometryViaRole);
    }

    TEST(ContextBuilder, RolePrompts_AreDistinctAndNeverOwnFinishDesign)
    {
        // Arrange / Act
        std::vector<std::string> prompts;
        for (auto const role : {gladius::agent::AgentRole::Requirements,
                                gladius::agent::AgentRole::Decomposition,
                                gladius::agent::AgentRole::Component,
                                gladius::agent::AgentRole::Review})
        {
            prompts.push_back(ContextBuilder::defaultSystemPrompt(gladius::agent::RolePrompt{role, false}));
        }

        // Assert: one prompt per role, and only the orchestrator may complete the design
        for (std::size_t i = 0; i < prompts.size(); ++i)
        {
            EXPECT_NE(prompts[i].find("finish_design"), std::string::npos) << "role must be told not to finish";
            EXPECT_NE(prompts[i].find("Never call finish_design"), std::string::npos);
            for (std::size_t j = i + 1; j < prompts.size(); ++j)
            {
                EXPECT_NE(prompts[i], prompts[j]);
            }
        }
    }

    TEST(ContextBuilder, ComponentRolePrompt_CarriesSnippetRules)
    {
        // Arrange / Act: a component designer writes snippets on either surface, a reviewer does not
        auto const component
          = ContextBuilder::defaultSystemPrompt(gladius::agent::RolePrompt{gladius::agent::AgentRole::Component, true});
        auto const scopedComponent
          = ContextBuilder::defaultSystemPrompt(gladius::agent::RolePrompt{gladius::agent::AgentRole::Component, false});
        auto const review
          = ContextBuilder::defaultSystemPrompt(gladius::agent::RolePrompt{gladius::agent::AgentRole::Review, false});

        // Assert
        EXPECT_NE(component.find("SNIPPET LANGUAGE RULES"), std::string::npos);
        EXPECT_NE(scopedComponent.find("SNIPPET LANGUAGE RULES"), std::string::npos);
        EXPECT_EQ(review.find("SNIPPET LANGUAGE RULES"), std::string::npos);
    }

    TEST(ContextBuilder, ScopedSnapshot_WithholdsOrchestratorState)
    {
        // Arrange
        ContextBuilder builder;
        Conversation conversation;
        conversation.appendUserText("Model the rim only.");
        ToolRegistry registry;
        PrivacySettings privacy;
        auto scoped = makeSnapshot("2/5 done");
        scoped.scopeNodeId = "n3";
        scoped.inheritedContract = "- bore diameter 4.8mm, frame origin at tyre centre\n";

        // Act
        auto const request = builder.build(conversation, registry, privacy, scoped);
        auto const live = liveContextText(request, request.messages.size() - 1);

        // Assert: the component agent sees its contract, not the whole design
        EXPECT_NE(live.find("Assigned component: n3"), std::string::npos);
        EXPECT_NE(live.find("bore diameter 4.8mm"), std::string::npos);
        EXPECT_EQ(live.find("helper_sphere_9"), std::string::npos) << "design graph summary must be withheld";
        EXPECT_EQ(live.find("2/5 done"), std::string::npos) << "orchestrator todos must be withheld";
        EXPECT_EQ(live.find("remember: root is main_3"), std::string::npos);
    }

    TEST(ContextBuilder, ScopedAndUnscopedSnapshots_ShareOneSystemPrompt)
    {
        // Arrange
        ContextBuilder builder;
        Conversation conversation;
        conversation.appendUserText("Model the rim only.");
        ToolRegistry registry;
        PrivacySettings privacy;
        auto scoped = makeSnapshot("0/5 done");
        scoped.scopeNodeId = "n3";

        // Act
        auto const unscoped = builder.build(conversation, registry, privacy, makeSnapshot("0/5 done"));
        auto const request = builder.build(conversation, registry, privacy, scoped);

        // Assert: scoping is live state only, so the cached prefix stays valid for the whole run
        EXPECT_EQ(unscoped.systemPrompt, request.systemPrompt);
    }
} // namespace agent::tests
