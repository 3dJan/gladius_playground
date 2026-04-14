/**
 * @file MCPApplicationInterface.h
 * @brief Minimal interface for MCP server to interact with Application
 */

#pragma once

#include <array>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "FunctionArgument.h"

namespace gladius
{
    /**
     * @brief Minimal interface for MCP server to access Application functionality
     * This avoids pulling in heavy Application.h dependencies that cause build issues
     */
    class MCPApplicationInterface
    {
      public:
        virtual ~MCPApplicationInterface() = default;

        // Basic application info
        virtual std::string getVersion() const = 0;
        virtual bool isRunning() const = 0;
        virtual std::string getApplicationName() const = 0;

        // Status information
        virtual std::string getStatus() const = 0;

        // UI / Headless control
        virtual void setHeadlessMode(bool headless) = 0;
        virtual bool isHeadlessMode() const = 0;
        virtual bool showUI() = 0;
        virtual bool isUIRunning() const = 0;

        // Document operations
        virtual bool hasActiveDocument() const = 0;
        virtual std::string getActiveDocumentPath() const = 0;

        // Document lifecycle operations
        virtual bool createNewDocument() = 0;
        virtual bool openDocument(const std::string & path) = 0;
        virtual bool saveDocument() = 0;
        virtual bool saveDocumentAs(const std::string & path) = 0;
        virtual bool exportDocument(const std::string & path, const std::string & format) = 0;

        // Parameter operations
        virtual bool setFloatParameter(uint32_t modelId,
                                       const std::string & nodeName,
                                       const std::string & parameterName,
                                       float value) = 0;
        virtual float getFloatParameter(uint32_t modelId,
                                        const std::string & nodeName,
                                        const std::string & parameterName) = 0;
        virtual bool setStringParameter(uint32_t modelId,
                                        const std::string & nodeName,
                                        const std::string & parameterName,
                                        const std::string & value) = 0;
        virtual std::string getStringParameter(uint32_t modelId,
                                               const std::string & nodeName,
                                               const std::string & parameterName) = 0;

        // Expression and function operations
        virtual std::pair<bool, uint32_t>
        createFunctionFromExpression(const std::string & name,
                                     const std::string & expression,
                                     const std::string & outputType,
                                     const std::vector<FunctionArgument> & arguments = {},
                                     const std::string & outputName = "") = 0;

        /// Create a function from a multi-line snippet (assignments, if→select, return).
        virtual std::pair<bool, uint32_t>
        createFunctionFromSnippet(const std::string & name,
                                  const std::string & snippet,
                                  const std::string & outputType,
                                  const std::vector<FunctionArgument> & arguments = {},
                                  const std::string & outputName = "") = 0;

        // 3MF and implicit modeling operations
        virtual bool validateDocumentFor3MF() const = 0;
        virtual bool exportDocumentAs3MF(const std::string & path,
                                         bool includeImplicitFunctions = true) const = 0;

        // 3MF Resource creation methods (return success flag and resource ID)
        virtual std::pair<bool, uint32_t> createLevelSet(
          uint32_t functionId,
          std::array<float, 3> minPoint = {-10.f, -10.f, -10.f},
          std::array<float, 3> maxPoint = {10.f, 10.f, 10.f}) = 0;
        virtual std::pair<bool, uint32_t> createImage3DFunction(const std::string & name,
                                                                const std::string & imagePath,
                                                                float valueScale = 1.0f,
                                                                float valueOffset = 0.0f) = 0;
        virtual std::pair<bool, uint32_t> createVolumetricColor(uint32_t functionId,
                                                                const std::string & channel) = 0;
        virtual std::pair<bool, uint32_t> createVolumetricProperty(const std::string & propertyName,
                                                                   uint32_t functionId,
                                                                   const std::string & channel) = 0;

        virtual nlohmann::json
        analyzeFunctionProperties(const std::string & functionName) const = 0;

        // Scene and hierarchy operations
        virtual nlohmann::json getSceneHierarchy() const = 0;
        virtual nlohmann::json getDocumentInfo() const = 0;
        virtual std::vector<std::string> listAvailableFunctions() const = 0;

        /**
         * @brief Get a comprehensive structure of the current 3MF model
         *
         * Returns a JSON object listing build items and resources (meshes, level sets,
         * functions, images, materials, etc.) to allow assistants to inspect
         * what is contained in the document.
         *
         * Expected JSON shape (fields may vary if information is unavailable):
         * {
         *   "has_document": bool,
         *   "document_path": string,
         *   "build_items": [ { ... } ],
         *   "resources": [ { ... } ],
         *   "counts": { "build_items": n, "resources": n, "meshes": n, ... }
         * }
         */
        virtual nlohmann::json get3MFStructure() const = 0;

