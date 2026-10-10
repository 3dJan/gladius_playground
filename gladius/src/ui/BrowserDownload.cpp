#include "BrowserDownload.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace gladius::ui
{
#ifdef __EMSCRIPTEN__
    bool startBrowserDownload(std::filesystem::path const & temporaryPath,
                              std::string const & downloadName,
                              std::string const & mimeType)
    {
        auto const temporaryPathString = temporaryPath.string();
        // An empty source name makes the shell fall back to downloadName.
        auto const emptySourceName = std::string{};
        int const accepted = EM_ASM_INT({
            if (typeof window.gladiusDownloadBrowserFile !== 'function') {
                return 0;
            }
            return window.gladiusDownloadBrowserFile(
                     UTF8ToString($0), UTF8ToString($1), UTF8ToString($2), UTF8ToString($3))
                       ? 1
                       : 0;
        },
                                        temporaryPathString.c_str(),
                                        emptySourceName.c_str(),
                                        downloadName.c_str(),
                                        mimeType.c_str());
        return accepted != 0;
    }
#else
    bool startBrowserDownload(std::filesystem::path const & temporaryPath,
                              std::string const & downloadName,
                              std::string const & mimeType)
    {
        (void) temporaryPath;
        (void) downloadName;
        (void) mimeType;
        return false;
    }
#endif
} // namespace gladius::ui
