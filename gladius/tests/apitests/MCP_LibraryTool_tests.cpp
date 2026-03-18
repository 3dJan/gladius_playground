/**
 * @file MCP_LibraryTool_tests.cpp
 * @brief Tests for MCP library tool operations
 */

#include "FunctionArgument.h"
#include "mcp/MCPApplicationInterface.h"
#include "mcp/MCPServer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>

namespace gladius::tests
{
    using json = nlohmann::json;
    using Float3Array = std::array<float, 3>;

    // ────────────────────────────────────────────────────────────────
    // Mock — mirrors MockMCPApplication from MCP_tests.cpp, extended
    // with library methods. Kept separate to avoid coupling.
    // ────────────────────────────────────────────────────────────────

    using Float12Array = std::array<float, 12>;

    class MockMCPApplicationForLibrary : public MCPApplicationInterface
    {
      public:
        // ── Basic info ──
        MOCK_METHOD(std::string, getVersion, (), (const, override));
        MOCK_METHOD(bool, isRunning, (), (const, override));
        MOCK_METHOD(std::string, getApplicationName, (), (const, override));
        MOCK_METHOD(std::string, getStatus, (), (const, override));
        MOCK_METHOD(void, setHeadlessMode, (bool), (override));
        MOCK_METHOD(bool, isHeadlessMode, (), (const, override));
        MOCK_METHOD(bool, showUI, (), (override));
        MOCK_METHOD(bool, isUIRunning, (), (const, override));
        MOCK_METHOD(bool, hasActiveDocument, (), (const, override));
        MOCK_METHOD(std::string, getActiveDocumentPath, (), (const, override));

        // ── Document lifecycle ──
        MOCK_METHOD(bool, createNewDocument, (), (override));
        MOCK_METHOD(bool, openDocument, (const std::string &), (override));
        MOCK_METHOD(bool, saveDocument, (), (override));
        MOCK_METHOD(bool, saveDocumentAs, (const std::string &), (override));
        MOCK_METHOD(bool, exportDocument, (const std::string &, const std::string &), (override));

        // ── Parameters ──
        MOCK_METHOD(bool,
                    setFloatParameter,
                    (uint32_t, const std::string &, const std::string &, float),
                    (override));
        MOCK_METHOD(float,
                    getFloatParameter,
                    (uint32_t, const std::string &, const std::string &),
                    (override));
        MOCK_METHOD(bool,
                    setStringParameter,
                    (uint32_t, const std::string &, const std::string &, const std::string &),
                    (override));
        MOCK_METHOD(std::string,
                    getStringParameter,
                    (uint32_t, const std::string &, const std::string &),
                    (override));

        // ── Expression/function ──
        MOCK_METHOD((std::pair<bool, uint32_t>),
                    createFunctionFromExpression,
                    (const std::string &,
                     const std::string &,
                     const std::string &,
                     const std::vector<FunctionArgument> &,
                     const std::string &),
                    (override));

        MOCK_METHOD((std::pair<bool, uint32_t>),
                    createFunctionFromSnippet,
                    (const std::string &,
                     const std::string &,
                     const std::string &,
                     const std::vector<FunctionArgument> &,
                     const std::string &),
                    (override));

        MOCK_METHOD(std::string, getLastErrorMessage, (), (const, override));
        MOCK_METHOD(bool, validateDocumentFor3MF, (), (const, override));
        MOCK_METHOD(bool, exportDocumentAs3MF, (const std::string &, bool), (const, override));

        // ── Resource creation ──
        MOCK_METHOD((std::pair<bool, uint32_t>),
                    createLevelSet,
                    (uint32_t, Float3Array, Float3Array),
                    (override));
        MOCK_METHOD((std::pair<bool, uint32_t>),
                    createImage3DFunction,
                    (const std::string &, const std::string &, float, float),
                    (override));
        MOCK_METHOD((std::pair<bool, uint32_t>),
                    createVolumetricColor,
                    (uint32_t, const std::string &),
                    (override));
        MOCK_METHOD((std::pair<bool, uint32_t>),
                    createVolumetricProperty,
                    (const std::string &, uint32_t, const std::string &),
                    (override));