        /**
         * @brief Sets the value of a parameter on a node.
         *
         * @param functionId ModelResourceID of the function (model).
         * @param nodeId The ID of the node.
         * @param parameterName The name of the parameter to set.
         * @param value The value to set.
         * @return JSON with success status.
         */
        virtual nlohmann::json setParameterValue(uint32_t functionId,
                                                 uint32_t nodeId,
                                                 const std::string & parameterName,
                                                 const nlohmann::json & value) = 0;

        /// Evaluate a function at sample points via OpenCL. Override to provide implementation.
        virtual nlohmann::json evaluateFunction(uint32_t /*functionId*/,
                                                nlohmann::json const & /*samples*/)
        {
            return {{"success", false}, {"error", "Not implemented"}};
        }

        /// Return a structured log of document changes since a given ISO-8601 timestamp.
        virtual nlohmann::json getChangesSince(std::string const & /*isoTimestamp*/) const
        {
            return {{"success", true}, {"changes", nlohmann::json::array()}};
        }

        /// Get code snippet for a function. Override to provide implementation.
        virtual nlohmann::json getFunctionSnippet(uint32_t /*functionId*/) const
        {
            return {{"success", false}, {"error", "Not implemented"}};
        }

        /// Set function graph from a code snippet. Override to provide implementation.
        virtual nlohmann::json
        setFunctionSnippet(uint32_t /*functionId*/,
                           std::string const & /*snippet*/,
                           std::string const & /*outputType*/ = "float",
                           std::vector<FunctionArgument> const & /*arguments*/ = {})
        {
            return {{"success", false}, {"error", "Not implemented"}};
        }

        /// Get the entire program as a single code listing. Override to provide implementation.
        virtual nlohmann::json getProgramSnippet() const
        {
            return {{"success", false}, {"error", "Not implemented"}};
        }

        /// Replace all functions from a multi-function code listing. Override to provide impl.
        virtual nlohmann::json setProgramSnippet(std::string const & /*snippet*/)
        {
            return {{"success", false}, {"error", "Not implemented"}};
        }

        // Manufacturing validation
        virtual nlohmann::json
        validateForManufacturing(const std::vector<std::string> & functionNames = {},
                                 const nlohmann::json & constraints = {}) const = 0;

        // Build item and level set modification (authoring helpers)
        /**
         * @brief Set the referenced object (by ModelResourceID) on an existing build item.
         * @param buildItemIndex Zero-based index of the build item in the model's build list.
         * @param objectModelResourceId ModelResourceID of the object to reference (mesh,
         * components, levelset, ...).
         * @return true on success, false otherwise (check getLastErrorMessage()).
         */
        virtual bool setBuildItemObjectByIndex(uint32_t buildItemIndex,
                                               uint32_t objectModelResourceId) = 0;

        /**
         * @brief Set the transform of an existing build item.
         * @param buildItemIndex Zero-based index of the build item in the model's build list.
         * @param transform4x3RowMajor 12 floats (row-major 4x3 matrix) matching Lib3MF::sTransform
         * fields.
         * @return true on success, false otherwise (check getLastErrorMessage()).
         */
        virtual bool
        setBuildItemTransformByIndex(uint32_t buildItemIndex,
                                     const std::array<float, 12> & transform4x3RowMajor) = 0;

        /**
         * @brief Modify a level set's referenced function and/or output channel.
         * @param levelSetModelResourceId ModelResourceID of the level set to modify.
         * @param functionModelResourceId Optional ModelResourceID of the function to reference.
         * @param channel Optional channel/output name to set (e.g., "shape" or "result").
         * @return true on success, false otherwise (check getLastErrorMessage()).
         */
        virtual bool modifyLevelSet(uint32_t levelSetModelResourceId,
                                    std::optional<uint32_t> functionModelResourceId,
                                    std::optional<std::string> channel) = 0;

