#pragma once

#include "nodes/types.h"

#include <vector>

namespace gladius::ui
{

    /**
     * @brief Manages navigation history for function/model browsing
     *
     * Tracks visited functions in a history stack, supporting back/forward
     * navigation similar to a web browser. When navigating to a new function
     * while not at the end of history, forward history is truncated.
     */
    class FunctionNavigationHistory
    {
      public:
        FunctionNavigationHistory() = default;
        ~FunctionNavigationHistory() = default;

        // Non-copyable, movable
        FunctionNavigationHistory(FunctionNavigationHistory const &) = delete;
        FunctionNavigationHistory & operator=(FunctionNavigationHistory const &) = delete;
        FunctionNavigationHistory(FunctionNavigationHistory &&) = default;
        FunctionNavigationHistory & operator=(FunctionNavigationHistory &&) = default;

        /**
         * @brief Record a navigation to a new function
         * @param currentId The current function ID before navigation (0 if none)
         * @param targetId The function ID being navigated to
         * @return true if navigation should proceed (not a no-op)
         *
         * If navigating while not at the end of history, forward history is truncated.
         * If history is empty, the current function is added first.
         */
        bool recordNavigation(nodes::ResourceId currentId, nodes::ResourceId targetId);

        /**
         * @brief Check if back navigation is possible
         * @return true if there is history to go back to
         */
        bool canGoBack() const;

        /**
         * @brief Check if forward navigation is possible
         * @return true if there is forward history available
         */
        bool canGoForward() const;

        /**
         * @brief Move back in history
         * @return The function ID to navigate to, or 0 if cannot go back
         */
        nodes::ResourceId goBack();

        /**
         * @brief Move forward in history
         * @return The function ID to navigate to, or 0 if cannot go forward
         */
        nodes::ResourceId goForward();

        /**
         * @brief Reset history with an initial function
         * @param initialId The function ID to start history with (0 for empty)
         */
        void reset(nodes::ResourceId initialId = 0);

        /**
         * @brief Set whether currently navigating via history (back/forward)
         * @param inNav true if currently performing history navigation
         *
         * When true, recordNavigation() will not modify history.
         */
        void setInHistoryNavigation(bool inNav);

        /**
         * @brief Check if currently navigating via history
         * @return true if in history navigation mode
         */
        bool isInHistoryNavigation() const;

      private:
        std::vector<nodes::ResourceId> m_history;
        std::size_t m_index{0};
        bool m_inHistoryNav{false};
    };

} // namespace gladius::ui
