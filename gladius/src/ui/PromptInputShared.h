#pragma once

#include "PromptInput.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstddef>
#include <fstream>
#include <string>

namespace gladius::ui
{
    /// @brief True when the prompt send chord (Ctrl/Cmd+Enter) went down this frame
    inline bool isSendChordPressed()
    {
        ImGuiIO const & io = ImGui::GetIO();
        return (io.KeyCtrl || io.KeySuper)
          && (ImGui::IsKeyPressed(ImGuiKey_Enter, false)
              || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
    }

    /// @brief Mean glyph width of a representative sentence, for soft-wrap estimation
    inline float averageGlyphWidth()
    {
        constexpr char const * reference =
          "The quick brown fox jumps over 13 lazy dogs, 0123456789.";
        float const referenceWidth = ImGui::CalcTextSize(reference).x;
        if (referenceWidth <= 0.f)
        {
            return ImGui::GetFontSize() * 0.5f;
        }
        return referenceWidth / static_cast<float>(PromptInput::countUtf8Characters(reference));
    }

    /// @brief Chord hint shown next to the prompt fields
    inline constexpr char const * sendChordHelp()
    {
        return "Ctrl/Cmd+Enter sends";
    }

    /// @brief Reads a whole file into outContent, capped at maxBytes; returns bytes read
    inline std::size_t readTextFile(std::filesystem::path const & path,
                                    std::size_t maxBytes,
                                    std::string & outContent,
                                    std::string & outError)
    {
        std::error_code ec;
        auto const size = std::filesystem::file_size(path, ec);
        if (ec)
        {
            outError = "Cannot read file: " + path.string();
            return 0u;
        }
        if (size > maxBytes)
        {
            outError = "File is larger than " + std::to_string(maxBytes / 1024u)
                     + " KiB: " + path.filename().string();
            return 0u;
        }

        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            outError = "Cannot open file: " + path.string();
            return 0u;
        }

        outContent.assign(size, '\0');
        in.read(outContent.data(), static_cast<std::streamsize>(size));
        return static_cast<std::size_t>(in.gcount());
    }
} // namespace gladius::ui
