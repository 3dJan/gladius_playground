#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
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

    inline NanoVdbPatchResult patchNanoVdbForRusticl(std::string source)
    {
        NanoVdbPatchResult result{};
        result.source = std::move(source);

        static constexpr char GRID_BLIND_RESERVED[] =
          "    uint8_t _reserved[CNANOVDB_ALIGNMENT_PADDING(sizeof(int64_t) + sizeof(uint64_t) +\n"
          "                                                   2 * sizeof(uint32_t) + 2 * sizeof(uint32_t) +\n"
          "                                                   256 * sizeof(char),\n"
          "                                                 CNANOVDB_DATA_ALIGNMENT)];\n";
        static constexpr char GRID_BLIND_REPLACEMENT[] =
          "    // GLADIUS: removed _reserved padding (rusticl workaround)\n";

        static constexpr char GRID_DATA_RESERVED[] =
          "    uint32_t _reserved[CNANOVDB_ALIGNMENT_PADDING(8 + 8 + 4 + 4 + 4 + 4 + 8 + 256 + 24 + 24 +\n"
          "                                                    sizeof(cnanovdb_map) + 24 + 4 + 4 + 8 + 4,\n"
          "                                                  CNANOVDB_DATA_ALIGNMENT) /\n"
          "                       4];\n";
        static constexpr char GRID_DATA_REPLACEMENT[] =
          "    // GLADIUS: removed _reserved padding (rusticl workaround)\n";

        static constexpr char TREE_DATA_RESERVED[] =
          "    uint8_t _reserved[CNANOVDB_ALIGNMENT_PADDING(4 * sizeof(uint64_t) + (3 + 3) * sizeof(uint32_t) +\n"
          "                                                   sizeof(uint64_t),\n"
          "                                                 CNANOVDB_DATA_ALIGNMENT)];\n";
        static constexpr char TREE_DATA_REPLACEMENT[] =
          "    // GLADIUS: removed _reserved padding (rusticl workaround)\n";

        static constexpr char LEAF_RESERVED[] =
          "        uint32_t _reserved[CNANOVDB_ALIGNMENT_PADDING(                                             \\\n"
          "                             sizeof(cnanovdb_mask##LOG2DIM) + 2 * sizeof(VALUETYPE) +              \\\n"
          "                               2 * sizeof(STATSTYPE) + sizeof(cnanovdb_coord) +                    \\\n"
          "                               sizeof(uint8_t[3]) + sizeof(uint8_t),                               \\\n"
          "                             CNANOVDB_DATA_ALIGNMENT) /                                            \\\n"
          "                           4];                                                                     \\\n";
        static constexpr char LEAF_REPLACEMENT[] =
          "        /* GLADIUS: removed _reserved padding (rusticl workaround) */ \\\n";

        static constexpr char INTERNAL_RESERVED[] =
          "        uint8_t _reserved[CNANOVDB_ALIGNMENT_PADDING(                                              \\\n"
          "          sizeof(cnanovdb_mask##LOG2DIM) + sizeof(VALUETYPE) * 2 + sizeof(STATSTYPE) * 2 +         \\\n"
          "            sizeof(cnanovdb_coord) * 2 + sizeof(int32_t) + sizeof(uint32_t),                       \\\n"
          "          CNANOVDB_DATA_ALIGNMENT)];                                                               \\\n";
        static constexpr char INTERNAL_REPLACEMENT[] =
          "        /* GLADIUS: removed _reserved padding (rusticl workaround) */ \\\n";

        static constexpr char ROOT_TILE_RESERVED[] =
          "        VALUETYPE value;                                                                           \\\n"
          "        uint8_t _reserved[CNANOVDB_ALIGNMENT_PADDING(sizeof(KEYSIZE) + sizeof(VALUETYPE) +         \\\n"
          "                                                       sizeof(int64_t) + sizeof(uint32_t),         \\\n"
          "                                                     CNANOVDB_DATA_ALIGNMENT)];                    \\\n";
        static constexpr char ROOT_TILE_REPLACEMENT[] =
          "        VALUETYPE value;                                                                           \\\n"
          "        /* GLADIUS: removed _reserved padding (rusticl workaround) */ \\\n";

        static constexpr char ROOT_RESERVED[] =
          "        VALUETYPE mMinimum, mMaximum;                                                              \\\n"
          "        STATSTYPE mAverage, mStdDevi;                                                              \\\n"
          "        uint32_t                                                                                   \\\n"
          "          _reserved[CNANOVDB_ALIGNMENT_PADDING(sizeof(cnanovdb_coord) * 2 + sizeof(uint32_t) +     \\\n"
          "                                                 sizeof(VALUETYPE) * 3 + sizeof(STATSTYPE) * 2,    \\\n"
          "                                               CNANOVDB_DATA_ALIGNMENT) /                          \\\n"
          "                    4];                                                                            \\\n";
        static constexpr char ROOT_REPLACEMENT[] =
          "        VALUETYPE mMinimum, mMaximum;                                                              \\\n"
          "        STATSTYPE mAverage, mStdDevi;                                                              \\\n"
          "        /* GLADIUS: removed _reserved padding (rusticl workaround) */ \\\n";

        struct Replacement
        {
            std::string_view pattern;
            std::string_view replacement;
            std::string_view description;
        };

        constexpr Replacement REPLACEMENTS[] = {
          {GRID_BLIND_RESERVED, GRID_BLIND_REPLACEMENT, "gridblindmetadata _reserved"},
          {GRID_DATA_RESERVED, GRID_DATA_REPLACEMENT, "griddata _reserved"},
          {TREE_DATA_RESERVED, TREE_DATA_REPLACEMENT, "treedata _reserved"},
          {LEAF_RESERVED, LEAF_REPLACEMENT, "CREATE_LEAF_NODE_int _reserved"},
          {INTERNAL_RESERVED, INTERNAL_REPLACEMENT, "CREATE_INTERNAL_NODE_int _reserved"},
          {ROOT_TILE_RESERVED, ROOT_TILE_REPLACEMENT, "CREATE_ROOTDATA tile _reserved"},
          {ROOT_RESERVED, ROOT_REPLACEMENT, "CREATE_ROOTDATA root _reserved"}};

        auto replacePattern = [&](Replacement const & repl) {
            auto pos = result.source.find(repl.pattern);
            if (pos == std::string::npos)
            {
                result.diagnostics.emplace_back(
                  std::string("NanoVDB patch: pattern not found for ") + repl.description.data());
                return false;
            }

            result.source.replace(pos, repl.pattern.size(), std::string(repl.replacement));
            return true;
        };

        bool allReplaced = true;
        for (auto const & repl : REPLACEMENTS)
        {
            allReplaced &= replacePattern(repl);
        }

        if (allReplaced)
        {
            result.reservedDeclarationsRemoved = true;
            result.modified = true;
        }

        constexpr std::string_view ATTRIBUTE_TEXT{
          " __attribute__((aligned(CNANOVDB_DATA_ALIGNMENT)))"};

        auto addAttributeForStruct = [&](std::string_view structPattern, std::size_t searchStart) -> std::optional<std::size_t> {
            auto structPos = result.source.find(structPattern, searchStart);
            if (structPos == std::string::npos)
            {
                return std::nullopt;
            }

            auto typedefPos = result.source.rfind("typedef struct", structPos);
            if (typedefPos == std::string::npos)
            {
                return std::nullopt;
            }

            auto bracePos = result.source.find('{', typedefPos);
            if (bracePos == std::string::npos || bracePos > structPos)
            {
                return std::nullopt;
            }

            auto existing = result.source.substr(typedefPos, bracePos - typedefPos);
            if (existing.find(ATTRIBUTE_TEXT) != std::string::npos)
            {
                return structPos + structPattern.size();
            }

            result.source.insert(bracePos, ATTRIBUTE_TEXT);
            result.alignAttributeInjected = true;
            result.modified = true;
            return structPos + structPattern.size() + ATTRIBUTE_TEXT.size();
        };

        auto addSingleStructAttribute = [&](std::string_view structPattern) {
            auto next = addAttributeForStruct(structPattern, 0);
            if (!next.has_value())
            {
                result.diagnostics.emplace_back(
                  std::string("NanoVDB patch: failed to inject alignment attribute for ") +
                  std::string(structPattern));
            }
        };

        addSingleStructAttribute("} cnanovdb_gridblindmetadata;");
        addSingleStructAttribute("} cnanovdb_griddata;");
        addSingleStructAttribute("} cnanovdb_treedata;");

        std::size_t searchPos = 0;
        while (true)
        {
            auto next = addAttributeForStruct("} cnanovdb_node##LEVEL##SUFFIX;", searchPos);
            if (!next.has_value())
            {
                break;
            }
            searchPos = *next;
        }

        searchPos = 0;
        while (true)
        {
            auto next = addAttributeForStruct("} cnanovdb_rootdata_tile##SUFFIX;", searchPos);
            if (!next.has_value())
            {
                break;
            }
            searchPos = *next;
        }

        searchPos = 0;
        while (true)
        {
            auto next = addAttributeForStruct("} cnanovdb_rootdata##SUFFIX;", searchPos);
            if (!next.has_value())
            {
                break;
            }
            searchPos = *next;
        }

        constexpr std::string_view guardMarker{"GLADIUS_NANOVDB_ALIGN_GUARD"};
        if (result.source.find(guardMarker) == std::string::npos)
        {
          std::string guardBlock =
            "\n#ifdef __OPENCL_VERSION__\n"
            "#ifndef GLADIUS_NANOVDB_ALIGN_GUARD\n"
            "#define GLADIUS_NANOVDB_ALIGN_GUARD\n"
            "#define GLADIUS_NANOVDB_ALIGN_ASSERT(TYPE) \\\n"
            "    typedef char gladius_nanovdb_align_check_##TYPE[(sizeof(TYPE) % CNANOVDB_DATA_ALIGNMENT) == 0 ? 1 : -1]\n"
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
              "Removed explicit padding arrays in NanoVDB OpenCL header for rusticl.");
        }
        if (result.alignAttributeInjected)
        {
            result.diagnostics.emplace_back(
              "Injected explicit CNANOVDB_DATA_ALIGNMENT attributes for patched structs.");
        }
        if (result.alignmentGuardsInserted)
        {
            result.diagnostics.emplace_back(
              "Added compile-time alignment guards for key NanoVDB structures.");
        }

        return result;
    }
} // namespace gladius::opencl
