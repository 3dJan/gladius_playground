#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace gladius::design
{
    /// @brief Status of a planning todo item
    enum class TodoStatus
    {
        Pending,
        InProgress,
        Done,
        Blocked
    };

    [[nodiscard]] inline char const * toString(TodoStatus status) noexcept
    {
        switch (status)
        {
        case TodoStatus::Pending:
            return "pending";
        case TodoStatus::InProgress:
            return "in_progress";
        case TodoStatus::Done:
            return "done";
        case TodoStatus::Blocked:
            return "blocked";
        }
        return "pending";
    }

    [[nodiscard]] inline TodoStatus todoStatusFromString(std::string const & str) noexcept
    {
        if (str == "in_progress")
            return TodoStatus::InProgress;
        if (str == "done")
            return TodoStatus::Done;
        if (str == "blocked")
            return TodoStatus::Blocked;
        return TodoStatus::Pending;
    }

    /// @brief Individual checklist step inside a TodoItem
    struct ChecklistEntry
    {
        std::string label;
        bool checked = false;
    };

    /// @brief Planning todo item with optional checklist sub-items
    struct TodoItem
    {
        std::string id;
        std::string title;
        std::string description;
        TodoStatus status = TodoStatus::Pending;
        std::vector<ChecklistEntry> checklist;
    };

    /// @brief Manages planning todos and checklists for the design agent
    class TodoStore
    {
      public:
        std::string addItem(std::string title,
                            std::string description = "",
                            std::vector<ChecklistEntry> checklist = {});

        bool updateItem(std::string const & id,
                        TodoStatus status,
                        std::string const & description = "",
                        std::vector<ChecklistEntry> const * checklist = nullptr);

        bool toggleChecklistEntry(std::string const & id, std::size_t index, bool checked);

        [[nodiscard]] std::vector<TodoItem> const & items() const noexcept
        {
            return m_items;
        }

        [[nodiscard]] std::string summarize() const;
        [[nodiscard]] nlohmann::json toJson() const;
        bool fromJson(nlohmann::json const & json);
        void clear();

      private:
        std::vector<TodoItem> m_items;
        std::size_t m_nextId = 1;
    };
} // namespace gladius::design
