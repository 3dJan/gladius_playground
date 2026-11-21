#include "TransactionManager.h"

namespace gladius::tools
{
    TransactionManager::TransactionManager(CommitCallback onCommit)
        : m_onCommit(std::move(onCommit))
    {
    }

    bool TransactionManager::beginEdit(std::string const & description, nodes::Assembly const & currentAssembly)
    {
        if (m_active)
        {
            return false;
        }
        m_preEditSnapshot.emplace(currentAssembly);
        m_description = description.empty() ? "Agent edit" : description;
        m_active = true;
        return true;
    }

    bool TransactionManager::commitEdit(nodes::Assembly const & currentAssembly)
    {
        if (!m_active || !m_preEditSnapshot.has_value())
        {
            return false;
        }

        bool const changed = !m_preEditSnapshot->equals(currentAssembly);
        if (changed && m_onCommit)
        {
            m_onCommit(*m_preEditSnapshot, m_description);
        }

        m_preEditSnapshot.reset();
        m_description.clear();
        m_active = false;
        return true;
    }

    bool TransactionManager::rollbackEdit(nodes::Assembly & targetAssembly)
    {
        if (!m_active || !m_preEditSnapshot.has_value())
        {
            return false;
        }

        targetAssembly = *m_preEditSnapshot;
        m_preEditSnapshot.reset();
        m_description.clear();
        m_active = false;
        return true;
    }
} // namespace gladius::tools