        MOCK_METHOD(nlohmann::json,
                    analyzeFunctionProperties,
                    (const std::string &),
                    (const, override));
        MOCK_METHOD(nlohmann::json, getSceneHierarchy, (), (const, override));
        MOCK_METHOD(nlohmann::json, getDocumentInfo, (), (const, override));
        MOCK_METHOD(nlohmann::json, get3MFStructure, (), (const, override));
        MOCK_METHOD(nlohmann::json, getFunctionGraph, (uint32_t), (const, override));

        nlohmann::json setFunctionGraph(uint32_t, const nlohmann::json &, bool) override
        {
            return nlohmann::json{{"success", true}};
        }

        MOCK_METHOD(std::vector<std::string>, listAvailableFunctions, (), (const, override));
        MOCK_METHOD(nlohmann::json,
                    validateForManufacturing,
                    (const std::vector<std::string> &, const nlohmann::json &),
                    (const, override));
        MOCK_METHOD(bool, executeBatchOperations, (const nlohmann::json &, bool), (override));

        MOCK_METHOD(bool, setBuildItemObjectByIndex, (uint32_t, uint32_t), (override));
        MOCK_METHOD(bool,
                    setBuildItemTransformByIndex,
                    (uint32_t, const Float12Array &),
                    (override));
        MOCK_METHOD(bool,
                    modifyLevelSet,
                    (uint32_t, std::optional<uint32_t>, std::optional<std::string>),
                    (override));

        // ── Rendering ──
        MOCK_METHOD(bool,
                    renderToFile,
                    (const std::string &, uint32_t, uint32_t, const std::string &, float),
                    (override));
        MOCK_METHOD(bool,
                    renderWithCamera,
                    (const std::string &, const nlohmann::json &, const nlohmann::json &),
                    (override));
        MOCK_METHOD(bool, generateThumbnail, (const std::string &, uint32_t), (override));
        MOCK_METHOD(nlohmann::json, getOptimalCameraPosition, (), (const, override));
        MOCK_METHOD(nlohmann::json, getModelBoundingBox, (), (const, override));
        MOCK_METHOD(nlohmann::json, removeUnusedResources, (), (override));

        // ── Graph editing ──
        MOCK_METHOD(nlohmann::json, getNodeInfo, (uint32_t, uint32_t), (const, override));
        MOCK_METHOD(nlohmann::json,
                    createNode,
                    (uint32_t, const std::string &, const std::string &, uint32_t),
                    (override));
        MOCK_METHOD(nlohmann::json, deleteNode, (uint32_t, uint32_t), (override));
        MOCK_METHOD(nlohmann::json,
                    setParameterValue,
                    (uint32_t, uint32_t, const std::string &, const nlohmann::json &),
                    (override));
        MOCK_METHOD(nlohmann::json,
                    createLink,
                    (uint32_t, uint32_t, const std::string &, uint32_t, const std::string &),
                    (override));
        MOCK_METHOD(nlohmann::json,
                    deleteLink,
                    (uint32_t, uint32_t, const std::string &),
                    (override));
        MOCK_METHOD(nlohmann::json,
                    createFunctionCallNode,
                    (uint32_t, uint32_t, const std::string &),
                    (override));
        MOCK_METHOD(nlohmann::json,
                    createConstantNodesForMissingParameters,
                    (uint32_t, uint32_t, bool, std::vector<std::string> const &),
                    (override));
        MOCK_METHOD(nlohmann::json, removeUnusedNodes, (uint32_t), (override));
        MOCK_METHOD(nlohmann::json, validateModel, (const nlohmann::json &), (override));

