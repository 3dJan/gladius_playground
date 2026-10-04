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

    bool isSdfDifferenceLibraryFunction(nodes::Model & functionModel)
    {
        auto const displayName = functionModel.getDisplayName();
        if (!displayName.has_value() || *displayName != "subtraction" ||
            functionModel.getBeginNode() == nullptr || functionModel.getEndNode() == nullptr)
        {
            return false;
        }

        auto const & inputs = functionModel.getInputs();
        auto const & outputs = functionModel.getOutputs();
        auto const shapeA = inputs.find("shapeA");
        auto const shapeB = inputs.find("shapeB");
        auto const shape = outputs.find("shape");
        if (inputs.size() != 2 || outputs.size() != 1 || shapeA == inputs.end() ||
            shapeA->second.getTypeIndex() != nodes::ParameterTypeIndex::Float ||
            shapeB == inputs.end() ||
            shapeB->second.getTypeIndex() != nodes::ParameterTypeIndex::Float ||
            shape == outputs.end() ||
            shape->second.getTypeIndex() != nodes::ParameterTypeIndex::Float ||
            functionModel.getSize() != 5)
        {
            return false;
        }

        nodes::ConstantScalar * negativeOne = nullptr;
        nodes::Multiplication * negateShapeB = nullptr;
        nodes::Max * difference = nullptr;
        for (auto & nodeEntry : functionModel)
        {
            auto * node = nodeEntry.second.get();
            if (auto * constant = dynamic_cast<nodes::ConstantScalar *>(node))
            {
                if (negativeOne != nullptr)
                {
                    return false;
                }
                negativeOne = constant;
            }
            else if (auto * multiplication = dynamic_cast<nodes::Multiplication *>(node))
            {
                if (negateShapeB != nullptr)
                {
                    return false;
                }
                negateShapeB = multiplication;
            }
            else if (auto * maximum = dynamic_cast<nodes::Max *>(node))
            {
                if (difference != nullptr)
                {
                    return false;
                }
                difference = maximum;
            }
            else if (node != functionModel.getBeginNode() && node != functionModel.getEndNode())
            {
                return false;
            }
        }

        if (negativeOne == nullptr || negativeOne->getValue() != -1.0f ||
            negateShapeB == nullptr || difference == nullptr)
        {
            return false;
        }

        auto const isConnectedToPort = [](nodes::VariantParameter const & input,
                                          nodes::Port const & expectedPort)
        {
            auto const & source = input.getConstSource();
            return source.has_value() && source->portId == expectedPort.getId() &&
                   source->nodeId == expectedPort.getParentId();
        };

        auto const * negativeOnePort = &negativeOne->getValueOutputPort();
        auto const * negatedShapeBPort = &negateShapeB->getResultOutputPort();
        auto const negatesShapeB =
          (isConnectedToPort(negateShapeB->getInputA(), *negativeOnePort) &&
           isConnectedToPort(negateShapeB->getInputB(), shapeB->second)) ||
          (isConnectedToPort(negateShapeB->getInputB(), *negativeOnePort) &&
           isConnectedToPort(negateShapeB->getInputA(), shapeB->second));
        auto const computesDifference =
          (isConnectedToPort(difference->getInputA(), shapeA->second) &&
           isConnectedToPort(difference->getInputB(), *negatedShapeBPort)) ||
          (isConnectedToPort(difference->getInputB(), shapeA->second) &&
           isConnectedToPort(difference->getInputA(), *negatedShapeBPort));

        return negatesShapeB && computesDifference &&
               isConnectedToPort(shape->second, difference->getResultOutputPort());
    }

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

        if (sourceType == nodes::ParameterTypeIndex::Float)
        {
            auto addBooleanAlias = [&choices, &nodeTypes, &sourceType](
                                    std::string const & nodeTypeName,
                                    CreateNodeChoiceSemantic semantic,
                                    std::string const & label)
            {
                nodes::staticFor(
                  nodeTypes,
                  [&choices, &sourceType, &nodeTypeName, semantic, &label](auto, auto & node)
                  {
                      if (node.name() != nodeTypeName || !node.canAcceptInputType("A", sourceType))
                      {
                          return;
                      }

                      CreateNodeChoiceId id;
                      id.kind = CreateNodeChoiceKind::NodePrototype;
                      id.nodeTypeName = nodeTypeName;
                      id.inputName = "A";
                      id.semantic = semantic;
                      choices.push_back({std::move(id),
                                         &node,
                                         nodes::Category::BoolOperation,
                                         label});
                  });
            };

            addBooleanAlias("Min", CreateNodeChoiceSemantic::Union, "Union");
            addBooleanAlias("Max", CreateNodeChoiceSemantic::Intersection, "Intersection");

            for (auto const & inputName : {std::string{"shapeA"}, std::string{"shapeB"}})
            {
                CreateNodeChoiceId id;
                id.kind = CreateNodeChoiceKind::LibraryFunction;
                id.inputName = inputName;
                id.semantic = CreateNodeChoiceSemantic::Difference;
                id.libraryAsset = "csg/subtraction.3mf";
                id.functionName = "subtraction";
                auto const label = inputName == "shapeA"
                                     ? "Difference (selected - other)"
                                     : "Difference (other - selected)";
                choices.push_back(
                  {std::move(id), nullptr, nodes::Category::BoolOperation, label});
            }
        }

        std::map<std::string, std::size_t> functionNameCounts;
        for (auto const & [functionId, functionModel] : assembly.getFunctions())
        {
            if (!functionModel || functionModel.get() == &currentModel ||
                functionId == currentModel.getResourceId())
            {
                continue;
            }
            if (isSdfDifferenceLibraryFunction(*functionModel))
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
            if (isSdfDifferenceLibraryFunction(*functionModel))
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
                             bool const lhsIsFunction = lhs.id.kind == CreateNodeChoiceKind::Function;
                             bool const rhsIsFunction = rhs.id.kind == CreateNodeChoiceKind::Function;
                             if (lhsIsFunction != rhsIsFunction)
                             {
                                 return !lhsIsFunction;
                             }
                             if (!lhsIsFunction && lhs.category != rhs.category)
                             {
                                 return lhs.category < rhs.category;
                             }
                             if (lhs.id.kind != rhs.id.kind)
                             {
                                 return lhs.id.kind < rhs.id.kind;
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