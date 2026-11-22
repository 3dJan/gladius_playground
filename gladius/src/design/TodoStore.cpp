#include "TodoStore.h"

#include <algorithm>
#include <sstream>

namespace gladius::design
{
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
        it->checklist[index].checked = checked;
        return true;
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
                checks.push_back({{"label", c.label}, {"checked", c.checked}});
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
                        item.checklist.push_back(
                          ChecklistEntry{c.value("label", ""), c.value("checked", false)});
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
