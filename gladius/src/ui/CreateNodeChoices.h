#pragma once

#include "../nodes/Assembly.h"
#include "../nodes/Model.h"

#include <map>
#include <string>
#include <typeindex>
#include <vector>

namespace gladius::ui
{
    enum class CreateNodeChoiceKind
    {
        NodePrototype,
        Function
    };

    struct CreateNodeChoiceId
    {
        CreateNodeChoiceKind kind{CreateNodeChoiceKind::NodePrototype};
        std::string nodeTypeName;
        nodes::ResourceId functionId{0};
        std::string inputName;

        bool operator==(CreateNodeChoiceId const &) const = default;
    };

    struct CreateNodeChoice
    {
        CreateNodeChoiceId id;
        nodes::NodeBase * prototype{nullptr};
        nodes::Category category{nodes::Category::Internal};
        std::string label;
    };

    /// Collects each built-in input and function argument compatible with a source output.
    [[nodiscard]] std::vector<CreateNodeChoice> collectCompatibleCreateNodeChoices(
      nodes::Model const & currentModel,
      nodes::Port const & sourcePort,
      nodes::Assembly const & assembly,
      nodes::NodeTypes & nodeTypes);

    /// UI-only most-recently-used choices, separated by concrete source output type.
    class CreateNodeChoiceHistory
    {
      public:
        static constexpr std::size_t MaxRecentChoices = 5;

        void record(std::type_index sourceType, CreateNodeChoiceId const & choice);
        [[nodiscard]] std::vector<CreateNodeChoiceId> getRecent(std::type_index sourceType) const;
        void clear();

      private:
        std::map<std::type_index, std::vector<CreateNodeChoiceId>> m_choicesBySourceType;
    };
} // namespace gladius::ui