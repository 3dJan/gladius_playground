#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace gladius::design
{
    /**
     * @brief Trims tool results down to what actually helps the model decide.
     * @details Tool results stay in the conversation for every later turn, so one chatty read is
     *          re-prefilled for the rest of the run. Images inside a JSON result are the worst
     *          offender: a variant preview costs about as many prompt tokens as a viewport capture.
     */
    namespace agent_tool_output
    {
        /// @brief Marker text left where content was removed
        constexpr char const * kTruncationNote =
          "... trimmed; ask again with a narrower request for the rest";

        /**
         * @brief Drops base64 previews and internal id counters from a design-graph dump.
         * @param graph the graph as produced by `DesignGraph::toJson`
         * @return the same structure without the fields that only bloat the prompt
         */
        [[nodiscard]] nlohmann::json compactDesignGraph(nlohmann::json graph);

        /**
         * @brief Keeps at most `maxChars` of a program listing, splitting on function boundaries.
         * @param snippet the full listing
         * @param maxChars budget for the returned listing
         * @return the trimmed listing, ending with `kTruncationNote` when something was dropped
         */
        [[nodiscard]] std::string capProgramListing(std::string const & snippet, std::size_t maxChars);

        /**
         * @brief Keeps only the functions whose display name is in `wanted`.
         * @param snippet the full listing
         * @param wanted function display names, matched case-sensitively against the header line
         * @return the filtered listing; empty when nothing matched
         */
        [[nodiscard]] std::string filterProgramListing(std::string const & snippet,
                                                       std::vector<std::string> const & wanted);
    } // namespace agent_tool_output
} // namespace gladius::design
