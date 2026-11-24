#include "FunctionCallPortSync.h"

#include "nodes/Assembly.h"
#include "nodes/DerivedNodes.h"
#include "nodes/Model.h"
#include "nodes/NodeBase.h"
#include "nodes/Parameter.h"

#include <set>
#include <vector>

namespace gladius
{
    namespace
    {
        using namespace nodes;

        /// @brief The call nodes of one model, collected before they are modified.
        template <class NodeType>
        std::vector<NodeType*> collectCallNodes(Model& model)
        {
            std::vector<NodeType*> result;
            for (auto& [nodeId, nodePtr] : model)
            {
                (void) nodeId;
                if (auto* typed = dynamic_cast<NodeType*>(nodePtr.get()))
                {
                    result.push_back(typed);
                }
            }
            return result;
        }

        /**
         * @brief Remove the outputs of a call node which the referenced model does not expose.
         *
         * Removing a port is a three step operation: parameters of the owning model that are fed
         * by the port are unlinked first, then the port is unregistered from the owning model and
         * finally it is erased from the call node itself.
         */
        void pruneObsoleteOutputs(
            Model& owningModel,
            NodeBase& callNode,
            std::set<PortName> const& validNames)
        {
            auto& outputs = callNode.getOutputs();

            std::vector<PortName> obsolete;
            for (auto const& [name, port] : outputs)
            {
                if (validNames.find(name) == validNames.end())
                {
                    obsolete.push_back(name);
                }
            }

            auto& portRegistry = owningModel.getPortRegistry();
            auto& parameterRegistry = owningModel.getParameterRegistry();

            for (auto const& name : obsolete)
            {
                auto portIt = outputs.find(name);
                if (portIt == outputs.end())
                {
                    continue;
                }

                auto const portId = portIt->second.getId();

                for (auto& [_, parameter] : parameterRegistry)
                {
                    auto const source = parameter->getSource();
                    if (source.has_value() && source->portId == portId)
                    {
                        parameter->setSource(std::nullopt);
                    }
                }

                auto const registeredIt = portRegistry.find(portId);
                if (registeredIt != portRegistry.end() && registeredIt->second == &portIt->second)
                {
                    portRegistry.erase(registeredIt);
                }

                outputs.erase(portIt);
            }
        }

        /**
         * @brief Drop the unconnected inputs of a call node which the referenced model no longer
         *        declares.
         *
         * A changed signature does not remove the ports of the call node that referenced it, so a
         * root call node keeps an input that can never be sourced. Flattening refuses exactly that
         * input, which makes the whole document unrenderable until the file is re-seeded. Only
         * unconnected inputs are dropped: a port that still has a source is a connection the user
         * made deliberately and pruning it would destroy the source node's output.
         */
        template <class CallType>
        void pruneObsoleteUnconnectedInputs(
            Model& owningModel,
            CallType& callNode,
            std::set<PortName> const& validNames)
        {
            auto& inputs = callNode.parameter();

            std::vector<PortName> obsolete;
            for (auto const& [name, input] : inputs)
            {
                if (name == FieldNames::FunctionId)
                {
                    continue;
                }
                if (validNames.find(name) == validNames.end() && !input.getConstSource().has_value())
                {
                    obsolete.push_back(name);
                }
            }

            auto& inputRegistry = owningModel.getParameterRegistry();
            for (auto const& name : obsolete)
            {
                auto it = inputs.find(name);
                if (it == inputs.end())
                {
                    continue;
                }
                auto const parameterId = it->second.getId();
                auto const registeredIt = inputRegistry.find(parameterId);
                if (registeredIt != inputRegistry.end() && registeredIt->second == &it->second)
                {
                    inputRegistry.erase(registeredIt);
                }
                inputs.erase(it);
            }
        }

        /// @brief The input names a model declares.
        std::set<PortName> collectModelInputs(Model& model)
        {
            std::set<PortName> result;
            for (auto const& [name, _] : model.getInputs())
            {
                result.insert(name);
            }
            return result;
        }

        /// @brief The output names a model exposes.
        std::set<PortName> collectModelOutputs(Model& model)
        {
            std::set<PortName> result;
            for (auto const& [name, _] : model.getOutputs())
            {
                result.insert(name);
            }
            return result;
        }

        /// @brief Re-syncs all call nodes of one model against the models they reference.
        template <class CallType>
        void syncCallsInModel(Model& owningModel, Assembly& assembly)
        {
            for (auto* callNode : collectCallNodes<CallType>(owningModel))
            {
                callNode->resolveFunctionId();
                auto* referencedModel = assembly.findModel(callNode->getFunctionId()).get();
                if (referencedModel == nullptr)
                {
                    continue;
                }

                callNode->updateInputsAndOutputs(*referencedModel);
                pruneObsoleteOutputs(owningModel, *callNode, collectModelOutputs(*referencedModel));
                pruneObsoleteUnconnectedInputs(
                  owningModel, *callNode, collectModelInputs(*referencedModel));

                owningModel.registerInputs(*callNode);
                owningModel.registerOutputs(*callNode);
            }
        }
    }

    void syncFunctionCallPorts(nodes::Assembly& assembly)
    {
        for (auto& [resourceId, model] : assembly.getFunctions())
        {
            (void) resourceId;
            if (model == nullptr)
            {
                continue;
            }

            syncCallsInModel<FunctionCall>(*model, assembly);
            syncCallsInModel<FunctionGradient>(*model, assembly);

            model->updateTypes();
            model->updateGraphAndOrderIfNeeded();
        }
    }
}
