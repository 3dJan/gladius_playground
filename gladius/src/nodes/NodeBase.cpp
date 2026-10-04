#include "NodeBase.h"
#include "Model.h"

namespace gladius::nodes
{
    /// Equal Operator for InputTypeMap
    std::type_index const AnyTypeIndex = std::type_index(typeid(void));

    namespace
    {
        bool matchesTypeRule(ParameterMap const & parameters,
                             TypeRule const & rule,
                             Model const * model,
                             ParameterName const * prospectiveInputName,
                             std::type_index prospectiveInputType)
        {
            if (prospectiveInputName != nullptr &&
                rule.input.find(*prospectiveInputName) == rule.input.end())
            {
                return false;
            }

            for (auto const & [inputName, expectedType] : rule.input)
            {
                auto const parameterIter = parameters.find(inputName);
                if (parameterIter == parameters.end())
                {
                    return false;
                }

                auto actualType = AnyTypeIndex;
                if (prospectiveInputName != nullptr && inputName == *prospectiveInputName)
                {
                    actualType = prospectiveInputType;
                }
                else if (auto const & source = parameterIter->second.getConstSource();
                         source.has_value())
                {
                    if (model == nullptr)
                    {
                        return false;
                    }

                    auto const * sourcePort = model->getPort(source->portId);
                    if (sourcePort == nullptr)
                    {
                        return false;
                    }
                    actualType = sourcePort->getTypeIndex();
                }

                if (actualType != AnyTypeIndex && actualType != expectedType)
                {
                    return false;
                }
            }

            return true;
        }

        template <typename Map>
        void restoreMapContents(Map & target, Map const & snapshot)
        {
            for (auto iter = target.begin(); iter != target.end();)
            {
                if (snapshot.find(iter->first) == snapshot.end())
                {
                    iter = target.erase(iter);
                }
                else
                {
                    ++iter;
                }
            }

            for (auto const & [key, value] : snapshot)
            {
                auto const current = target.find(key);
                if (current == target.end())
                {
                    target.emplace(key, value);
                }
                else
                {
                    current->second = value;
                }
            }
        }
    } // namespace

    bool operator==(const InputTypeMap & lhs, const InputTypeMap & rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        return std::all_of(lhs.begin(),
                           lhs.end(),
                           [&rhs](auto const & lhsPair)
                           {
                               auto const rhsIter = rhs.find(lhsPair.first);
                               return rhsIter != rhs.end() &&
                                      (lhsPair.second == rhsIter->second ||
                                       lhsPair.second == AnyTypeIndex ||
                                       rhsIter->second == AnyTypeIndex);
                           });
    }

    bool NodeBase::canAcceptInputType(ParameterName const & inputName,
                                     std::type_index inputType,
                                     Model const * model) const
    {
        auto const parameterIter = m_parameter.find(inputName);
        if (parameterIter == m_parameter.end())
            return false;

        if (m_typeRules.empty())
        {
            return parameterIter->second.getTypeIndex() == inputType;
        }

        auto const inputIsRuleConstrained =
          std::any_of(m_typeRules.begin(),
                      m_typeRules.end(),
                      [&inputName](TypeRule const & rule)
                      { return rule.input.find(inputName) != rule.input.end(); });

        if (!inputIsRuleConstrained)
        {
            if (parameterIter->second.getTypeIndex() != inputType)
            {
                return false;
            }

            return std::any_of(m_typeRules.begin(),
                               m_typeRules.end(),
                               [&](TypeRule const & rule)
                               {
                                   return matchesTypeRule(m_parameter,
                                                          rule,
                                                          model,
                                                          nullptr,
                                                          AnyTypeIndex);
                               });
        }

        return std::any_of(m_typeRules.begin(),
                           m_typeRules.end(),
                           [&](TypeRule const & rule)
                           {
                               return matchesTypeRule(
                                 m_parameter, rule, model, &inputName, inputType);
                           });
    }

    bool NodeBase::hasCompatibleTypeRule(Model const & model) const
    {
        if (m_typeRules.empty())
        {
            return true;
        }

        auto ruleIter = std::find_if(
          m_typeRules.begin(),
          m_typeRules.end(),
          [&model, this](TypeRule const & rule)
          { return matchesTypeRule(m_parameter, rule, &model, nullptr, AnyTypeIndex); });
        return ruleIter != m_typeRules.end();
    }

    NodeBase::TypeState NodeBase::captureTypeState() const
    {
        return {m_parameter, m_outputs, m_ruleType, m_activeTypeRule};
    }

    void NodeBase::restoreTypeState(TypeState const & state)
    {
        if (state.activeRule.has_value())
        {
            applyTypeRule(*state.activeRule);
        }
        else
        {
            m_activeTypeRule.reset();
        }

        restoreMapContents(m_parameter, state.parameters);
        restoreMapContents(m_outputs, state.outputs);
        m_ruleType = state.ruleType;
        m_activeTypeRule = state.activeRule;
        updateNodeIds();
    }

    bool NodeBase::updateTypes(nodes::Model & model)
    {
        if (m_typeRules.empty())
        {
            return true;
        }

        auto ruleIter = std::find_if(
          m_typeRules.begin(),
          m_typeRules.end(),
          [&model, this](TypeRule const & rule)
          { return matchesTypeRule(m_parameter, rule, &model, nullptr, AnyTypeIndex); });

        // if we have a match, apply the output types
        if (ruleIter != m_typeRules.end())
        {
            applyTypeRule(*ruleIter);
            model.registerOutputs(*this);
            model.registerInputs(*this);
            return true;
        }
        return false;
    };

} // namespace gladius::nodes
