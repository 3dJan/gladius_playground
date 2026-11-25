#include "PromptInput.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <utility>

namespace gladius::ui
{
    std::size_t PromptInput::countUtf8Characters(std::string const & text)
    {
        std::size_t count = 0u;
        for (char const c : text)
        {
            auto const byte = static_cast<unsigned char>(c);
            if ((byte & 0xC0u) != 0x80u)
            {
                ++count;
            }
        }
        return count;
    }

    std::size_t PromptInput::countVisualLines(std::string const & text, std::size_t charsPerLine)
    {
        std::size_t const wrapWidth = std::max<std::size_t>(1u, charsPerLine);
        std::size_t lines = 0u;
        std::size_t lineChars = 0u;
        std::size_t const size = text.size();

        for (std::size_t i = 0u; i < size; ++i)
        {
            if (text[i] == '\n')
            {
                lines += lineChars == 0u ? 1u : (lineChars + wrapWidth - 1u) / wrapWidth;
                lineChars = 0u;
                continue;
            }
            auto const byte = static_cast<unsigned char>(text[i]);
            if ((byte & 0xC0u) != 0x80u)
            {
                ++lineChars;
            }
        }

        bool const trailingEmptyLine = !text.empty() && text.back() == '\n';
        if (trailingEmptyLine)
        {
            return lines + 1u;
        }
        return lines + (lineChars == 0u ? 0u : (lineChars + wrapWidth - 1u) / wrapWidth);
    }

    float PromptInput::computeFieldHeight(std::size_t lineCount,
                                          float lineHeightWithSpacing,
                                          float framePaddingY,
                                          float minHeight,
                                          float maxHeight)
    {
        float const contentHeight = static_cast<float>(lineCount) * lineHeightWithSpacing
                                  + framePaddingY * 2.f;
        float const upper = std::max(minHeight, maxHeight);
        return std::clamp(contentHeight, minHeight, upper);
    }

    bool PromptInput::isSupportedTextFile(std::filesystem::path const & path)
    {
        std::string extension = path.extension().string();
        std::transform(extension.begin(),
                       extension.end(),
                       extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension == ".txt" || extension == ".md" || extension == ".markdown"
          || extension == ".text";
    }

    void PromptInput::appendToDraft(std::string & draft, std::string const & text)
    {
        if (text.empty())
        {
            return;
        }

        std::string body = text;
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r'))
        {
            body.pop_back();
        }

        if (draft.empty())
        {
            draft = std::move(body);
            return;
        }
        if (draft.back() != '\n')
        {
            draft += '\n';
        }
        draft += body;
    }

    bool PromptInput::isTextContent(std::string const & content)
    {
        for (char const c : content)
        {
            if (c == '\0')
            {
                return false;
            }
            if (c == '\x1A') // EOF marker used by some Windows text files
            {
                return false;
            }
        }
        return true;
    }
} // namespace gladius::ui
