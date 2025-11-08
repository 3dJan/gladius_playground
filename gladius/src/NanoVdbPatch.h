#pragma once

#include <string>
#include <string_view>
#include <cstddef>
#include <utility>
#include <vector>

namespace gladius::opencl
{
    struct NanoVdbPatchResult
    {
        bool modified{false};
        bool reservedDeclarationsRemoved{false};
        bool alignAttributeInjected{false};
        bool alignmentGuardsInserted{false};
        std::vector<std::string> diagnostics{};
        std::string source{};
    };

    namespace detail
    {
        inline bool containsAttributeAt(std::string const & source, std::size_t insertPos)
        {
            constexpr std::string_view attr{" __attribute__((aligned(CNANOVDB_DATA_ALIGNMENT)))"};
            return source.compare(insertPos, attr.size(), attr.data()) == 0;
        }

        inline bool ensureAlignmentAttribute(std::string & source, std::size_t searchPos)
        {
            constexpr std::string_view typedefStruct{"typedef struct"};
            constexpr std::string_view attr{" __attribute__((aligned(CNANOVDB_DATA_ALIGNMENT)))"};

            auto typedefPos = source.rfind(typedefStruct.data(), searchPos);
            if (typedefPos == std::string::npos)
            {
                return false;
            }

            auto insertPos = typedefPos + typedefStruct.size();
            if (containsAttributeAt(source, insertPos))
            {
                return false;
            }

            source.insert(insertPos, attr);
            return true;
        }

        inline std::string makeReservedReplacement(std::string const & line)
        {
            std::string indent;
            indent.reserve(line.size());
            for (auto ch : line)
            {
                if (ch == ' ' || ch == '\t')
                {
                    indent.push_back(ch);
                }
                else
                {
                    break;
                }
            }

            return indent + "// _reserved removed by GLADIUS rusticl workaround\n";
        }
    } // namespace detail

    inline NanoVdbPatchResult patchNanoVdbForRusticl(std::string source)
    {
        NanoVdbPatchResult result{};
        result.source = std::move(source);

        constexpr std::string_view marker{"_reserved["};
        if (result.source.find(marker) == std::string::npos)
        {
            result.diagnostics.emplace_back("NanoVDB patch: no _reserved declarations detected.");
            return result;
        }

        std::size_t pos = 0;
        while ((pos = result.source.find(marker, pos)) != std::string::npos)
        {
            auto insertedAttribute = detail::ensureAlignmentAttribute(result.source, pos);
            if (insertedAttribute)
            {
                result.alignAttributeInjected = true;
            }

            auto lineStart = result.source.rfind('\n', pos);
            if (lineStart == std::string::npos)
            {
                lineStart = 0;
            }
            else
            {
                lineStart += 1;
            }

            auto semicolon = result.source.find(';', pos);
            if (semicolon == std::string::npos)
            {
                break;
            }

            auto line = result.source.substr(lineStart, pos - lineStart);
            auto replacement = detail::makeReservedReplacement(line);
            result.source.replace(lineStart, semicolon - lineStart + 1, replacement);

            pos = lineStart + replacement.size();
            result.modified = true;
            result.reservedDeclarationsRemoved = true;
        }

        if (result.alignAttributeInjected)
        {
            result.modified = true;
        }

        if (!result.modified)
        {
            result.diagnostics.emplace_back("NanoVDB patch: replacement aborted (missing semicolon).");
            return result;
        }

        constexpr std::string_view guardMarker{"GLADIUS_NANOVDB_ALIGN_GUARD"};
        if (result.source.find(guardMarker) == std::string::npos)
        {
            std::string guardBlock =
              "\n#ifdef __OPENCL_VERSION__\n"
              "#ifndef GLADIUS_NANOVDB_ALIGN_GUARD\n"
              "#define GLADIUS_NANOVDB_ALIGN_GUARD\n"
              "#define GLADIUS_NANOVDB_ALIGN_ASSERT(TYPE) \\\n    typedef char gladius_nanovdb_align_check_##TYPE[(sizeof(TYPE) % CNANOVDB_DATA_ALIGNMENT) == 0 ? 1 : -1]\n"
              "GLADIUS_NANOVDB_ALIGN_ASSERT(cnanovdb_gridblindmetadata);\n"
              "GLADIUS_NANOVDB_ALIGN_ASSERT(cnanovdb_griddata);\n"
              "GLADIUS_NANOVDB_ALIGN_ASSERT(cnanovdb_treedata);\n"
              "#undef GLADIUS_NANOVDB_ALIGN_ASSERT\n"
              "#endif // GLADIUS_NANOVDB_ALIGN_GUARD\n"
              "#endif // __OPENCL_VERSION__\n";

            auto endifPos = result.source.rfind("#endif");
            if (endifPos == std::string::npos)
            {
                result.source.append(guardBlock);
            }
            else
            {
                result.source.insert(endifPos, guardBlock);
            }

            result.alignmentGuardsInserted = true;
            result.modified = true;
        }

        if (result.reservedDeclarationsRemoved)
        {
            result.diagnostics.emplace_back(
              "Removed zero-sized padding arrays to avoid rusticl SPIR-V emission errors.");
        }
        if (result.alignAttributeInjected)
        {
            result.diagnostics.emplace_back(
              "Injected alignment attributes to maintain CNANOVDB_DATA_ALIGNMENT layout.");
        }
        if (result.alignmentGuardsInserted)
        {
            result.diagnostics.emplace_back(
              "Added compile-time alignment guards for key NanoVDB structures.");
        }

        return result;
    }
} // namespace gladius::opencl
