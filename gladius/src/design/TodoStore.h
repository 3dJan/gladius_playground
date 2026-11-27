#pragma once

#include <functional>
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

    /// @brief What proves a checklist entry, and who is allowed to mark it checked
    /// @details Entries with a deterministic `kind` are only ever checked by a gate evaluator that
    ///          records its measurement in `evidence`; the model cannot self-certify them.
    enum class ChecklistKind
    {
        Manual,
        Geometry,
        Manufacturing,
        Renderability,
        Interface,
        DomainBounds,
        FlatNodeBudget,
        User
    };

    [[nodiscard]] inline char const * toString(ChecklistKind kind) noexcept
    {
        switch (kind)
        {
        case ChecklistKind::Manual:
            return "manual";
        case ChecklistKind::Geometry:
            return "geometry";
        case ChecklistKind::Manufacturing:
            return "manufacturing";
        case ChecklistKind::Renderability:
            return "renderability";
        case ChecklistKind::Interface:
            return "interface";
        case ChecklistKind::DomainBounds:
            return "domain_bounds";
        case ChecklistKind::FlatNodeBudget:
            return "flat_node_budget";
        case ChecklistKind::User:
            return "user";
        }
        return "manual";
    }

    [[nodiscard]] inline ChecklistKind checklistKindFromString(std::string const & str) noexcept
    {
        if (str == "geometry")
            return ChecklistKind::Geometry;
        if (str == "manufacturing")
            return ChecklistKind::Manufacturing;
        if (str == "renderability")
            return ChecklistKind::Renderability;
        if (str == "interface")
            return ChecklistKind::Interface;
        if (str == "domain_bounds")
            return ChecklistKind::DomainBounds;
        if (str == "flat_node_budget")
            return ChecklistKind::FlatNodeBudget;
        if (str == "user")
            return ChecklistKind::User;
        return ChecklistKind::Manual;
    }

    /// @brief Names a checklist kind, refusing unknown spellings instead of defaulting to Manual
    [[nodiscard]] inline std::optional<ChecklistKind> parseChecklistKind(std::string const & str) noexcept
    {
        if (str == "manual" || str == "geometry" || str == "manufacturing" || str == "renderability" ||
            str == "interface" || str == "domain_bounds" || str == "flat_node_budget" || str == "user")
        {
            return checklistKindFromString(str);
        }
        return std::nullopt;
    }

    /// @brief True when only a deterministic gate or a named human decision may check it
    [[nodiscard]] inline bool isDeterministic(ChecklistKind kind) noexcept
    {
        return kind != ChecklistKind::Manual && kind != ChecklistKind::User;
    }

    /// @brief Individual checklist step inside a TodoItem
    struct ChecklistEntry
    {
        std::string label;
        bool checked = false;
        /// @brief Gate that proves the entry; Manual entries are model or human judgement
        ChecklistKind kind = ChecklistKind::Manual;
        /// @brief Design node this entry constrains; empty for whole-design entries
        std::string scopeNodeId;
        /// @brief Measurement recorded by the gate that checked the entry
        nlohmann::json evidence = nlohmann::json::object();
        /// @brief True when no user was asked and a default was assumed instead
        bool assumed = false;
    };

    /// @brief Reads one checklist entry from a plain label string or an extended object
    /// @details Accepts the legacy `{"label", "checked"}` payload and bare strings unchanged.
    [[nodiscard]] ChecklistEntry checklistEntryFromJson(nlohmann::json const & json);

    /// @brief One deterministic checklist entry that still lacks gate evidence
    struct ChecklistGap
    {
        std::string todoId;
        std::size_t index = 0;
        std::string label;
        ChecklistKind kind = ChecklistKind::Manual;
        std::string scopeNodeId;
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
        /// @brief Schema fragment describing checklist entries accepted by the todo tools
        /// @details Accepts a plain requirement label or an extended object carrying a gate `kind`.
        static nlohmann::json checklistSchema()
        {
            nlohmann::json objectEntry = {{"type", "object"},
                                          {"required", {"label"}},
                                          {"properties",
                                           {{"label", {{"type", "string"}}},
                                            {"checked", {{"type", "boolean"}}},
                                            {"scopeNodeId", {{"type", "string"}}},
                                            {"evidence", {{"type", "object"}}},
                                            {"assumed", {{"type", "boolean"}}}}}};
            objectEntry["properties"]["kind"] = {{"type", "string"},
                                                 {"enum",
                                                  {"manual",
                                                   "geometry",
                                                   "manufacturing",
                                                   "renderability",
                                                   "interface",
                                                   "domain_bounds",
                                                   "flat_node_budget",
                                                   "user"}}};
            nlohmann::json entry = {{"oneOf", {{{"type", "string"}}, std::move(objectEntry)}}};
            return {{"type", "array"}, {"items", std::move(entry)}};
        }

        std::string addItem(std::string title,
                            std::string description = "",
                            std::vector<ChecklistEntry> checklist = {});

        bool updateItem(std::string const & id,
                        TodoStatus status,
                        std::string const & description = "",
                        std::vector<ChecklistEntry> const * checklist = nullptr);

        /// @brief Model or human check of an entry; refuses deterministic kinds
        /// @details A deterministic entry is only ever checked by `checkByGate`, so the model cannot
        ///          certify its own geometry by flipping a flag.
        bool toggleChecklistEntry(std::string const & id, std::size_t index, bool checked);

        /// @brief Marks a deterministic entry checked together with the measurement that proves it
        /// @param evidence Gate output stored on the entry; copied into the session JSON
        /// @param reason Optional out parameter describing why the check was refused
        bool checkByGate(std::string const & id,
                         std::size_t index,
                         nlohmann::json evidence,
                         std::string * reason = nullptr);

        /**
         * @brief Gate verdict for one checklist entry.
         * @details A gate either proves the requirement and hands back its measurement, or explains
         *          why it cannot; there is no third state in which the model may certify itself.
         */
        struct GateVerdict
        {
            bool passed = false;
            std::string message;
            nlohmann::json evidence = nlohmann::json::object();
        };

        /// @brief Evaluates one deterministic checklist entry against the live design
        using GateEvaluator = std::function<GateVerdict(ChecklistEntry const &)>;

        /// @brief Deterministic entries that are not checked yet, in item order
        [[nodiscard]] std::vector<ChecklistGap> uncheckedDeterministicEntries() const;

        /**
         * @brief Deterministic entries still unchecked after `evaluate` has tried them.
         * @details Entries the gate accepts are checked with their evidence recorded, so a model that
         *          genuinely satisfies a requirement needs no hand-checking, while an entry the gate
         *          refused keeps the measurement that explains the failure.
         */
        [[nodiscard]] std::vector<ChecklistGap> uncheckedDeterministicEntries(GateEvaluator const & evaluate);

        /// @brief Deterministic entries scoped to `nodeId`, plus unscoped ones when `includeGlobal`
        [[nodiscard]] std::vector<ChecklistGap> uncheckedDeterministicEntries(std::string const & nodeId,
                                                                             bool includeGlobal) const;

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
