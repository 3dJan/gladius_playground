#include "AgentToolOutput.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace gladius::design::agent_tool_output
{
    namespace
    {
        /// @brief Base64 payloads the model cannot read and does not need to decide anything
        constexpr std::string_view kBase64Suffix = "Base64";
        /// @brief Internal id counters of the design graph, never useful to the model
        constexpr std::array<std::string_view, 4> kInternalCounters = {
          "nextReqId", "nextNodeId", "nextVariantId", "nextConfigId"};

        /// @brief Removes values that carry no information for the model
        void dropEmptyValues(nlohmann::json & obj)
        {
            for (auto it = obj.begin(); it != obj.end();)
            {
                bool const empty = it->is_null() || it->is_string()
                                 || (it->is_array() && it->empty())
                                 || (it->is_object() && it->empty());
                it = empty ? obj.erase(it) : std::next(it);
            }
        }

        /// @brief True when an evaluation block holds nothing the model acted on
        [[nodiscard]] bool isBlankEvaluation(nlohmann::json const & evaluation)
        {
            if (!evaluation.is_object())
            {
                return true;
            }
            auto const emptyList = [&evaluation](char const * key)
            {
                auto const it = evaluation.find(key);
                return it == evaluation.end() || (it->is_array() && it->empty());
            };
            auto const score = evaluation.find("pruningScore");
            bool const noScore = score == evaluation.end() || !score->is_number()
                              || score->get<double>() == 0.0;
            return emptyList("deterministicWarnings") && emptyList("heuristicNotes")
                && emptyList("subjectiveNotes") && noScore;
        }

        /**
         * @brief Keeps whole collection entries until the budget is spent, marking what was dropped.
         * @details Cutting mid-entry would produce invalid JSON the model then has to guess about.
         */
        void capCollection(nlohmann::json & arr, std::size_t maxBytes, char const * name)
        {
            if (!arr.is_array() || arr.dump().size() <= maxBytes)
            {
                return;
            }
            nlohmann::json kept = nlohmann::json::array();
            std::size_t bytes = 2; // the array brackets
            for (auto const & item : arr)
            {
                std::size_t const itemBytes = item.dump().size() + 1;
                if (bytes + itemBytes > maxBytes)
                {
                    break;
                }
                bytes += itemBytes;
                kept.push_back(item);
            }
            arr = std::move(kept);
            arr.push_back({{"trimmed", name},
                           {"note", "newest entries not shown; ask for a narrower request"}});
        }

        /// @brief Splits a program listing into `// Function:` blocks, header included
        [[nodiscard]] std::vector<std::string> splitFunctions(std::string const & snippet)
        {
            std::vector<std::string> blocks;
            std::string_view const remaining{snippet};
            std::size_t pos = 0;
            while (true)
            {
                auto const next = remaining.find("// Function:", pos);
                if (next == std::string_view::npos)
                {
                    std::string tail{remaining.substr(pos)};
                    if (!tail.empty())
                    {
                        blocks.push_back(std::move(tail));
                    }
                    break;
                }
                if (next > pos)
                {
                    blocks.emplace_back(remaining.substr(pos, next - pos));
                }
                auto const end = remaining.find("// Function:", next + 1);
                blocks.emplace_back(remaining.substr(
                  next, end == std::string_view::npos ? std::string_view::npos : end - next));
                pos = end == std::string_view::npos ? remaining.size() : end;
            }
            return blocks;
        }
    } // namespace

    nlohmann::json compactDesignGraph(nlohmann::json graph)
    {
        if (!graph.is_object())
        {
            return graph;
        }

        for (auto const & key : kInternalCounters)
        {
            graph.erase(std::string{key});
        }

        for (auto const section : {"variants", "configurations"})
        {
            auto found = graph.find(section);
            if (found == graph.end() || !found->is_array())
            {
                continue;
            }
            for (auto & entry : *found)
            {
                if (!entry.is_object())
                {
                    continue;
                }
                for (auto it = entry.begin(); it != entry.end();)
                {
                    bool const isPreview = it.key().size() > kBase64Suffix.size()
                                        && it.key().compare(it.key().size() - kBase64Suffix.size(),
                                                            kBase64Suffix.size(), kBase64Suffix) == 0;
                    it = isPreview ? entry.erase(it) : std::next(it);
                }
                if (isBlankEvaluation(entry.value("evaluation", nlohmann::json{})))
                {
                    entry.erase("evaluation");
                }
                dropEmptyValues(entry);
            }
            capCollection(*found, 4096, section);
        }

        capCollection(graph["nodes"], 4096, "nodes");
        capCollection(graph["requirements"], 2048, "requirements");
        return graph;
    }

    std::string capProgramListing(std::string const & snippet, std::size_t maxChars)
    {
        if (snippet.size() <= maxChars)
        {
            return snippet;
        }

        std::string out;
        for (auto const & block : splitFunctions(snippet))
        {
            if (out.size() + block.size() > maxChars)
            {
                break;
            }
            out += block;
        }
        if (out.empty())
        {
            // A single function larger than the budget still has to return something usable.
            return snippet.substr(0, maxChars) + kTruncationNote;
        }
        return out + kTruncationNote;
    }

    std::string filterProgramListing(std::string const & snippet,
                                     std::vector<std::string> const & wanted)
    {
        if (wanted.empty())
        {
            return snippet;
        }

        auto const headerName = [](std::string const & block) -> std::string
        {
            auto const start = block.find("(ID:");
            if (start == std::string::npos)
            {
                return {};
            }
            std::string const before = block.substr(0, start);
            auto const colon = before.find(':');
            std::string name = before.substr(colon == std::string::npos ? 0 : colon + 1);
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            return name;
        };

        std::string out;
        for (auto const & block : splitFunctions(snippet))
        {
            std::string const name = headerName(block);
            if (std::find(wanted.begin(), wanted.end(), name) != wanted.end())
            {
                out += block;
            }
        }
        return out;
    }
} // namespace gladius::design::agent_tool_output
