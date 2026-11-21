#include "tools/ToolExecutor.h"
#include "tools/ToolRegistry.h"

#include <gtest/gtest.h>
#include <stdexcept>

namespace gladius::tools::tests
{
    TEST(ToolRegistry, RegisterAndLookup_ValidTool_ReturnsDefinitionAndTool)
    {
        // Arrange
        ToolRegistry registry;
        ToolDefinition def{
            "get_summary",
            "Get model summary",
            nlohmann::json{{"type", "object"}},
            ToolPolicy::ReadOnly};

        // Act
        registry.registerSyncTool(def, [](nlohmann::json const &) {
            return nlohmann::json{{"success", true}};
        });

        // Assert
        EXPECT_TRUE(registry.hasTool("get_summary"));
        EXPECT_EQ(registry.size(), 1u);
        auto const defs = registry.definitions();
        ASSERT_EQ(defs.size(), 1u);
        EXPECT_EQ(defs[0].name, "get_summary");
        EXPECT_EQ(defs[0].policy, ToolPolicy::ReadOnly);
        ASSERT_NE(registry.getTool("get_summary"), nullptr);
    }

    TEST(ToolRegistry, DefinitionsByMaxPolicy_MixedPolicies_FiltersCorrectly)
    {
        // Arrange
        ToolRegistry registry;
        registry.registerSyncTool({"read_tool", "Read", {}, ToolPolicy::ReadOnly},
                                  [](nlohmann::json const &) { return nlohmann::json{}; });
        registry.registerSyncTool({"view_tool", "View", {}, ToolPolicy::ViewModification},
                                  [](nlohmann::json const &) { return nlohmann::json{}; });
        registry.registerSyncTool({"model_tool", "Model", {}, ToolPolicy::ModelModification},
                                  [](nlohmann::json const &) { return nlohmann::json{}; });

        // Act
        auto const filtered = registry.definitionsByMaxPolicy(ToolPolicy::ViewModification);

        // Assert
        ASSERT_EQ(filtered.size(), 2u);
        EXPECT_EQ(filtered[0].name, "read_tool");
        EXPECT_EQ(filtered[1].name, "view_tool");
    }

    TEST(ToolExecutor, ExecuteSync_PolicyDenied_ReturnsStructuredError)
    {
        // Arrange
        ToolRegistry registry;
        bool executed = false;
        registry.registerSyncTool({"modify_model", "Modify", {}, ToolPolicy::ModelModification},
                                  [&](nlohmann::json const &) {
                                      executed = true;
                                      return nlohmann::json{{"success", true}};
                                  });

        ToolExecutor executor(registry, [](ToolDefinition const & def, nlohmann::json const &) -> PolicyDecision {
            if (def.policy == ToolPolicy::ModelModification)
            {
                return {false, "Confirmation required"};
            }
            return {true, ""};
        });
        io::CancellationToken token;

        // Act
        auto const result = executor.executeSync("modify_model", {}, token);

        // Assert
        EXPECT_FALSE(executed);
        EXPECT_FALSE(result.value("success", true));
        EXPECT_EQ(result.value("error", ""), "Confirmation required");
        EXPECT_EQ(result.value("policy", ""), "model_modification");
    }

    TEST(ToolExecutor, ExecuteSync_CancelledBeforeStart_ReturnsCancelledError)
    {
        // Arrange
        ToolRegistry registry;
        registry.registerSyncTool({"read_tool", "Read", {}, ToolPolicy::ReadOnly},
                                  [](nlohmann::json const &) { return nlohmann::json{{"success", true}}; });
        ToolExecutor executor(registry);
        io::CancellationToken token;
        token.requestCancellation();

        // Act
        auto const result = executor.executeSync("read_tool", {}, token);

        // Assert
        EXPECT_FALSE(result.value("success", true));
        EXPECT_EQ(result.value("tool", ""), "read_tool");
    }

    TEST(ToolExecutor, ExecuteSync_UnknownTool_ReturnsError)
    {
        // Arrange
        ToolRegistry registry;
        ToolExecutor executor(registry);
        io::CancellationToken token;

        // Act
        auto const result = executor.executeSync("nonexistent", {}, token);

        // Assert
        EXPECT_FALSE(result.value("success", true));
        EXPECT_EQ(result.value("error", ""), "Unknown tool: nonexistent");
    }

    TEST(ToolExecutor, ExecuteSync_ThrowingTool_CatchesAndReturnsError)
    {
        // Arrange
        ToolRegistry registry;
        registry.registerSyncTool({"boom", "Throws", {}, ToolPolicy::ReadOnly},
                                  [](nlohmann::json const &) -> nlohmann::json {
                                      throw std::runtime_error("unexpected failure");
                                  });
        ToolExecutor executor(registry);
        io::CancellationToken token;

        // Act
        auto const result = executor.executeSync("boom", {}, token);

        // Assert
        EXPECT_FALSE(result.value("success", true));
        EXPECT_EQ(result.value("error", ""), "unexpected failure");
    }
} // namespace gladius::tools::tests
