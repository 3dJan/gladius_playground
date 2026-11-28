#pragma once

#include "DesignTypes.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace gladius::design
{
    class DesignGraph;

    /// @brief A requirement edit the user made in the UI while a run held the graph
    struct RequirementEdit
    {
        enum class Kind
        {
            Add,
            Update,
            Remove,
        };

        Kind kind = Kind::Update;
        std::string id;
        std::string description;
        std::string category = "functional";
        bool mandatory = true;
    };

    /**
     * @brief Staging area for requirement edits made while the agent owns the design graph.
     * @details The design graph is written by the run thread. While a run is active the UI writes
     *          its requirement edits here instead, and the run thread folds them into the graph at a
     *          turn boundary, where `DesignGraph` records them as user requirement changes and they
     *          reach the model in that same turn's live context.
     */
    class RequirementEditQueue
    {
      public:
        void stageAdd(std::string description, std::string category, bool mandatory);
        void stageUpdate(std::string id,
                         std::string description,
                         std::string category,
                         bool mandatory);
        void stageRemove(std::string id);

        /// @brief Graph requirements with the staged edits applied, for the UI to draw immediately
        [[nodiscard]] std::vector<Requirement> mergedView(DesignGraph const & graph) const;

        /**
         * @brief Applies every staged edit to the graph in submission order (run thread only).
         * @details Edits keep `trackUserChange` enabled, so the graph reports them through its
         *          pending requirement changes and the agent sees them without a new context field.
         */
        void applyTo(DesignGraph & graph);

        [[nodiscard]] std::size_t size() const;
        [[nodiscard]] bool empty() const;

        /// @brief Drops staged edits, e.g. when a run is stopped before they could be applied
        void clear();

      private:
        /// @brief Replaces an older staged edit for the same requirement, keeping submission order
        void replaceOrAppendLocked(RequirementEdit edit);

        mutable std::mutex m_mutex;
        std::vector<RequirementEdit> m_edits;
        /// @brief Sequence for the provisional ids staged additions get until the graph names them
        std::size_t m_nextStagedId = 1;
    };
} // namespace gladius::design