        /**
         * @brief Validate the current model in two phases and return diagnostics.
         *
         * Phases:
         *  1) graph_sync: Update 3MF model and inputs/outputs, validate assembly structure.
         *  2) opencl_compile: Generate kernels and attempt an OpenCL build.
         *
         * Options (JSON):
         *  - compile (bool, default true): run the OpenCL compile phase.
         *  - max_messages (int, default 50): limit of diagnostic messages to include.
         *
         * Returns a JSON object with fields:
         * {
         *   success: bool,
         *   phases: [
         *     { name: "graph_sync", ok: bool, errors: n, warnings: n, messages: [...] },
         *     { name: "opencl_compile", ok: bool, errors: n, warnings: n, messages: [...] }
         *   ],
         *   summary: { graph_ok: bool, compile_ok: bool }
         * }
         */
        /**
         * @brief Optional: Validate the current model. Default returns a stub if not overridden.
         */
        virtual nlohmann::json validateModel(const nlohmann::json & options = {})
        {
            (void) options; // unused in default implementation
            nlohmann::json res;
            res["success"] = false;
            res["phases"] = nlohmann::json::array();
            res["summary"] = {{"graph_ok", false}, {"compile_ok", false}};
            res["error"] = "validateModel not implemented";
            return res;
        }

        // Rendering operations
        /**
         * @brief Render the current 3MF model to an image file
         * @param outputPath File path where to save the rendered image
         * @param width Image width in pixels (default: 1024)
         * @param height Image height in pixels (default: 1024)
         * @param format Output format ("png", "jpg") (default: "png")
         * @param quality Quality setting 0.0-1.0 for lossy formats (default: 0.9)
         * @return true on success, false otherwise
         */
        virtual bool renderToFile(const std::string & outputPath,
                                  uint32_t width = 1024,
                                  uint32_t height = 1024,
                                  const std::string & format = "png",
                                  float quality = 0.9f) = 0;

        /**
         * @brief Render with camera settings
         * @param outputPath File path where to save the rendered image
         * @param cameraSettings JSON object with camera parameters:
         *   - eye_position: [x, y, z] camera position
         *   - target_position: [x, y, z] look-at target
         *   - up_vector: [x, y, z] up direction (default: [0, 0, 1])
         *   - field_of_view: degrees (default: 45.0)
         * @param renderSettings JSON object with render parameters:
         *   - width: image width in pixels (default: 1024)
         *   - height: image height in pixels (default: 1024)
         *   - format: output format "png", "jpg" (default: "png")
         *   - quality: quality 0.0-1.0 for lossy formats (default: 0.9)
         *   - background_color: [r, g, b, a] normalized (default: [0.2, 0.2, 0.2, 1.0])
         *   - enable_shadows: boolean (default: true)
         *   - enable_lighting: boolean (default: true)
         * @return true on success, false otherwise
         */
        virtual bool renderWithCamera(const std::string & outputPath,
                                      const nlohmann::json & cameraSettings = {},
                                      const nlohmann::json & renderSettings = {}) = 0;

        /**
         * @brief Generate a thumbnail image of the current model
         * @param outputPath File path where to save the thumbnail
         * @param size Thumbnail size in pixels (square image) (default: 256)
         * @return true on success, false otherwise
         */
        virtual bool generateThumbnail(const std::string & outputPath, uint32_t size = 256) = 0;

        /**
         * @brief Get optimal camera position for the current model
         * @return JSON object with suggested camera settings for best view
         */
        virtual nlohmann::json getOptimalCameraPosition() const = 0;

        /**
         * @brief Get the bounding box of the whole 3MF model (auto-updates if needed).
         *
         * Computes or refreshes the model's bounding box and returns a JSON object with:
         * {
         *   success: bool,
         *   bounding_box: {
         *     min: [x,y,z],
         *     max: [x,y,z],
         *     size: [sx,sy,sz],
         *     center: [cx,cy,cz],
         *     diagonal: float,
         *     units: "mm",
         *     is_valid: bool
         *   },
         *   error: string (optional)
         * }
         */
        virtual nlohmann::json getModelBoundingBox() const
        {
            nlohmann::json res;
            res["success"] = false;
            res["error"] = "getModelBoundingBox not implemented";
            return res;
        }

        /**
         * @brief Remove all unused resources from the current 3MF document.
         *
         * This performs a non-interactive cleanup equivalent to the UI's
         * "Delete unused resources" action but without a selection dialog.
         *
         * Returns a JSON object with:
         * { success: bool, removed_count: number, message?: string, error?: string }
         */
        virtual nlohmann::json removeUnusedResources()
        {
            // Safe default to avoid breaking mocks; real implementation in adapter
            return nlohmann::json{{"success", false},
                                  {"removed_count", 0},
                                  {"error", "removeUnusedResources not implemented"}};
        }

        // Batch operations
        virtual bool executeBatchOperations(const nlohmann::json & operations,
                                            bool rollbackOnError = true) = 0;

        // Library operations

        /// @brief List all library categories and their entries.
        /// @param category Optional filter for a specific category.
        /// @param query Optional case-insensitive substring to filter entries by name, description, or tags.
        /// @return JSON with categories array, each containing entries with metadata.
        virtual nlohmann::json listLibrary(std::string const & category = "",
                                           std::string const & query = "") const = 0;

