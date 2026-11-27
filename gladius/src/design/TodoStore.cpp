#include "TodoStore.h"

#include <algorithm>
#include <optional>
#include <sstream>

namespace gladius::design
{
    ChecklistEntry checklistEntryFromJson(nlohmann::json const & json)
    {
        if (json.is_string())
        {
            return ChecklistEntry{json.get<std::string>(), false};
        }
        ChecklistEntry entry;
        entry.label = json.value("label", "");
        entry.checked = json.value("checked", false);
        entry.kind = checklistKindFromString(json.value("kind", "manual"));
        entry.scopeNodeId = json.value("scopeNodeId", "");
        entry.assumed = json.value("assumed", false);
        if (json.contains("evidence") && json["evidence"].is_object())
        {
            entry.evidence = json["evidence"];
        }
        return entry;
    }

    namespace
    {
        /// @brief Serialises a checklist entry, omitting fields that carry no information
        nlohmann::json checklistEntryToJson(ChecklistEntry const & entry)
        {
            nlohmann::json json = {{"label", entry.label}, {"checked", entry.checked}};
            if (entry.kind != ChecklistKind::Manual)
            {
                json["kind"] = toString(entry.kind);
            }
            if (!entry.scopeNodeId.empty())
            {
                json["scopeNodeId"] = entry.scopeNodeId;
            }
            if (!entry.evidence.empty() && !entry.evidence.is_null())
            {
                json["evidence"] = entry.evidence;
            }
            if (entry.assumed)
            {
                json["assumed"] = true;
            }
            return json;
        }

        /// @brief Applies a gate check to one entry of `item`, or reports why it cannot be applied
        bool applyGateCheck(TodoItem & item,
                            std::size_t index,
                            nlohmann::json const & evidence,
                            std::string * reason)
        {
            if (index >= item.checklist.size())
            {
                if (reason != nullptr)
                {
                    *reason = "no checklist entry at index " + std::to_string(index);
                }
                return false;
            }
            ChecklistEntry & entry = item.checklist[index];
            if (!isDeterministic(entry.kind))
            {
                if (reason != nullptr)
                {
                    *reason = std::string(toString(entry.kind)) + " entries are not gate checked";
                }
                return false;
            }
            entry.checked = true;
            entry.evidence = evidence;
            return true;
        }
    } // namespace

    bool TodoStore::checkByGate(std::string const & id,
                                std::size_t index,
                                nlohmann::json evidence,
                                std::string * reason)
    {
        auto it = std::find_if(m_items.begin(), m_items.end(), [&](TodoItem const & item) {
            return item.id == id;
        });
        if (it == m_items.end())
        {
            if (reason != nullptr)
            {
                *reason = "unknown todo id " + id;
            }
            return false;
        }
        return applyGateCheck(*it, index, evidence, reason);
    }

    std::string TodoStore::addItem(std::string title,
                                   std::string description,
                                   std::vector<ChecklistEntry> checklist)
    {
        std::string const id = "todo_" + std::to_string(m_nextId++);
        m_items.push_back(TodoItem{
          id, std::move(title), std::move(description), TodoStatus::Pending, std::move(checklist)});
        return id;
    }

    bool TodoStore::updateItem(std::string const & id,
                               TodoStatus status,
                               std::string const & description,
                               std::vector<ChecklistEntry> const * checklist)
    {
        auto it = std::find_if(m_items.begin(), m_items.end(), [&](TodoItem const & item) {
            return item.id == id;
        });
        if (it == m_items.end())
        {
            return false;
        }
        it->status = status;
        if (!description.empty())
        {
            it->description = description;
        }
        if (checklist != nullptr)
        {
            it->checklist = *checklist;
        }
        return true;
    }

    bool TodoStore::toggleChecklistEntry(std::string const & id, std::size_t index, bool checked)
    {
        auto it = std::find_if(m_items.begin(), m_items.end(), [&](TodoItem const & item) {
            return item.id == id;
        });
        if (it == m_items.end() || index >= it->checklist.size())
        {
            return false;
        }
        ChecklistEntry & entry = it->checklist[index];
        if (isDeterministic(entry.kind))
        {
            // Deterministic entries are only ever checked by a gate evaluator with evidence.
            return false;
        }
        entry.checked = checked;
        return true;
    }

