/**
 * @file MockMCPApplication.h
 * @brief GMock implementation of MCPApplicationInterface shared by all MCP tests.
 */

#pragma once

#include "FunctionArgument.h"
#include "mcp/MCPApplicationInterface.h"

#include <gmock/gmock.h>
#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gladius::tests
{
    using json = nlohmann::json;

    // Type aliases to avoid comma issues in MOCK_METHOD
    using Float3Array = std::array<float, 3>;
    using Float12Array = std::array<float, 12>;

    // Mock implementation of MCPApplicationInterface for testing
    class MockMCPApplication : public MCPApplicationInterface
    {
      public:
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

        MOCK_METHOD(bool, createNewDocument, (), (override));
        MOCK_METHOD(bool, openDocument, (const std::string &), (override));
        MOCK_METHOD(bool, saveDocument, (), (override));
        MOCK_METHOD(bool, saveDocumentAs, (const std::string &), (override));
        MOCK_METHOD(bool, exportDocument, (const std::string &, const std::string &), (override));

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

        // New MCP interface methods
        MOCK_METHOD(bool, validateDocumentFor3MF, (), (const, override));
        MOCK_METHOD(bool, exportDocumentAs3MF, (const std::string &, bool), (const, override));

        // New 3MF Resource creation methods
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

        MOCK_METHOD(std::vector<std::string>, listAvailableFunctions, (), (const, override));
        MOCK_METHOD(nlohmann::json,
                    validateForManufacturing,
                    (const std::vector<std::string> &, const nlohmann::json &),
                    (const, override));
        MOCK_METHOD(bool, executeBatchOperations, (const nlohmann::json &, bool), (override));

        // Newly added MCP interface methods for build item and level set modifications
        MOCK_METHOD(bool, setBuildItemObjectByIndex, (uint32_t, uint32_t), (override));
        MOCK_METHOD(bool,
                    setBuildItemTransformByIndex,
                    (uint32_t, const Float12Array &),
                    (override));
        MOCK_METHOD(bool,
                    modifyLevelSet,
                    (uint32_t, std::optional<uint32_t>, std::optional<std::string>),
                    (override));

        // Rendering methods
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

        // Newly added tools
        MOCK_METHOD(nlohmann::json, getModelBoundingBox, (), (const, override));
        MOCK_METHOD(nlohmann::json, removeUnusedResources, (), (override));


        MOCK_METHOD(nlohmann::json,
                    setParameterValue,
                    (uint32_t, uint32_t, const std::string &, const nlohmann::json &),
                    (override));

        MOCK_METHOD(nlohmann::json, validateModel, (const nlohmann::json &), (override));

        // Library operations
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
                     uint32_t,
                     std::string const &,
                     std::vector<std::string> const &,
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
                     std::vector<std::string> const &,
                     std::string const &,
                     std::string const &),
                    (override));
        MOCK_METHOD(nlohmann::json,
                    importLibraryEntry,
                    (std::string const &, std::string const &),
                    (override));
        MOCK_METHOD(nlohmann::json,
                    deleteLibraryEntry,
                    (std::string const &, std::string const &),
                    (override));
        MOCK_METHOD(nlohmann::json,
                    browseBin,
                    (std::string const &),
                    (const, override));
        MOCK_METHOD(nlohmann::json,
                    restoreBinEntry,
                    (std::string const &, std::string const &),
                    (override));
        MOCK_METHOD(nlohmann::json,
                    deleteBinEntry,
                    (std::string const &, std::string const &),
                    (override));
        MOCK_METHOD(nlohmann::json,
                    emptyBin,
                    (),
                    (override));

#ifdef ENABLE_UI_TESTING
        bool uiClick(std::string const & /*path*/) override { return false; }
        std::vector<std::string> uiDumpItems(std::string const & /*parentPath*/) override { return {}; }
        bool captureUIScreenshot(std::string const & /*outputPath*/) override { return false; }
#endif
    };
} // namespace gladius::tests

