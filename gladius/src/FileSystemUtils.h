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
}