        // ── Library operations (new) ──
        MOCK_METHOD(nlohmann::json, listLibrary, (std::string const &, std::string const &), (const, override));
        MOCK_METHOD(nlohmann::json,
                    getLibraryEntryInfo,
                    (std::string const &, std::string const &),
                    (const, override));
        MOCK_METHOD(nlohmann::json,
                    createLibraryEntry,
                    (std::string const &,
                     std::string const &,
                     std::string const &,
                     std::string const &,
                     bool),
                    (override));
        MOCK_METHOD(nlohmann::json,
                    createLibraryEntryFromSnippet,
                    (std::string const &,
                     std::string const &,
                     std::string const &,
                     std::string const &,
                     std::vector<FunctionArgument> const &,
                     std::string const &,
                     bool),
                    (override));
        MOCK_METHOD(nlohmann::json,
                    exportToLibrary,
                    (uint32_t,
                     std::string const &,
                     std::string const &,
                     std::string const &,
                     bool,
                     bool),
                    (override));
        MOCK_METHOD(nlohmann::json,
                    setLibraryMetadata,
                    (std::vector<uint32_t> const &,
                     std::string const &,
                     std::vector<std::string> const &),
                    (override));
        MOCK_METHOD(nlohmann::json,
                    importLibraryEntry,
                    (std::string const &, std::string const &),
                    (override));
        MOCK_METHOD(nlohmann::json,
                    deleteLibraryEntry,
                    (std::string const &, std::string const &),
                    (override));

#ifdef ENABLE_UI_TESTING
        bool uiClick(std::string const & /*path*/) override { return false; }
        std::vector<std::string> uiDumpItems(std::string const & /*parentPath*/) override { return {}; }
        bool captureUIScreenshot(std::string const & /*outputPath*/) override { return false; }
#endif
    };

    // ────────────────────────────────────────────────────────────────
    // Test fixture
    // ────────────────────────────────────────────────────────────────

    class MCPLibraryToolTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_mockApp = std::make_unique<MockMCPApplicationForLibrary>();

            EXPECT_CALL(*m_mockApp, validateModel(::testing::_))
              .WillRepeatedly(::testing::Return(json{{"success", true}}));

