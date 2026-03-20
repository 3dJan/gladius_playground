/**
 * @file LibraryTool.h
 * @brief MCP tool for library browsing, creation, import, export, and deletion
 */

#pragma once

#include "MCPToolBase.h"

#include "../../FunctionArgument.h"

#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>

namespace gladius
{
    class Document;

namespace mcp::tools
{
    /**
     * @brief Implements MCP library operations for browsing, creating,
     *        importing, exporting, and deleting library entries.
     *
     * Library entries are 3MF files stored in the user library directory
     * (~/.local/share/gladius/library/) organized by category subdirectories.
     * Each entry can contain tagged implicit functions with metadata.
     */
    class LibraryTool : public MCPToolBase
    {
      public:
        explicit LibraryTool(Application * app);
        ~LibraryTool() override = default;

        /// @brief List all library categories and their entries.
        /// @param category Optional filter for a specific category.
        /// @param query Optional case-insensitive substring to filter entries by name, description, or tags.
        /// @return JSON with categories array containing entries and metadata.
        nlohmann::json listLibrary(std::string const & category = "",
                                   std::string const & query = "") const;

        /// @brief Get detailed info about a specific library entry.
        /// @param category Category subdirectory name.
        /// @param name Entry name (filename without .3mf extension).
        /// @return JSON with function signatures, metadata, and parameters.
        nlohmann::json getLibraryEntryInfo(std::string const & category,
                                           std::string const & name) const;

        /// @brief Create a library entry from a full program snippet.
        ///
        /// The snippet must include a `main` function that demonstrates the
        /// library function (used for bounding-box validation and thumbnail
        /// rendering).  The tool creates a fresh document from the template,
        /// applies the program snippet, validates the bounding box, renders a
        /// thumbnail, and exports the result with the full scaffold.
        ///
        /// @param name        Entry name (used as filename).
        /// @param category    Category subdirectory name.
        /// @param programSnippet  Full program listing including main and the
        ///                        library function (same format as set_program_snippet).
        /// @param functionId  Resource ID of the function to tag as the library function.
        /// @param description Human-readable description.
        /// @param tags        Optional keyword tags for searchability.
        /// @param overwrite   If true, replace an existing entry.
        /// @return JSON with creation result including path, function ID and message.
        nlohmann::json createLibraryEntry(std::string const & name,
                                          std::string const & category,
                                          std::string const & programSnippet,
                                          uint32_t functionId,
                                          std::string const & description,
                                          std::vector<std::string> const & tags = {},
                                          bool overwrite = false);

        /// @brief Export a function from the active document to the library.
        /// @param functionId ModelResourceID of the function to export.
        /// @param category Category subdirectory name.
        /// @param name Entry name (filename without .3mf extension).
        /// @param description Human-readable description.
        /// @param overwrite If true, replace existing entry.
        /// @param keepScaffold If true, keep the full document scaffold
        ///        (build items, levelset, mesh, main) so the entry renders standalone.
        /// @return JSON with export result including path and tagged IDs.
        nlohmann::json exportToLibrary(uint32_t functionId,
                                       std::string const & category,
                                       std::string const & name,
                                       std::string const & description,
                                       bool overwrite = false,
                                       bool keepScaffold = false);

        /// @brief Set library metadata on the current document or a specific library entry.
        /// @param functionIds Resource IDs of tagged (importable) functions.
        /// @param description Human-readable description of the library entry.
        /// @param tags Optional keyword tags for searchability.
        /// @param category Optional category to target a library entry directly.
        /// @param name Optional entry name to target a library entry directly.
        /// @return JSON with success status.
        nlohmann::json setLibraryMetadata(std::vector<uint32_t> const & functionIds,
                                          std::string const & description,
                                          std::vector<std::string> const & tags = {},
                                          std::string const & category = "",
                                          std::string const & name = "");

        /// @brief Import a library entry's tagged functions into the active document.
        /// @param category Category subdirectory name.
        /// @param name Entry name (filename without .3mf extension).
        /// @return JSON with imported function IDs and names.
        nlohmann::json importLibraryEntry(std::string const & category,
                                          std::string const & name);

        /// @brief Delete a library entry from the user library.
        /// @param category Category subdirectory name.
        /// @param name Entry name (filename without .3mf extension).
        /// @return JSON with deletion result.
        nlohmann::json deleteLibraryEntry(std::string const & category,
                                          std::string const & name);

      private:
        /// @brief Resolve a library entry path, checking both user and shipped dirs.
        /// @param category Category subdirectory name.
        /// @param name Entry name (without .3mf).
        /// @param isShipped Output: true if the entry is in the shipped library.
        /// @return Resolved path, or empty path if not found.
        std::filesystem::path resolveEntryPath(std::string const & category,
                                               std::string const & name,
                                               bool & isShipped) const;

        /// @brief Get list of available categories from both library directories.
        std::vector<std::string> getAvailableCategories() const;

        /// @brief Get list of available entries in a category.
        std::vector<std::string> getAvailableEntries(std::string const & category) const;

        /// @brief Prepare the GPU and render a PNG thumbnail for the active document.
        /// @param document Active document (must not be null).
        /// @return Raw PNG bytes, or empty if the compute core is unavailable or rendering fails.
        std::vector<unsigned char> renderThumbnailPng(std::shared_ptr<Document> document) const;
    };
} // namespace gladius::mcp::tools
} // namespace gladius
