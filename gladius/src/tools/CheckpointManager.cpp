#include "CheckpointManager.h"

#include <algorithm>
#include <unordered_set>

namespace gladius::tools
{
    std::string CheckpointManager::createCheckpoint(std::string const & label,
                                                    int level,
                                                    nodes::Assembly const & assembly,
                                                    std::vector<std::byte> previewPng)
    {
        std::string const id = "cp_" + std::to_string(m_nextId++);
        CheckpointSummary summary{
            id,
            label.empty() ? id : label,
            level,
            assembly.getFunctions().size(),
            std::chrono::system_clock::now(),
            std::move(previewPng)};
        m_entries.push_back(Entry{std::move(summary), assembly});
        return id;
    }

    bool CheckpointManager::restoreCheckpoint(std::string const & id, nodes::Assembly & targetAssembly) const
    {
        auto const it = std::find_if(m_entries.begin(), m_entries.end(), [&](Entry const & entry) {
            return entry.summary.id == id;
        });
        if (it == m_entries.end())
        {
            return false;
        }
        targetAssembly = it->assembly;
        return true;
    }

    std::vector<CheckpointSummary> CheckpointManager::listCheckpoints() const
    {
        std::vector<CheckpointSummary> result;
        result.reserve(m_entries.size());
        for (auto const & entry : m_entries)
        {
            result.push_back(entry.summary);
        }
        return result;
    }

    nlohmann::json CheckpointManager::compareCheckpoint(std::string const & id,
                                                        nodes::Assembly const & currentAssembly) const
    {
        auto const it = std::find_if(m_entries.begin(), m_entries.end(), [&](Entry const & entry) {
            return entry.summary.id == id;
        });
        if (it == m_entries.end())
        {
            return {{"success", false}, {"error", "Checkpoint not found: " + id}};
        }

        auto const & cpFuncs = it->assembly.getFunctions();
        auto const & curFuncs = currentAssembly.getFunctions();

        std::vector<nodes::ResourceId> added;
        std::vector<nodes::ResourceId> removed;

        for (auto const & [funcId, _] : curFuncs)
        {
            if (cpFuncs.find(funcId) == cpFuncs.end())
            {
                added.push_back(funcId);
            }
        }
        for (auto const & [funcId, _] : cpFuncs)
        {
            if (curFuncs.find(funcId) == curFuncs.end())
            {
                removed.push_back(funcId);
            }
        }

        return {
            {"success", true},
            {"checkpointId", id},
            {"label", it->summary.label},
            {"checkpointFunctionCount", cpFuncs.size()},
            {"currentFunctionCount", curFuncs.size()},
            {"addedFunctionIds", added},
            {"removedFunctionIds", removed}};
    }

    bool CheckpointManager::hasCheckpoint(std::string const & id) const
    {
        return std::any_of(m_entries.begin(), m_entries.end(), [&](Entry const & entry) {
            return entry.summary.id == id;
        });
    }

    bool CheckpointManager::setFeedback(std::string const & id, std::string feedback)
    {
        auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](Entry const & entry) {
            return entry.summary.id == id;
        });
        if (it == m_entries.end())
        {
            return false;
        }
        it->summary.userFeedback = std::move(feedback);
        return true;
    }

    void CheckpointManager::clear()
    {
        m_entries.clear();
        m_nextId = 1;
    }
} // namespace gladius::tools