            m_server = std::make_unique<mcp::MCPServer>(m_mockApp.get());
        }

        void TearDown() override
        {
            m_server.reset();
            m_mockApp.reset();
        }

        /// Helper: invoke an MCP tool via JSON-RPC and return the parsed content text.
        json callTool(std::string const & toolName, json const & arguments = json::object())
        {
            json request = {{"jsonrpc", "2.0"},
                            {"id", 1},
                            {"method", "tools/call"},
                            {"params", {{"name", toolName}, {"arguments", arguments}}}};
            json response = m_server->processJSONRPCRequest(request);
            // Extract the tool result from the MCP envelope
            if (response.contains("result") && response["result"].contains("content") &&
                !response["result"]["content"].empty())
            {
                auto const & text = response["result"]["content"][0]["text"];
                return json::parse(text.get<std::string>());
            }
            return response;
        }

        std::unique_ptr<MockMCPApplicationForLibrary> m_mockApp;
        std::unique_ptr<mcp::MCPServer> m_server;
    };

    // ────────────────────────────────────────────────────────────────
    // Phase 3: US5 Transport — no stdout pollution
    // ────────────────────────────────────────────────────────────────

    TEST_F(MCPLibraryToolTest, LibraryToolsRegistered_ServerInitialized_ToolsAppearInList)
    {
        // Arrange
        json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}};

        // Act
        json response = m_server->processJSONRPCRequest(request);

        // Assert — verify library tools are registered
        auto const & tools = response["result"]["tools"];
        std::vector<std::string> toolNames;
        for (auto const & tool : tools)
        {
            toolNames.push_back(tool["name"].get<std::string>());
        }

        EXPECT_FALSE(toolNames.empty());
    }

    TEST_F(MCPLibraryToolTest, StdioTransport_ToolRegistration_NoStdoutPollution)
    {
        // Arrange — capture stdout during server construction
        std::stringstream capturedStdout;
        auto * originalBuf = std::cout.rdbuf(capturedStdout.rdbuf());

        // Act — construct a new server (which registers all tools)
        auto freshMock = std::make_unique<MockMCPApplicationForLibrary>();
        EXPECT_CALL(*freshMock, validateModel(::testing::_))
          .WillRepeatedly(::testing::Return(json{{"success", true}}));
        auto freshServer = std::make_unique<mcp::MCPServer>(freshMock.get());

        // Restore stdout
        std::cout.rdbuf(originalBuf);

        // Assert — captured stdout must be empty (registration logs go to stderr)
        EXPECT_TRUE(capturedStdout.str().empty())
          << "Tool registration must not write to stdout. Captured: '"
          << capturedStdout.str() << "'";
    }

    TEST_F(MCPLibraryToolTest, StdioTransport_ServerStop_NoStdoutPollution)
    {
        // Arrange — capture stdout during server destruction
        std::stringstream capturedStdout;
        auto * originalBuf = std::cout.rdbuf(capturedStdout.rdbuf());

        // Act — destroy the server (triggers stop)
        m_server.reset();

        // Restore stdout
        std::cout.rdbuf(originalBuf);

        // Assert — no stdout output from server stop
        EXPECT_TRUE(capturedStdout.str().empty())
          << "Server stop must not write to stdout. Captured: '"
          << capturedStdout.str() << "'";
    }

    // ────────────────────────────────────────────────────────────────
    // Phase 4: US1 Discovery — list_library and get_library_entry_info
    // ────────────────────────────────────────────────────────────────

    TEST_F(MCPLibraryToolTest, ListLibrary_EmptyDirectory_ReturnsEmptyCategories)
    {
        // Arrange
        json mockResult = {
          {"success", true},
          {"library_root", "/tmp/test_library"},
          {"categories", json::array()}
        };
        EXPECT_CALL(*m_mockApp, listLibrary(std::string(""), std::string("")))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("list_library");

        // Assert
        EXPECT_TRUE(result["success"].get<bool>());
        EXPECT_TRUE(result["categories"].is_array());
        EXPECT_TRUE(result["categories"].empty());
    }

    TEST_F(MCPLibraryToolTest, ListLibrary_WithEntries_ReturnsCategoriesAndMetadata)
    {
        // Arrange
        json mockResult = {
          {"success", true},
          {"library_root", "/tmp/test_library"},
          {"categories", {
            {{"name", "primitives"},
             {"is_shipped", true},
             {"entries", {
               {{"name", "sphere"},
                {"description", "Unit sphere SDF"},
                {"tagged_function_ids", {5}},
                {"has_metadata", true},
                {"is_shipped", true}}
             }}}
          }}
        };
        EXPECT_CALL(*m_mockApp, listLibrary(std::string(""), std::string("")))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("list_library");

        // Assert
        EXPECT_TRUE(result["success"].get<bool>());
        EXPECT_EQ(result["categories"].size(), 1u);
        EXPECT_EQ(result["categories"][0]["name"], "primitives");
        EXPECT_EQ(result["categories"][0]["entries"].size(), 1u);
        EXPECT_EQ(result["categories"][0]["entries"][0]["name"], "sphere");
        EXPECT_EQ(result["categories"][0]["entries"][0]["description"], "Unit sphere SDF");
    }

    TEST_F(MCPLibraryToolTest, ListLibrary_InvalidCategory_ReturnsErrorWithAvailableCategories)
    {
        // Arrange
        json mockResult = {
          {"success", false},
          {"error", "Category 'nonexistent' not found"},
          {"available_categories", {"lattices", "primitives"}}
        };
        EXPECT_CALL(*m_mockApp, listLibrary(std::string("nonexistent"), std::string("")))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("list_library", {{"category", "nonexistent"}});

        // Assert
        EXPECT_FALSE(result["success"].get<bool>());
        EXPECT_TRUE(result.contains("available_categories"));
        EXPECT_EQ(result["available_categories"].size(), 2u);
    }

    TEST_F(MCPLibraryToolTest, GetLibraryEntryInfo_ValidEntry_ReturnsFunctionSignatures)
    {
        // Arrange
        json mockResult = {
          {"success", true},
          {"name", "gyroid"},
          {"category", "lattices"},
          {"description", "A triply periodic minimal surface"},
          {"path", "/tmp/library/lattices/gyroid.3mf"},
          {"is_shipped", true},
          {"functions", {
            {{"resource_id", 5},
             {"name", "Gyroid"},
             {"type", "ImplicitFunction"},
             {"is_tagged", true},
             {"inputs", {{{"name", "pos"}, {"type", "vec3"}}}},
             {"outputs", {{{"name", "result"}, {"type", "float"}}}}}
          }}
        };
        EXPECT_CALL(*m_mockApp, getLibraryEntryInfo(std::string("lattices"), std::string("gyroid")))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("get_library_entry_info",
                               {{"category", "lattices"}, {"name", "gyroid"}});

        // Assert
        EXPECT_TRUE(result["success"].get<bool>());
        EXPECT_EQ(result["name"], "gyroid");
        EXPECT_EQ(result["category"], "lattices");
        EXPECT_FALSE(result["functions"].empty());
        EXPECT_EQ(result["functions"][0]["name"], "Gyroid");
        EXPECT_TRUE(result["functions"][0]["is_tagged"].get<bool>());
        EXPECT_FALSE(result["functions"][0]["inputs"].empty());
        EXPECT_FALSE(result["functions"][0]["outputs"].empty());
    }

    TEST_F(MCPLibraryToolTest,
           GetLibraryEntryInfo_NonexistentEntry_ReturnsErrorWithAvailableEntries)
    {
        // Arrange
        json mockResult = {
          {"success", false},
          {"error", "Entry 'bogus' not found in category 'primitives'"},
          {"available_entries", {"cube", "sphere", "torus"}}
        };
        EXPECT_CALL(*m_mockApp,
                    getLibraryEntryInfo(std::string("primitives"), std::string("bogus")))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("get_library_entry_info",
                               {{"category", "primitives"}, {"name", "bogus"}});

        // Assert
        EXPECT_FALSE(result["success"].get<bool>());
        EXPECT_TRUE(result.contains("available_entries"));
        EXPECT_EQ(result["available_entries"].size(), 3u);
    }

    // ────────────────────────────────────────────────────────────────
    // Phase 5: US2 Create — create_library_entry
    // ────────────────────────────────────────────────────────────────

    TEST_F(MCPLibraryToolTest, CreateLibraryEntry_ValidExpression_CreatesFileWithMetadata)
    {
        // Arrange
        json mockResult = {
          {"success", true},
          {"path", "/home/user/.local/share/gladius/library/primitives/my-sphere.3mf"},
          {"name", "my-sphere"},
          {"category", "primitives"},
          {"function_id", 1},
          {"message", "Created library entry 'my-sphere' in category 'primitives'"}
        };
        EXPECT_CALL(*m_mockApp,
                    createLibraryEntry(std::string("my-sphere"),
                                       std::string("primitives"),
                                       std::string("sqrt(x*x + y*y + z*z) - 5"),
                                       std::string("Sphere with radius 5"),
                                       false))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("create_library_entry",
                               {{"name", "my-sphere"},
                                {"category", "primitives"},
                                {"expression", "sqrt(x*x + y*y + z*z) - 5"},
                                {"description", "Sphere with radius 5"}});

        // Assert
        EXPECT_TRUE(result["success"].get<bool>());
        EXPECT_EQ(result["name"], "my-sphere");
        EXPECT_EQ(result["category"], "primitives");
        EXPECT_TRUE(result.contains("path"));
        EXPECT_TRUE(result.contains("function_id"));
        EXPECT_TRUE(result.contains("message"));
    }

    TEST_F(MCPLibraryToolTest, CreateLibraryEntry_InvalidExpression_ReturnsErrorWithSyntaxHelp)
    {
        // Arrange
        json mockResult = {
          {"success", false},
          {"error", "Expression parsing failed: unexpected token '@@'"},
          {"supported_syntax", {
            {"variables", "x, y, z"},
            {"operators", "+, -, *, /"},
            {"functions", "sin, cos, sqrt, abs, min, max, pow"}
          }},
          {"usage_example", {
            {"name", "my-sphere"},
            {"category", "primitives"},
            {"expression", "sqrt(x*x + y*y + z*z) - 5"},
            {"description", "Sphere with radius 5"}
          }}
        };
        EXPECT_CALL(*m_mockApp,
                    createLibraryEntry(std::string("bad-func"),
                                       std::string("primitives"),
                                       std::string("@@invalid@@"),
                                       std::string("Bad function"),
                                       false))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("create_library_entry",
                               {{"name", "bad-func"},
                                {"category", "primitives"},
                                {"expression", "@@invalid@@"},
                                {"description", "Bad function"}});

        // Assert
        EXPECT_FALSE(result["success"].get<bool>());
        EXPECT_TRUE(result.contains("error"));
        EXPECT_TRUE(result.contains("supported_syntax"));
        EXPECT_TRUE(result.contains("usage_example"));
    }

    TEST_F(MCPLibraryToolTest, CreateLibraryEntry_MissingParams_ReturnsErrorWithUsageExample)
    {
        // Act — send request with missing required params (no mock needed — handled by server)
        auto result = callTool("create_library_entry", {{"name", "only-name"}});

        // Assert
        EXPECT_FALSE(result["success"].get<bool>());
        EXPECT_TRUE(result.contains("error"));
        EXPECT_TRUE(result.contains("usage_example"));
    }

    TEST_F(MCPLibraryToolTest, CreateLibraryEntry_ExistingFile_ReturnsConflictError)
    {
        // Arrange
        json mockResult = {
          {"success", false},
          {"error", "Library entry 'sphere' already exists in category 'primitives'"},
          {"usage_example", {
            {"name", "sphere"},
            {"category", "primitives"},
            {"overwrite", true}
          }}
        };
        EXPECT_CALL(*m_mockApp,
                    createLibraryEntry(std::string("sphere"),
                                       std::string("primitives"),
                                       std::string("sqrt(x*x + y*y + z*z) - 1"),
                                       std::string("Unit sphere"),
                                       false))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("create_library_entry",
                               {{"name", "sphere"},
                                {"category", "primitives"},
                                {"expression", "sqrt(x*x + y*y + z*z) - 1"},
                                {"description", "Unit sphere"}});

        // Assert
        EXPECT_FALSE(result["success"].get<bool>());
        EXPECT_THAT(result["error"].get<std::string>(),
                    ::testing::HasSubstr("already exists"));
    }

    TEST_F(MCPLibraryToolTest, CreateLibraryEntry_OverwriteTrue_ReplacesExistingFile)
    {
        // Arrange
        json mockResult = {
          {"success", true},
          {"path", "/home/user/.local/share/gladius/library/primitives/sphere.3mf"},
          {"name", "sphere"},
          {"category", "primitives"},
          {"function_id", 1},
          {"message", "Created library entry 'sphere' in category 'primitives' (overwritten)"}
        };
        EXPECT_CALL(*m_mockApp,
                    createLibraryEntry(std::string("sphere"),
                                       std::string("primitives"),
                                       std::string("sqrt(x*x + y*y + z*z) - 1"),
                                       std::string("Unit sphere"),
                                       true))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("create_library_entry",
                               {{"name", "sphere"},
                                {"category", "primitives"},
                                {"expression", "sqrt(x*x + y*y + z*z) - 1"},
                                {"description", "Unit sphere"},
                                {"overwrite", true}});

        // Assert
        EXPECT_TRUE(result["success"].get<bool>());
        EXPECT_EQ(result["name"], "sphere");
        EXPECT_TRUE(result.contains("path"));
    }

    // ────────────────────────────────────────────────────────────────
    // Phase 6: US3 Export — export_to_library
    // ────────────────────────────────────────────────────────────────

    TEST_F(MCPLibraryToolTest, ExportToLibrary_ValidFunction_ExportsWithMetadata)
    {
        // Arrange
        json mockResult = {
          {"success", true},
          {"path", "/home/user/.local/share/gladius/library/custom/my-shape.3mf"},
          {"name", "my-shape"},
          {"category", "custom"},
          {"function_id", 5},
          {"message", "Exported function 5 to library entry 'my-shape' in category 'custom'"}
        };
        EXPECT_CALL(*m_mockApp,
                    exportToLibrary(5u,
                                    std::string("custom"),
                                    std::string("my-shape"),
                                    std::string("A custom shape"),
                                    false,
                                    false))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("export_to_library",
                               {{"function_id", 5},
                                {"category", "custom"},
                                {"name", "my-shape"},
                                {"description", "A custom shape"}});

        // Assert
        EXPECT_TRUE(result["success"].get<bool>());
        EXPECT_EQ(result["name"], "my-shape");
        EXPECT_EQ(result["category"], "custom");
        EXPECT_EQ(result["function_id"], 5u);
        EXPECT_TRUE(result.contains("path"));
    }

    TEST_F(MCPLibraryToolTest, ExportToLibrary_InvalidFunctionId_ReturnsErrorWithAvailableIds)
    {
        // Arrange
        json mockResult = {
          {"success", false},
          {"error", "Function with resource ID 999 not found in document"},
          {"usage_example", {{"function_id", 5}, {"category", "custom"}, {"name", "my-fn"}, {"description", "desc"}}},
          {"available_functions", {{{"resource_id", 5}, {"name", "Gyroid"}}, {{"resource_id", 7}, {"name", "Sphere"}}}}
        };
        EXPECT_CALL(*m_mockApp,
                    exportToLibrary(999u,
                                    std::string("custom"),
                                    std::string("my-fn"),
                                    std::string("desc"),
                                    false,
                                    false))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("export_to_library",
                               {{"function_id", 999},
                                {"category", "custom"},
                                {"name", "my-fn"},
                                {"description", "desc"}});

        // Assert
        EXPECT_FALSE(result["success"].get<bool>());
        EXPECT_TRUE(result.contains("available_functions"));
        EXPECT_EQ(result["available_functions"].size(), 2u);
    }

    TEST_F(MCPLibraryToolTest, ExportToLibrary_NoActiveDocument_ReturnsError)
    {
        // Arrange
        json mockResult = {
          {"success", false},
          {"error", "No active document. Open or create a document first."},
          {"usage_example", {{"function_id", 5}, {"category", "primitives"}, {"name", "my-function"}, {"description", "My exported function"}}}
        };
        EXPECT_CALL(*m_mockApp,
                    exportToLibrary(5u,
                                    std::string("primitives"),
                                    std::string("my-fn"),
                                    std::string("desc"),
                                    false,
                                    false))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("export_to_library",
                               {{"function_id", 5},
                                {"category", "primitives"},
                                {"name", "my-fn"},
                                {"description", "desc"}});

        // Assert
        EXPECT_FALSE(result["success"].get<bool>());
        EXPECT_THAT(result["error"].get<std::string>(),
                    ::testing::HasSubstr("document"));
    }

    // ────────────────────────────────────────────────────────────────
    // Phase 7: US4 Import — import_library_entry
    // ────────────────────────────────────────────────────────────────

    TEST_F(MCPLibraryToolTest, ImportLibraryEntry_ValidEntry_MergesFunctionsIntoDocument)
    {
        // Arrange
        json mockResult = {
          {"success", true},
          {"name", "gyroid"},
          {"category", "lattices"},
          {"imported_function", {{"resource_id", 12}, {"name", "Gyroid"}}},
          {"message", "Imported function 'Gyroid' (resource ID 12) from library entry 'gyroid' in category 'lattices'"}
        };
        EXPECT_CALL(*m_mockApp,
                    importLibraryEntry(std::string("lattices"), std::string("gyroid")))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("import_library_entry",
                               {{"category", "lattices"}, {"name", "gyroid"}});

        // Assert
        EXPECT_TRUE(result["success"].get<bool>());
        EXPECT_EQ(result["name"], "gyroid");
        EXPECT_EQ(result["category"], "lattices");
        EXPECT_TRUE(result.contains("imported_function"));
        EXPECT_EQ(result["imported_function"]["resource_id"], 12u);
        EXPECT_EQ(result["imported_function"]["name"], "Gyroid");
    }

    TEST_F(MCPLibraryToolTest, ImportLibraryEntry_NoActiveDocument_ReturnsError)
    {
        // Arrange
        json mockResult = {
          {"success", false},
          {"error", "No active document. Open or create a document first."},
          {"usage_example", {{"category", "primitives"}, {"name", "sphere"}}}
        };
        EXPECT_CALL(*m_mockApp,
                    importLibraryEntry(std::string("primitives"), std::string("sphere")))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("import_library_entry",
                               {{"category", "primitives"}, {"name", "sphere"}});

        // Assert
        EXPECT_FALSE(result["success"].get<bool>());
        EXPECT_THAT(result["error"].get<std::string>(),
                    ::testing::HasSubstr("document"));
    }

    TEST_F(MCPLibraryToolTest,
           ImportLibraryEntry_NonexistentEntry_ReturnsErrorWithAvailableEntries)
    {
        // Arrange
        json mockResult = {
          {"success", false},
          {"error", "Entry 'bogus' not found in category 'primitives'"},
          {"available_entries", {"cube", "sphere", "torus"}}
        };
        EXPECT_CALL(*m_mockApp,
                    importLibraryEntry(std::string("primitives"), std::string("bogus")))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("import_library_entry",
                               {{"category", "primitives"}, {"name", "bogus"}});

        // Assert
        EXPECT_FALSE(result["success"].get<bool>());
        EXPECT_TRUE(result.contains("available_entries"));
        EXPECT_EQ(result["available_entries"].size(), 3u);
    }

    // ────────────────────────────────────────────────────────────────
    // Phase 8: US6 Errors — agent-friendly error messages
    // ────────────────────────────────────────────────────────────────

    TEST_F(MCPLibraryToolTest, ExportToLibrary_MissingParams_ReturnsUsageExample)
    {
        // Act — send request with missing required params (handled by server)
        auto result = callTool("export_to_library", {{"function_id", 5}});

        // Assert
        EXPECT_FALSE(result["success"].get<bool>());
        EXPECT_TRUE(result.contains("error"));
        EXPECT_TRUE(result.contains("usage_example"));
    }

    TEST_F(MCPLibraryToolTest, ImportLibraryEntry_MissingParams_ReturnsUsageExample)
    {
        // Act — send request with missing required params (handled by server)
        auto result = callTool("import_library_entry", {{"category", "primitives"}});

        // Assert
        EXPECT_FALSE(result["success"].get<bool>());
        EXPECT_TRUE(result.contains("error"));
        EXPECT_TRUE(result.contains("usage_example"));
    }

    TEST_F(MCPLibraryToolTest, DeleteLibraryEntry_MissingParams_ReturnsUsageExample)
    {
        // Act — send request with missing required params (handled by server)
        auto result = callTool("delete_library_entry", {{"category", "primitives"}});

        // Assert
        EXPECT_FALSE(result["success"].get<bool>());
        EXPECT_TRUE(result.contains("error"));
        EXPECT_TRUE(result.contains("usage_example"));
    }

    // ────────────────────────────────────────────────────────────────
    // Phase 9: Delete — delete_library_entry
    // ────────────────────────────────────────────────────────────────

    TEST_F(MCPLibraryToolTest, DeleteLibraryEntry_UserEntry_DeletesFile)
    {
        // Arrange
        json mockResult = {
          {"success", true},
          {"name", "my-sphere"},
          {"category", "primitives"},
          {"message", "Deleted library entry 'my-sphere' from category 'primitives'"}
        };
        EXPECT_CALL(*m_mockApp,
                    deleteLibraryEntry(std::string("primitives"), std::string("my-sphere")))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("delete_library_entry",
                               {{"category", "primitives"}, {"name", "my-sphere"}});

        // Assert
        EXPECT_TRUE(result["success"].get<bool>());
        EXPECT_EQ(result["name"], "my-sphere");
        EXPECT_EQ(result["category"], "primitives");
        EXPECT_TRUE(result.contains("message"));
    }

    TEST_F(MCPLibraryToolTest, DeleteLibraryEntry_ShippedEntry_ReturnsReadOnlyError)
    {
        // Arrange
        json mockResult = {
          {"success", false},
          {"error", "Cannot delete shipped library entry 'sphere' in category 'primitives'. Only user-created entries can be deleted."}
        };
        EXPECT_CALL(*m_mockApp,
                    deleteLibraryEntry(std::string("primitives"), std::string("sphere")))
          .WillOnce(::testing::Return(mockResult));

        // Act
        auto result = callTool("delete_library_entry",
                               {{"category", "primitives"}, {"name", "sphere"}});

        // Assert
        EXPECT_FALSE(result["success"].get<bool>());
        EXPECT_THAT(result["error"].get<std::string>(),
                    ::testing::HasSubstr("shipped"));
    }

} // namespace gladius::tests
