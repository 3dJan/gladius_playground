#pragma once
#include <filesystem>

namespace gladius
{
    /// @brief Returns the directory containing the running executable.
    std::filesystem::path getAppDir();

    /// @brief Returns the shipped (read-only) library directory: <appDir>/library
    std::filesystem::path getShippedLibraryDir();

    /// @brief Returns the per-user library directory: ~/.local/share/gladius/library
    /// Creates the directory if it does not exist.
    std::filesystem::path getUserLibraryDir();

    /// @brief Copies files from a source directory into a target directory.
    /// Existing target files are never overwritten. Sub-folder structure is preserved.
    /// @param source Source directory to copy from.
    /// @param target Target directory to copy into.
    /// @return The number of files that were copied.
    [[nodiscard]] std::size_t syncLibraryDirectory(
      std::filesystem::path const & source,
      std::filesystem::path const & target);

    /// @brief Copies new files from the shipped library into the user library.
    /// Existing user files are never overwritten. Sub-folder structure is preserved.
    /// @return The number of files that were copied.
    [[nodiscard]] std::size_t syncShippedLibrary();

    /// @brief Returns the bin directory for soft-deleted library entries: <userLib>/.bin
    std::filesystem::path getBinDir();

    /// @brief Checks whether a library entry is a shipped entry (or synced copy).
    /// Returns true if a file with the same category/name exists in the shipped library dir.
    /// @param category Category subdirectory name (e.g., "primitives").
    /// @param name Entry name without extension (e.g., "sphere").
    bool isShippedEntry(std::string const & category, std::string const & name);

    /// @brief Returns a non-colliding path in the given directory by appending _1, _2, etc.
    /// If the base path does not exist, returns it unchanged.
    /// @param directory Target directory.
    /// @param stem Filename stem (e.g., "sphere").
    /// @param extension File extension including dot (e.g., ".3mf").
    /// @return A path that does not yet exist on disk.
    std::filesystem::path disambiguateFilename(std::filesystem::path const & directory,
                                               std::string const & stem,
                                               std::string const & extension);

    /// @brief Returns the full path to the currently running executable.
    std::filesystem::path getExecutablePath();

    /// @brief Opens a file in a new (detached) instance of Gladius.
    /// @param filePath The file to open.
    /// @return true if the process was spawned successfully.
    bool openFileInNewInstance(std::filesystem::path const & filePath);
}
