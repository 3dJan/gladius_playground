#pragma once

#include "nodes/types.h"
#include "nodesfwd.h"

#include <vector>

namespace gladius::ui
{

    /**
     * @brief Entry in the navigation history containing function ID and anchor node
     *
     * The anchor node is the node that should be centered when returning to this
     * function via back/forward navigation. This provides a "you came from here"
     * view restoration experience.
     */
    struct NavigationHistoryEntry
    {
        nodes::ResourceId functionId{0};
        nodes::NodeId anchorNode{0}; ///< Node to center on when returning (0 = use NavigateToContent)
    };

    /**
     * @brief Manages navigation history for function/model browsing
     *
     * Tracks visited functions in a history stack, supporting back/forward
     * navigation similar to a web browser. When navigating to a new function
     * while not at the end of history, forward history is truncated.
     *
     * Each history entry stores an optional "anchor node" - the node that was
     * clicked to trigger navigation. When returning via back/forward, the view
     * is centered on this anchor node for better context restoration.
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
         * @param sourceNode The node that was clicked to trigger navigation (0 if none)
         * @return true if navigation should proceed (not a no-op)
         *
         * If navigating while not at the end of history, forward history is truncated.
         * If history is empty, the current function is added first.
         * The sourceNode is stored as the anchor for the current function entry.
         */
        bool recordNavigation(nodes::ResourceId currentId, 
                              nodes::ResourceId targetId,
                              nodes::NodeId sourceNode = 0);

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
         * @return The history entry to navigate to (functionId=0 if cannot go back)
         */
        NavigationHistoryEntry goBack();

        /**
         * @brief Move forward in history
         * @return The history entry to navigate to (functionId=0 if cannot go forward)
         */
        NavigationHistoryEntry goForward();

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
        std::vector<NavigationHistoryEntry> m_history;
        std::size_t m_index{0};
        bool m_inHistoryNav{false};
    };

} // namespace gladius::ui
