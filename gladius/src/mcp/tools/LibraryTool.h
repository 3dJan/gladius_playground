/**
 * @file LibraryTool.h
 * @brief MCP tool for library browsing, creation, import, export, and deletion
 */

#pragma once

#include "MCPToolBase.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace gladius::mcp::tools
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
        /// @return JSON with categories array containing entries and metadata.
        nlohmann::json listLibrary(std::string const & category = "") const;

        /// @brief Get detailed info about a specific library entry.
        /// @param category Category subdirectory name.
        /// @param name Entry name (filename without .3mf extension).
        /// @return JSON with function signatures, metadata, and parameters.
        nlohmann::json getLibraryEntryInfo(std::string const & category,
                                           std::string const & name) const;

        /// @brief Create a new library entry from a math expression.
        /// @param name Entry name (used as filename).
        /// @param category Category subdirectory name.
        /// @param expression Math expression defining the SDF function.
        /// @param description Human-readable description.
        /// @param overwrite If true, replace existing entry.
        /// @return JSON with creation result including path and function ID.
        nlohmann::json createLibraryEntry(std::string const & name,
                                          std::string const & category,
                                          std::string const & expression,
                                          std::string const & description,
                                          bool overwrite = false);

        /// @brief Export a function from the active document to the library.
        /// @param functionId ModelResourceID of the function to export.
        /// @param category Category subdirectory name.
        /// @param name Entry name (filename without .3mf extension).
        /// @param description Human-readable description.
        /// @param overwrite If true, replace existing entry.
        /// @return JSON with export result including path and tagged IDs.
        nlohmann::json exportToLibrary(uint32_t functionId,
                                       std::string const & category,
                                       std::string const & name,
                                       std::string const & description,
                                       bool overwrite = false);

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
    };
} // namespace gladius::mcp::tools