    std::vector<ChecklistGap> TodoStore::uncheckedDeterministicEntries() const
    {
        std::vector<ChecklistGap> gaps;
        for (auto const & item : m_items)
        {
            for (std::size_t i = 0; i < item.checklist.size(); ++i)
            {
                auto const & entry = item.checklist[i];
                if (isDeterministic(entry.kind) && !entry.checked)
                {
                    gaps.push_back(ChecklistGap{item.id, i, entry.label, entry.kind, entry.scopeNodeId});
                }
            }
        }
        return gaps;
    }

    std::vector<ChecklistGap> TodoStore::uncheckedDeterministicEntries(GateEvaluator const & evaluate)
    {
        for (auto & item : m_items)
        {
            for (std::size_t i = 0; i < item.checklist.size(); ++i)
            {
                auto const & entry = item.checklist[i];
                if (!isDeterministic(entry.kind) || entry.checked)
                {
                    continue;
                }
                auto const verdict = evaluate(entry);
                if (verdict.passed)
                {
                    applyGateCheck(item, i, verdict.evidence, nullptr);
                }
            }
        }
        return uncheckedDeterministicEntries();
    }

    std::vector<ChecklistGap> TodoStore::uncheckedDeterministicEntries(std::string const & nodeId,
                                                                       bool includeGlobal) const
    {
        std::vector<ChecklistGap> gaps;
        for (auto const & gap : uncheckedDeterministicEntries())
        {
            if (gap.scopeNodeId == nodeId || (includeGlobal && gap.scopeNodeId.empty()))
            {
                gaps.push_back(gap);
            }
        }
        return gaps;
    }

    std::string TodoStore::summarize() const
    {
        if (m_items.empty())
        {
            return "";
        }
        std::ostringstream out;
        for (auto const & item : m_items)
        {
            out << "- [" << toString(item.status) << "] " << item.id << ": " << item.title;
            if (!item.checklist.empty())
            {
                std::size_t doneCount = 0;
                for (auto const & c : item.checklist)
                {
                    if (c.checked)
                    {
                        ++doneCount;
                    }
                }
                out << " (" << doneCount << "/" << item.checklist.size() << " checks)";
            }
            out << "\n";
        }
        return out.str();
    }

    nlohmann::json TodoStore::toJson() const
    {
        nlohmann::json arr = nlohmann::json::array();
        for (auto const & item : m_items)
        {
            nlohmann::json checks = nlohmann::json::array();
            for (auto const & c : item.checklist)
            {
                checks.push_back(checklistEntryToJson(c));
            }
            arr.push_back({
              {"id", item.id},
              {"title", item.title},
              {"description", item.description},
              {"status", toString(item.status)},
              {"checklist", std::move(checks)}});
        }
        return {{"items", std::move(arr)}, {"nextId", m_nextId}};
    }

    bool TodoStore::fromJson(nlohmann::json const & json)
    {
        if (!json.is_object())
        {
            return false;
        }
        clear();
        if (json.contains("items") && json["items"].is_array())
        {
            for (auto const & j : json["items"])
            {
                TodoItem item;
                item.id = j.value("id", "");
                item.title = j.value("title", "");
                item.description = j.value("description", "");
                item.status = todoStatusFromString(j.value("status", "pending"));
                if (j.contains("checklist") && j["checklist"].is_array())
                {
                    for (auto const & c : j["checklist"])
                    {
                        item.checklist.push_back(checklistEntryFromJson(c));
                    }
                }
                if (!item.id.empty())
                {
                    m_items.push_back(std::move(item));
                }
            }
        }
        m_nextId = json.value("nextId", m_items.size() + 1);
        return true;
    }

    void TodoStore::clear()
    {
        m_items.clear();
        m_nextId = 1;
    }
} // namespace gladius::design
