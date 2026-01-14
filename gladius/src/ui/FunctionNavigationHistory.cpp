#include "FunctionNavigationHistory.h"

namespace gladius::ui
{

    bool FunctionNavigationHistory::recordNavigation(nodes::ResourceId currentId,
                                                     nodes::ResourceId targetId)
    {
        // Don't record if target is same as current (no-op)
        if (currentId == targetId)
        {
            return false;
        }

        // Skip recording if we're doing a back/forward navigation
        if (m_inHistoryNav)
        {
            return true;
        }

        // If we're not at the end, truncate forward history
        if (!m_history.empty() && (m_index + 1u) < m_history.size())
        {
            m_history.erase(m_history.begin() + static_cast<long>(m_index + 1u), m_history.end());
        }

        // If history is empty, seed with current
        if (m_history.empty() && currentId != 0u)
        {
            m_history.push_back(currentId);
        }

        // Push new target and advance index
        m_history.push_back(targetId);
        m_index = m_history.size() - 1u;

        return true;
    }

    bool FunctionNavigationHistory::canGoBack() const
    {
        return !m_history.empty() && m_index > 0u;
    }

    bool FunctionNavigationHistory::canGoForward() const
    {
        return !m_history.empty() && (m_index + 1u) < m_history.size();
    }

    nodes::ResourceId FunctionNavigationHistory::goBack()
    {
        if (!canGoBack())
        {
            return 0;
        }
        m_index -= 1u;
        return m_history[m_index];
    }

    nodes::ResourceId FunctionNavigationHistory::goForward()
    {
        if (!canGoForward())
        {
            return 0;
        }
        m_index += 1u;
        return m_history[m_index];
    }

    void FunctionNavigationHistory::reset(nodes::ResourceId initialId)
    {
        m_history.clear();
        m_index = 0u;
        if (initialId != 0u)
        {
            m_history.push_back(initialId);
        }
    }

    void FunctionNavigationHistory::setInHistoryNavigation(bool inNav)
    {
        m_inHistoryNav = inNav;
    }

    bool FunctionNavigationHistory::isInHistoryNavigation() const
    {
        return m_inHistoryNav;
    }

} // namespace gladius::ui
