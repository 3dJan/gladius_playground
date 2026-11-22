#include "ClipboardImageHelper.h"

#include <lodepng.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <mutex>
#endif

namespace gladius::ui
{
#ifdef __EMSCRIPTEN__
    namespace
    {
        std::mutex g_webClipMutex;
        ClipboardImageResult g_webClipPending;
    } // namespace

    extern "C"
    {
        EMSCRIPTEN_KEEPALIVE void gladiusOnWebClipboardImage(uint8_t const * data, int size)
        {
            if (data == nullptr || size <= 0)
            {
                return;
            }
            std::lock_guard<std::mutex> lock(g_webClipMutex);
            g_webClipPending.success = true;
            g_webClipPending.filename = "clipboard.png";
            g_webClipPending.mimeType = "image/png";
            g_webClipPending.pngBytes.resize(static_cast<std::size_t>(size));
            for (int i = 0; i < size; ++i)
            {
                g_webClipPending.pngBytes[static_cast<std::size_t>(i)] =
                  static_cast<std::byte>(data[i]);
            }
        }
    }

    void ClipboardImageHelper::requestWebClipboardImage()
    {
        EM_ASM({
            if (!navigator.clipboard || !navigator.clipboard.read) { return; }
            navigator.clipboard.read().then(async (items) => {
                for (const item of items) {
                    const pngType = item.types.find(t => t === 'image/png' || t.startsWith('image/'));
                    if (pngType) {
                        const blob = await item.getType(pngType);
                        const buf = new Uint8Array(await blob.arrayBuffer());
                        const ptr = Module._malloc(buf.length);
                        Module.HEAPU8.set(buf, ptr);
                        Module._gladiusOnWebClipboardImage(ptr, buf.length);
                        Module._free(ptr);
                        break;
                    }
                }
            }).catch(() => {});
        });
    }

    bool ClipboardImageHelper::consumePendingWebImage(ClipboardImageResult & out)
    {
        std::lock_guard<std::mutex> lock(g_webClipMutex);
        if (!g_webClipPending.success)
        {
            return false;
        }
        out = std::move(g_webClipPending);
        g_webClipPending = ClipboardImageResult{};
        return true;
    }
#endif

    namespace
    {
        bool isPngSignature(std::vector<std::byte> const & bytes)
        {
            static constexpr std::array<unsigned char, 8> kPngSig = {
              0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
            if (bytes.size() < kPngSig.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < kPngSig.size(); ++i)
            {
                if (static_cast<unsigned char>(bytes[i]) != kPngSig[i])
                {
                    return false;
                }
            }
            return true;
        }

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
        std::vector<std::byte> runCommandCaptureBytes(char const * cmd)
        {
            std::vector<std::byte> out;
            FILE * pipe = popen(cmd, "r");
            if (!pipe)
            {
                return out;
            }
            std::array<unsigned char, 4096> buf{};
            while (true)
            {
                std::size_t const n = std::fread(buf.data(), 1, buf.size(), pipe);
                if (n > 0)
                {
                    auto const oldSize = out.size();
                    out.resize(oldSize + n);
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        out[oldSize + i] = static_cast<std::byte>(buf[i]);
                    }
                }
                if (n < buf.size())
                {
                    break;
                }
            }
            int const rc = pclose(pipe);
            if (rc != 0)
            {
                out.clear();
            }
            return out;
        }
#endif
    } // namespace

    ClipboardImageResult ClipboardImageHelper::readClipboardImage()
    {
        ClipboardImageResult result;
#ifdef __EMSCRIPTEN__
        requestWebClipboardImage();
        return result;
#elif defined(_WIN32)
        if (!OpenClipboard(nullptr))
        {
            return result;
        }
        HANDLE hData = GetClipboardData(CF_DIB);
        if (hData != nullptr)
        {
            auto const * bmi = static_cast<BITMAPINFO const *>(GlobalLock(hData));
            if (bmi != nullptr)
            {
                int const width = bmi->bmiHeader.biWidth;
                int const height = std::abs(bmi->bmiHeader.biHeight);
                bool const topDown = bmi->bmiHeader.biHeight < 0;
                WORD const bpp = bmi->bmiHeader.biBitCount;
                if (width > 0 && height > 0 && (bpp == 24 || bpp == 32))
                {
                    auto const * pixels = reinterpret_cast<unsigned char const *>(bmi)
                                          + bmi->bmiHeader.biSize
                                          + bmi->bmiHeader.biClrUsed * sizeof(RGBQUAD);
                    std::size_t const stride =
                      ((static_cast<std::size_t>(width) * bpp + 31) / 32) * 4;
                    std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4);
                    for (int y = 0; y < height; ++y)
                    {
                        int const srcY = topDown ? y : (height - 1 - y);
                        unsigned char const * row = pixels + static_cast<std::size_t>(srcY) * stride;
                        for (int x = 0; x < width; ++x)
                        {
                            std::size_t const dstIdx =
                              (static_cast<std::size_t>(y) * width + x) * 4;
                            std::size_t const srcIdx = static_cast<std::size_t>(x) * (bpp / 8);
                            rgba[dstIdx + 0] = row[srcIdx + 2];
                            rgba[dstIdx + 1] = row[srcIdx + 1];
                            rgba[dstIdx + 2] = row[srcIdx + 0];
                            rgba[dstIdx + 3] = (bpp == 32) ? row[srcIdx + 3] : 255;
                        }
                    }
                    std::vector<unsigned char> png;
                    if (lodepng::encode(png,
                                        rgba,
                                        static_cast<unsigned int>(width),
                                        static_cast<unsigned int>(height))
                        == 0)
                    {
                        result.pngBytes.resize(png.size());
                        for (std::size_t i = 0; i < png.size(); ++i)
                        {
                            result.pngBytes[i] = static_cast<std::byte>(png[i]);
                        }
                        result.success = true;
                    }
                }
                GlobalUnlock(hData);
            }
        }
        CloseClipboard();
        return result;
#elif defined(__APPLE__)
        std::filesystem::path const tmp =
          std::filesystem::temp_directory_path() / "gladius_clip.png";
        std::string const cmd =
          "osascript -e 'try' -e 'set d to the clipboard as «class PNGf»' -e 'set f to open for access POSIX file \""
          + tmp.string()
          + "\" with write permission' -e 'set eof f to 0' -e 'write d to f' -e 'close access f' -e 'end try' >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0 && std::filesystem::exists(tmp))
        {
            std::ifstream in(tmp, std::ios::binary | std::ios::ate);
            auto const sz = in.tellg();
            if (sz > 0)
            {
                result.pngBytes.resize(static_cast<std::size_t>(sz));
                in.seekg(0, std::ios::beg);
                in.read(reinterpret_cast<char *>(result.pngBytes.data()), sz);
                result.success = isPngSignature(result.pngBytes);
            }
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
        }
        return result;
#else
        // Linux: try Wayland wl-paste first, then X11 xclip
        auto bytes = runCommandCaptureBytes("wl-paste --no-newline --type image/png 2>/dev/null");
        if (!isPngSignature(bytes))
        {
            bytes = runCommandCaptureBytes(
              "xclip -selection clipboard -t image/png -o 2>/dev/null");
        }
        if (isPngSignature(bytes))
        {
            result.pngBytes = std::move(bytes);
            result.success = true;
        }
        return result;
#endif
    }
} // namespace gladius::ui
