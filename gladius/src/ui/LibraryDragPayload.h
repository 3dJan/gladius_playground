#pragma once

#include <filesystem>
#include <string>

namespace gladius::ui
{
    /// @brief Payload type name used for ImGui drag-and-drop from the library browser.
    constexpr char const * LIBRARY_DND_TYPE = "LIBRARY_FUNC";

    /// @brief Data carried during a drag-and-drop from the library browser.
    struct LibraryDragPayload
    {
        std::filesystem::path filePath; ///< Path to the .3mf library file
        std::string functionName;       ///< Display name of the function to import
    };

} // namespace gladius::ui