        /// @brief Get detailed information about a specific library entry.
        /// @param category Category subdirectory name.
        /// @param name Entry name (filename without .3mf extension).
        /// @return JSON with function signatures, metadata, and parameters.
        virtual nlohmann::json getLibraryEntryInfo(std::string const & category,
                                                   std::string const & name) const = 0;

        /// @brief Create a library entry from a full program snippet with quality gate.
        virtual nlohmann::json createLibraryEntry(std::string const & name,
                                                  std::string const & category,
                                                  std::string const & programSnippet,
                                                  uint32_t functionId,
                                                  std::string const & description,
                                                  std::vector<std::string> const & tags = {},
                                                  bool overwrite = false) = 0;

        /// @brief Export a function from the active document to the library.
        /// @param functionId ModelResourceID of the function to export.
        /// @param category Category subdirectory name.
        /// @param name Entry name (filename without .3mf extension).
        /// @param description Human-readable description.
        /// @param overwrite If true, replace an existing entry.
        /// @param keepScaffold If true, keep the full document scaffold (build items,
        ///        levelset, mesh, main) so the entry renders standalone.
        /// @return JSON with export result including path and tagged function IDs.
        virtual nlohmann::json exportToLibrary(uint32_t functionId,
                                               std::string const & category,
                                               std::string const & name,
                                               std::string const & description,
                                               bool overwrite = false,
                                               bool keepScaffold = false) = 0;

        /// @brief Set library metadata (tagged functions and description) on the current document.
        /// @param functionIds Resource IDs of tagged (importable) functions.
        /// @param description Human-readable description of the library entry.
        /// @param tags Optional keyword tags for searchability.
        /// @param category Optional category to target a library entry directly.
        /// @param name Optional entry name to target a library entry directly.
        /// @return JSON with success status.
        virtual nlohmann::json
        setLibraryMetadata(std::vector<uint32_t> const & functionIds,
                           std::string const & description,
                           std::vector<std::string> const & tags = {},
                           std::string const & category = "",
                           std::string const & name = "") = 0;

        /// @brief Import a library entry's tagged functions into the active document.
        /// @param category Category subdirectory name.
        /// @param name Entry name (filename without .3mf extension).
        /// @return JSON with imported function IDs and names.
        virtual nlohmann::json importLibraryEntry(std::string const & category,
                                                  std::string const & name) = 0;

        /// @brief Delete a library entry from the user library (soft-delete to bin).
        /// @param category Category subdirectory name.
        /// @param name Entry name (filename without .3mf extension).
        /// @return JSON with deletion result (fails for shipped/read-only entries).
        virtual nlohmann::json deleteLibraryEntry(std::string const & category,
                                                  std::string const & name) = 0;

        /// @brief List all entries currently in the bin, optionally filtered by category.
        /// @param category Optional category filter (empty = all categories).
        /// @return JSON with binned entries grouped by category.
        virtual nlohmann::json browseBin(std::string const & category = "") const = 0;

        /// @brief Restore a binned entry back to the user library.
        /// @param category Category subdirectory name inside the bin.
        /// @param name Entry name (filename without .3mf extension).
        /// @return JSON with restore result.
        virtual nlohmann::json restoreBinEntry(std::string const & category,
                                               std::string const & name) = 0;

        /// @brief Permanently delete a single entry from the bin.
        /// @param category Category subdirectory name inside the bin.
        /// @param name Entry name (filename without .3mf extension).
        /// @return JSON with deletion result.
        virtual nlohmann::json deleteBinEntry(std::string const & category,
                                              std::string const & name) = 0;

        /// @brief Permanently delete all entries in the bin.
        /// @return JSON with count of removed entries.
        virtual nlohmann::json emptyBin() = 0;

#ifdef ENABLE_UI_TESTING
        /// @brief Perform a UI click on the specified ImGui test engine path.
        /// @param path The ImGui test path (e.g., "//MainWindow/File/Open").
        /// @return True if the click was successfully queued.
        virtual bool uiClick(std::string const & path) = 0;

        /// @brief Dump all items within a parent path.
        virtual std::vector<std::string> uiDumpItems(std::string const & parentPath) = 0;

        /// @brief Capture a screenshot of the current UI.
        /// @param outputPath The file path where the screenshot will be saved.
        /// @return True if the screenshot was saved successfully.
        virtual bool captureUIScreenshot(std::string const & outputPath) = 0;
#endif

        // Error handling
        virtual std::string getLastErrorMessage() const = 0;
    };
}
