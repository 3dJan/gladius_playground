#pragma once

#include <filesystem>
#include <string>

namespace gladius::ui
{
    /// @brief Hands a generated file in the browser virtual file system to the user as a download.
    /// @param temporaryPath File to read; it is copied by the browser before this call returns.
    /// @param downloadName Suggested file name for the download.
    /// @param mimeType MIME type attached to the download blob.
    /// @return true when the browser accepted the download. Always false outside the browser build.
    [[nodiscard]] bool startBrowserDownload(std::filesystem::path const & temporaryPath,
                                            std::string const & downloadName,
                                            std::string const & mimeType);
} // namespace gladius::ui
