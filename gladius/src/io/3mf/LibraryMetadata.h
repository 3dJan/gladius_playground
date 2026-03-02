#pragma once

#include "EventLogger.h"

#include <lib3mf_implicit.hpp>

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace gladius::io
{
    /// @brief Metadata stored at model level in a 3MF library file.
    ///
    /// The `libraryFunctions` field contains semicolon-separated model resource IDs
    /// identifying which functions should be imported selectively.
    /// The `libraryDescription` field contains a free-text description of the library entry.
    struct LibraryMetadata
    {
        std::string libraryFunctions;  ///< Semicolon-separated model resource IDs (e.g. "5;12")
        std::string libraryDescription; ///< Free-text description of this library entry
    };

    /// Metadata namespace used for library-specific keys in 3MF files.
    inline auto constexpr LIBRARY_METADATA_NAMESPACE = "gladius";

    /// Metadata key for the list of importable function resource IDs.
    inline auto constexpr LIBRARY_FUNCTIONS_KEY = "library-functions";

    /// Metadata key for the library entry description.
    inline auto constexpr LIBRARY_DESCRIPTION_KEY = "library-description";

    /// @brief Parses a semicolon-separated string of resource IDs into a vector.
    ///
    /// Whitespace around individual IDs is trimmed. Empty segments are skipped.
    /// @param value The semicolon-separated string (e.g. "5 ; 12 ; 3").
    /// @return Vector of parsed model resource IDs.
    [[nodiscard]] std::vector<Lib3MF_uint32> parseResourceIds(std::string const & value);

    /// @brief Serializes a vector of resource IDs into a semicolon-separated string.
    ///
    /// @param ids The resource IDs to serialize.
    /// @return Semicolon-separated string (e.g. "5;12;3").
    [[nodiscard]] std::string serializeResourceIds(std::vector<Lib3MF_uint32> const & ids);

    /// @brief Reads library metadata from a 3MF model's metadata group.
    ///
    /// Returns std::nullopt if the `gladius:library-functions` key is not present.
    /// The `gladius:library-description` key is optional; if absent, the description
    /// field will be empty.
    /// @param model The 3MF model to read metadata from.
    /// @return The library metadata, or std::nullopt if not a library file.
    [[nodiscard]] std::optional<LibraryMetadata> readLibraryMetadata(Lib3MF::PModel model);

    /// @brief Writes library metadata to a 3MF model's metadata group.
    ///
    /// Stamps both `gladius:library-functions` and `gladius:library-description`
    /// as model-level metadata with type "xs:string" and mustPreserve=true.
    /// @param model The 3MF model to write metadata to.
    /// @param metadata The metadata to write.
    void writeLibraryMetadata(Lib3MF::PModel model, LibraryMetadata const & metadata);

    /// @brief Removes all library metadata entries from a 3MF model.
    ///
    /// Removes any metadata in the `gladius` namespace from the model-level
    /// metadata group. Safe to call even if no library metadata is present.
    /// @param model The 3MF model to clean up.
    void removeLibraryMetadata(Lib3MF::PModel model);

    /// @brief Computes the selective import closure for a set of tagged functions.
    ///
    /// Given a source model and a list of model resource IDs identifying the
    /// "library functions" to import, builds a dependency graph and computes the
    /// transitive closure of all required resources. The returned set contains
    /// model resource IDs of the tagged functions plus all their dependencies.
    ///
    /// @param sourceModel The source 3MF model containing the library functions.
    /// @param taggedModelResourceIds Model resource IDs of the tagged functions.
    /// @param logger Logger for warnings and errors.
    /// @return Set of model resource IDs in the closure, or std::nullopt if any
    ///         tagged ID doesn't exist in the model or the input is empty.
    [[nodiscard]] std::optional<std::unordered_set<Lib3MF_uint32>>
    computeSelectiveImportClosure(Lib3MF::PModel sourceModel,
                                  std::vector<Lib3MF_uint32> const & taggedModelResourceIds,
                                  events::SharedLogger logger);

    /// @brief Removes resources from a model that are outside the import closure.
    ///
    /// Removes all build items and any implicit functions / FunctionFromImage3D
    /// resources whose model resource ID is not in the provided closure set.
    /// Mesh objects referenced only by removed build items can be cleaned up
    /// separately via lib3mf's unused resource removal.
    ///
    /// @param model The 3MF model to prune.
    /// @param closureModelResourceIds Set of model resource IDs to keep.
    /// @return true if pruning was successful, false on error.
    [[nodiscard]] bool pruneModelForSelectiveImport(
      Lib3MF::PModel model,
      std::unordered_set<Lib3MF_uint32> const & closureModelResourceIds);

} // namespace gladius::io
