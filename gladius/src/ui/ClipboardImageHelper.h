#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gladius::ui
{
    /// @brief Result of reading an image from the OS or browser clipboard
    struct ClipboardImageResult
    {
        bool success = false;
        std::vector<std::byte> pngBytes;
        std::string filename = "clipboard.png";
        std::string mimeType = "image/png";
    };

    /// @brief Cross-platform helper to read PNG image data from the system clipboard
    class ClipboardImageHelper
    {
      public:
        /// @brief Attempt to synchronously read an image from the clipboard (desktop OSes)
        [[nodiscard]] static ClipboardImageResult readClipboardImage();

#ifdef __EMSCRIPTEN__
        /// @brief Request async browser clipboard image read; invokes callback on completion
        static void requestWebClipboardImage();
        [[nodiscard]] static bool consumePendingWebImage(ClipboardImageResult & out);
#endif
    };
} // namespace gladius::ui
