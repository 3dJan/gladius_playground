#include "CreateNodeChoices.h"

#include "../nodes/DerivedNodes.h"

#include <algorithm>
#include <map>
#include <utility>

namespace gladius::ui
{
    namespace
    {
        std::string makeChoiceLabel(std::string const & itemName, std::string const & inputName)
        {
            auto label = itemName;
            label += " :: ";
            label += inputName;
            return label;
        }
    } // namespace

    std::vector<CreateNodeChoice> collectCompatibleCreateNodeChoices(
      nodes::Model const & currentModel,
      nodes::Port const & sourcePort,
      nodes::Assembly const & assembly,
      nodes::NodeTypes & nodeTypes)
    {
        std::vector<CreateNodeChoice> choices;
        auto const sourceType = sourcePort.getTypeIndex();

        nodes::staticFor(
          nodeTypes,
          [&choices, sourceType](auto, auto & node)
          {
              if (node.getCategory() == nodes::Category::Internal ||
                  dynamic_cast<nodes::FunctionCall *>(&node) != nullptr)
              {
                  return;
              }

              for (auto & [inputName, parameter] : node.parameter())
              {
                  if (!parameter.isVisible() || parameter.isInternal() ||
                      !node.canAcceptInputType(inputName, sourceType))
                  {
                      continue;
                  }

                  CreateNodeChoiceId id;
                  id.kind = CreateNodeChoiceKind::NodePrototype;
                  id.nodeTypeName = node.name();
                  id.inputName = inputName;
                  choices.push_back(
                    {std::move(id), &node, node.getCategory(), makeChoiceLabel(node.name(), inputName)});
              }
          });

        std::map<std::string, std::size_t> functionNameCounts;
        for (auto const & [functionId, functionModel] : assembly.getFunctions())
        {
            if (!functionModel || functionModel.get() == &currentModel ||
                functionId == currentModel.getResourceId())
            {
                continue;
            }

            auto * beginNode = functionModel->getBeginNode();
            if (beginNode == nullptr || functionModel->getEndNode() == nullptr)
            {
                continue;
            }

            bool const hasCompatibleInput =
              std::any_of(beginNode->outputs().begin(),
                          beginNode->outputs().end(),
                          [sourceType](auto const & portEntry)
                          {
                              auto const & inputPort = portEntry.second;
                              return inputPort.isVisible() &&
                                     inputPort.getTypeIndex() == sourceType;
                          });
            if (!hasCompatibleInput)
            {
                continue;
            }

            auto const functionName = functionModel->getDisplayName().value_or("Function");
            ++functionNameCounts[functionName];
        }

        for (auto const & [functionId, functionModel] : assembly.getFunctions())
        {
            if (!functionModel || functionModel.get() == &currentModel ||
                functionId == currentModel.getResourceId())
            {
                continue;
            }

            auto * beginNode = functionModel->getBeginNode();
            if (beginNode == nullptr || functionModel->getEndNode() == nullptr)
            {
                continue;
            }

            auto functionName = functionModel->getDisplayName().value_or("Function");
            auto const nameCount = functionNameCounts.find(functionName);
            if (nameCount != functionNameCounts.end() && nameCount->second > 1)
            {
                functionName += " (" + std::to_string(functionId) + ")";
            }
            for (auto const & [inputName, inputPort] : beginNode->outputs())
            {
                if (!inputPort.isVisible() || inputPort.getTypeIndex() != sourceType)
                {
                    continue;
                }

                CreateNodeChoiceId id;
                id.kind = CreateNodeChoiceKind::Function;
                id.functionId = functionId;
                id.inputName = inputName;
                choices.push_back({std::move(id),
                                   nullptr,
                                   nodes::Category::Misc,
                                   makeChoiceLabel(functionName, inputName)});
            }
        }

        std::stable_sort(choices.begin(),
                         choices.end(),
                         [](CreateNodeChoice const & lhs, CreateNodeChoice const & rhs)
                         {
                             if (lhs.id.kind != rhs.id.kind)
                             {
                                 return lhs.id.kind == CreateNodeChoiceKind::NodePrototype;
                             }
                             if (lhs.id.kind == CreateNodeChoiceKind::NodePrototype &&
                                 lhs.category != rhs.category)
                             {
                                 return lhs.category < rhs.category;
                             }
                             return lhs.label < rhs.label;
                         });

        return choices;
    }

    void CreateNodeChoiceHistory::record(std::type_index sourceType,
                                         CreateNodeChoiceId const & choice)
    {
        auto & recentChoices = m_choicesBySourceType[sourceType];
        auto const existing = std::find(recentChoices.begin(), recentChoices.end(), choice);
        if (existing != recentChoices.end())
        {
            recentChoices.erase(existing);
        }

        recentChoices.insert(recentChoices.begin(), choice);
        if (recentChoices.size() > MaxRecentChoices)
        {
            recentChoices.resize(MaxRecentChoices);
        }
    }

    std::vector<CreateNodeChoiceId> CreateNodeChoiceHistory::getRecent(
      std::type_index sourceType) const
    {
        auto const iter = m_choicesBySourceType.find(sourceType);
        if (iter == m_choicesBySourceType.end())
        {
            return {};
        }
        return iter->second;
    }

    void CreateNodeChoiceHistory::clear()
    {
        m_choicesBySourceType.clear();
    }
} // namespace gladius::ui