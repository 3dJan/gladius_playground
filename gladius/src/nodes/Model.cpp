#include "Model.h"
#include "Assembly.h"
#include "EventLogger.h"
#include "exceptions.h"
#include "graph/GraphAlgorithms.h"
#include "nodesfwd.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <tracy/Tracy.hpp>

namespace gladius::nodes
{

    void printNodeTypes()
    {
        NodeTypes nodeTypes;
        staticFor(nodeTypes,
                  [&](auto i, auto & node)
                  {
                      (void) i;
                      std::cout << node.name() << "\n";
                  });
    }

    bool equalsCaseInsensitive(std::string const & a, std::string const & b)
    {
        return std::equal(a.begin(),
                          a.end(),
                          b.begin(),
                          b.end(),
                          [](char a, char b) { return tolower(a) == tolower(b); });
    }

    NodeBase * createNodeFromName(const std::string & name, Model & nodes)
    {
        NodeBase * createdNode{nullptr};
        NodeTypes nodeTypes;

        staticFor(nodeTypes,
                  [&](auto, auto & node)
                  {
                      if (equalsCaseInsensitive(node.name(), name))
                      {
                          createdNode = nodes.create(node);
                          return;
                      }
                  });
        return createdNode;
    }

    Model::Model(const Model & other)
        : m_lastParameterId(other.m_lastParameterId)
        , m_lastId(other.m_lastId)
        , m_graph(other.m_graph)
        , m_outputOrder(other.m_outputOrder)
        , m_graphRequiresUpdate(other.m_graphRequiresUpdate)
        , m_typesRequireUpdate(true)
        , m_name(other.m_name)
        , m_displayName(other.m_displayName)
        , m_resourceId(other.m_resourceId)
        , m_isManaged(other.m_isManaged)
        , m_allInputReferencesAreValid(other.m_allInputReferencesAreValid)
        , m_nodesHaveBeenLayouted(other.m_nodesHaveBeenLayouted)
        , m_isValid(other.m_isValid)
        , m_numericWidgetLayoutModes(other.m_numericWidgetLayoutModes)
        , m_vectorDisplayModes(other.m_vectorDisplayModes)
    {
        m_outPorts.clear();
        m_inputParameter.clear();
        m_nodes.clear();

        auto cloneNode = [&](auto & node)
        {
            m_nodes[node.second->getId()] = node.second->clone();
            auto & clonedNode = m_nodes[node.second->getId()];
            clonedNode->updateNodeIds();
        };

        for (auto & node : other.m_nodes)
        {
            cloneNode(node);
        }

        for (auto & node : m_nodes)
        {
            for (auto & port : node.second->getOutputs())
            {
                m_outPorts[port.second.getId()] = (&(port.second));
            }

            registerInputs(*node.second);
            node.second->setUniqueName(node.second->getUniqueName()); //???
            node.second->setId(node.second->getId());
        }

        auto beginVisitor =
          OnTypeVisitor<Begin>([&](auto & beginNode) { m_beginNode = &beginNode; });
        visitNodes(beginVisitor);

        auto endVisitor = OnTypeVisitor<End>([&](auto & endNode) { m_endNode = &endNode; });
        visitNodes(endVisitor);

        for (const auto & node : other.m_nodes)
        {
            for (auto & parameter : node.second->parameter())
            {
                if (parameter.second.getSource().has_value())
                {
                    auto const & src = parameter.second.getSource().value();

                    // Primary lookup: by uniqueName (the normal path).
                    auto portIter = std::find_if(std::begin(m_outPorts),
                                                 std::end(m_outPorts),
                                                 [&](auto & port)
                                                 {
                                                     return port.second->getUniqueName() ==
                                                            src.uniqueName;
                                                 });

                    // Fallback: by portId. This handles the case where a port was renamed
                    // after the link was established and the Source::uniqueName became stale.
                    if (portIter == std::end(m_outPorts))
                    {
                        portIter = m_outPorts.find(src.portId);
                    }

                    if (portIter == std::end(m_outPorts))
                    {
                        if (m_logger)
                        {
                            m_logger->addEvent(
                              {fmt::format("Output port with the name {} could not be found — "
                                           "link dropped during model copy",
                                           src.uniqueName),
                               events::Severity::Warning});
                        }
                        // Clear the stale source so the model remains in a consistent state.
                        parameter.second.setSource(std::nullopt);
                        continue;
                    }
                    bool const skipLinkValidation = true;
                    addLink(
                      portIter->second->getId(), parameter.second.getId(), skipLinkValidation);
                }

                parameter.second.setConsumedByFunction(parameter.second.isConsumedByFunction());
            }
        }

        this->updateTypes();
        this->updateOrder();
    }

    std::string extractArgumentName(const std::string & partParameterName,
                                    std::string extendedArgumentName)
    {
        const std::string strToRemove = partParameterName + "_";
        const auto pos = extendedArgumentName.find(strToRemove);

        if (pos != std::string::npos)
        {
            extendedArgumentName.erase(pos, strToRemove.length());
        }
        return extendedArgumentName;
    }

    void Model::updatePartArguments(NodeId partNodeId,
                                    Model & referencedModel,
                                    const std::string & partParameterName)
    {
        const auto partNodeOpt = getNode(partNodeId);
        if (!partNodeOpt.has_value() || partNodeOpt.value() == nullptr)
        {
            return;
        }

        auto * partNode = partNodeOpt.value();
        auto * beginNode = referencedModel.getBeginNode();

        if (beginNode == nullptr)
        {
            throw std::runtime_error("beginNode of referenced Model is not set");
        }

        // Erase existing arguments, that are not contained in the new argument list
        for (auto iter = std::begin(partNode->parameter());
             iter != std::end(partNode->parameter());)
        {
            const auto originalArgumentName = extractArgumentName(partParameterName, iter->first);
            const bool isStillAnArgument = beginNode->getOutputs().find(originalArgumentName) !=
                                           std::end(beginNode->getOutputs());
            if (iter->second.isArgument() && !isStillAnArgument &&
                (iter->second.getArgumentAssoziation().empty() ||
                 (iter->second.getArgumentAssoziation() == partParameterName)))
            {
                iter = partNode->parameter().erase(iter);
            }
            else
            {
                ++iter;
            }
        }

        for (auto & [name, parameter] : beginNode->parameter())
        {
            if (name == FieldNames::Pos)
            {
                continue;
            }

            const auto extendedName = partParameterName + std::string{"_"} + name;
            if (partNode->parameter().find(extendedName) == std::end(partNode->parameter()))
            {
                float initialValue = 0.f;
                auto & initialParameter = parameter.Value();
                if (const auto initialValuePtr = std::get_if<float>(&initialParameter))
                {
                    initialValue = *initialValuePtr;
                }

                partNode->parameter()[extendedName] =
                  VariantParameter(initialValue, parameter.getContentType());
                partNode->parameter()[extendedName].setArgumentAssoziation(partParameterName);
                registerInput(partNode->parameter()[extendedName]);
            }

            if (partNode->getOutputs().find(extendedName) == std::end(partNode->getOutputs()))
            {
                partNode->addOutputPort(extendedName, ParameterTypeIndex::Float);
                partNode->updateNodeIds();
                registerOutput(partNode->getOutputs()[extendedName]);
                partNode->getOutputs()[extendedName].hide();
            }
        }
    }

    void Model::setModelName(const ModelName & modelName)
    {
        m_name = modelName;
    }

    void Model::registerInput(VariantParameter & parameter)
    {
        for (auto iter = m_inputParameter.begin(); iter != m_inputParameter.end();)
        {
            if (iter->second == &parameter)
            {
                if (iter->first == parameter.getId())
                {
                    return;
                }

                iter = m_inputParameter.erase(iter);
            }
            else
            {
                ++iter;
            }
        }

        if (parameter.getId() < 1)
        {
            parameter.setId(20000 + ++m_lastParameterId);
        }

        m_inputParameter[parameter.getId()] = &parameter;
    }

    void Model::registerOutput(Port & port)
    {
        if (port.getId() < 1)
        {
            const auto uppderBound =
              std::max_element(std::begin(m_outPorts),
                               std::end(m_outPorts),
                               [](auto & lhs, auto & rhs) { return lhs.first < rhs.first; });
            if (uppderBound != std::end(m_outPorts))
            {
                port.setId(std::max(10000, uppderBound->first + 1));
            }
            else
            {
                port.setId(10000 + static_cast<int>(m_outPorts.size()));
            }
        }
        m_outPorts[port.getId()] = (&port);
    }

    bool Model::isNodeNameOccupied(const NodeName & name)
    {
        return findNode(name) != nullptr;
    }

    NodeBase * Model::findNode(const NodeName & name)
    {
        const auto iter =
          std::find_if(m_nodes.begin(),
                       m_nodes.end(),
                       [name](auto & node) { return node.second->getUniqueName() == name; });
        if (iter == m_nodes.end())
        {
            return nullptr;
        }

        return iter->second.get();
    }

    std::string & Model::getModelName()
    {
        return m_name;
    }

    Begin * Model::getBeginNode()
    {
        return m_beginNode;
    }

    End * Model::getEndNode()
    {
        return m_endNode;
    }

    void Model::addArgument(ParameterName name, VariantParameter parameter)
    {
        if (m_beginNode == nullptr)
        {
            return;
        }
        parameter.setSortIndex(static_cast<int>(m_beginNode->parameter().size()));
        m_beginNode->parameter()[name] = parameter;
        m_beginNode->addOutputPort(name, parameter.getTypeIndex());
        m_beginNode->getOutputs()[name].setSortIndex(parameter.getSortIndex());
        registerInput(m_beginNode->parameter()[name]);
        registerOutput(m_beginNode->getOutputs()[name]);
    }

    void Model::addFunctionOutput(ParameterName name, VariantParameter parameter)
    {
        if (m_endNode == nullptr)
        {
            return;
        }

        m_endNode->parameter()[name] = parameter;
        registerInput(m_endNode->parameter()[name]);
    }

    void Model::removeArgument(ParameterName const & name)
    {
        if (m_beginNode == nullptr)
        {
            return;
        }

        auto & params = m_beginNode->parameter();
        if (auto it = params.find(name); it != params.end())
        {
            m_numericWidgetLayoutModes.erase(it->second.getId());
            m_vectorDisplayModes.erase(it->second.getId());
            m_inputParameter.erase(it->second.getId());
            params.erase(it);
        }

        auto & outputs = m_beginNode->getOutputs();
        if (auto it = outputs.find(name); it != outputs.end())
        {
            auto const removedPortId = it->second.getId();
            m_outPorts.erase(removedPortId);
            outputs.erase(it);

            for (auto & nodePair : m_nodes)
            {
                if (!nodePair.second)
                {
                    continue;
                }
                for (auto & in : nodePair.second->parameter())
                {
                    if (in.second.getSource() && in.second.getSource()->portId == removedPortId)
                    {
                        in.second.getSource().reset();
                    }
                }
            }
        }
    }

    void Model::renameArgument(ParameterName const & oldName, ParameterName const & newName)
    {
        if (m_beginNode == nullptr || oldName == newName)
        {
            return;
        }

        if (m_beginNode->parameter().find(newName) != m_beginNode->parameter().end())
        {
            return;
        }

        auto & params = m_beginNode->parameter();
        if (auto it = params.find(oldName); it != params.end())
        {
            auto node = params.extract(it);
            node.key() = newName;
            params.insert(std::move(node));
        }

        auto & outputs = m_beginNode->getOutputs();
        if (auto it = outputs.find(oldName); it != outputs.end())
        {
            auto node = outputs.extract(it);
            node.key() = newName;
            node.mapped().setShortName(newName);
            node.mapped().setUniqueName(m_beginNode->getUniqueName() + "_" + newName);
            outputs.insert(std::move(node));
        }
    }

    void Model::reorderArgument(ParameterName source, ParameterName target)
    {
        if (m_beginNode == nullptr || source == target)
        {
            return;
        }

        auto & params = m_beginNode->parameter();
        auto & outputs = m_beginNode->getOutputs();

        auto srcParamIt = params.find(source);
        auto tgtParamIt = params.find(target);
        if (srcParamIt == params.end() || tgtParamIt == params.end())
        {
            return;
        }

        // Collect entries sorted by current sortIndex
        std::vector<ParameterName> ordered;
        ordered.reserve(params.size());
        for (auto & [name, _] : params)
        {
            ordered.push_back(name);
        }
        std::sort(ordered.begin(), ordered.end(), [&](auto const & a, auto const & b) {
            return params.at(a).getSortIndex() < params.at(b).getSortIndex();
        });

        // Remove source from ordered list and insert before target
        ordered.erase(std::remove(ordered.begin(), ordered.end(), source), ordered.end());
        auto targetIt = std::find(ordered.begin(), ordered.end(), target);
        ordered.insert(targetIt, source);

        // Reassign sequential sort indices
        for (int i = 0; i < static_cast<int>(ordered.size()); ++i)
        {
            params.at(ordered[i]).setSortIndex(i);
            auto outIt = outputs.find(ordered[i]);
            if (outIt != outputs.end())
            {
                outIt->second.setSortIndex(i);
            }
        }
    }

    void Model::removeFunctionOutput(ParameterName const & name)
    {
        if (m_endNode == nullptr)
        {
            return;
        }

        auto & params = m_endNode->parameter();
        if (auto it = params.find(name); it != params.end())
        {
            m_numericWidgetLayoutModes.erase(it->second.getId());
            m_vectorDisplayModes.erase(it->second.getId());
            m_inputParameter.erase(it->second.getId());
            params.erase(it);
        }
    }

    void Model::setNumericWidgetLayoutMode(ParameterId parameterId, NumericWidgetLayoutMode layoutMode)
    {
        m_numericWidgetLayoutModes[parameterId] = layoutMode;
    }

    NumericWidgetLayoutMode Model::getNumericWidgetLayoutMode(ParameterId parameterId) const
    {
        auto const iter = m_numericWidgetLayoutModes.find(parameterId);
        if (iter == m_numericWidgetLayoutModes.end())
        {
            return NumericWidgetLayoutMode::DialPlusDragFloat;
        }

        return iter->second;
    }

    bool Model::hasNumericWidgetLayoutMode(ParameterId parameterId) const
    {
        return m_numericWidgetLayoutModes.find(parameterId) != m_numericWidgetLayoutModes.end();
    }

    void Model::setVectorDisplayMode(ParameterId parameterId, VectorDisplayMode mode)
    {
        m_vectorDisplayModes[parameterId] = mode;
    }

    VectorDisplayMode Model::getVectorDisplayMode(ParameterId parameterId) const
    {
        auto const iter = m_vectorDisplayModes.find(parameterId);
        if (iter == m_vectorDisplayModes.end())
        {
            return VectorDisplayMode::Vector;
        }
        return iter->second;
    }

    void Model::renameFunctionOutput(ParameterName const & oldName, ParameterName const & newName)
    {
        if (m_endNode == nullptr || oldName == newName)
        {
            return;
        }

        if (m_endNode->parameter().find(newName) != m_endNode->parameter().end())
        {
            return;
        }

        auto & params = m_endNode->parameter();
        if (auto it = params.find(oldName); it != params.end())
        {
            auto node = params.extract(it);
            node.key() = newName;
            params.insert(std::move(node));
        }
    }

    PortRegistry & Model::getPortRegistry()
    {
        return m_outPorts;
    }

    std::unordered_set<int64_t> Model::collectCompatibleLinkCandidates(int64_t sourceEndpointId,
                                                                       bool sourceIsOutput)
    {
        std::unordered_set<int64_t> candidates;

        if (sourceIsOutput)
        {
            auto const sourcePortIter = m_outPorts.find(static_cast<PortId>(sourceEndpointId));
            if (sourcePortIter == m_outPorts.end() || sourcePortIter->second == nullptr)
            {
                return candidates;
            }

            for (auto const & [parameterId, parameter] : m_inputParameter)
            {
                auto * targetParameter = dynamic_cast<VariantParameter *>(parameter);
                if (targetParameter == nullptr)
                {
                    continue;
                }

                if (targetParameter->getParentId() == sourcePortIter->second->getParentId())
                {
                    continue;
                }

                if (targetParameter->getTypeIndex() != sourcePortIter->second->getTypeIndex())
                {
                    continue;
                }

                if (isLinkValid(sourcePortIter->second->getId(), targetParameter->getId()))
                {
                    candidates.insert(parameterId);
                }
            }

            return candidates;
        }

        auto const sourceParameterIter = m_inputParameter.find(static_cast<ParameterId>(sourceEndpointId));
        if (sourceParameterIter == m_inputParameter.end())
        {
            return candidates;
        }

        auto * sourceParameter = dynamic_cast<VariantParameter *>(sourceParameterIter->second);
        if (sourceParameter == nullptr)
        {
            return candidates;
        }

        for (auto const & [portId, port] : m_outPorts)
        {
            if (port == nullptr)
            {
                continue;
            }

            if (port->getParentId() == sourceParameter->getParentId())
            {
                continue;
            }

            if (port->getTypeIndex() != sourceParameter->getTypeIndex())
            {
                continue;
            }

            if (isLinkValid(port->getId(), sourceParameter->getId()))
            {
                candidates.insert(portId);
            }
        }

        return candidates;
    }

    const graph::AdjacencyListDirectedGraph & Model::getGraph() const
    {
        return m_graph;
    }

    graph::VertexList const & Model::getOutputOrder() const
    {
        return m_outputOrder;
    }

    InputParameterRegistry & Model::getParameterRegistry()
    {
        return m_inputParameter;
    }

    InputParameterRegistry const & Model::getConstParameterRegistry() const
    {
        return m_inputParameter;
    }

    PortRegistry const & Model::getConstPortRegistry() const
    {
        return m_outPorts;
    }

    std::optional<NodeBase *> Model::getNode(NodeId id) const
    {
        const auto node = m_nodes.find(id);
        if (node != std::end(m_nodes))
        {
            return {node->second.get()};
        }
        return {};
    }

    void Model::updateGraphAndOrderIfNeeded()
    {
        try
        {
            if (m_graphRequiresUpdate)
            {
                ZoneScopedN("RebuildGraph");
                buildGraph();
                m_outputOrder = topologicalSort(m_graph);
                m_graphRequiresUpdate = false;
                updateOrder();
            }
        }
        catch (const std::exception & e)
        {
            if (m_logger)
            {
                m_logger->addEvent({e.what(), events::Severity::Error});
            }
            std::cerr << e.what() << "\n";
        }
    }

    void Model::updateOrder()
    {
        NodeId order{};
        updateGraphAndOrderIfNeeded();

        for (const auto id : m_outputOrder)
        {
            auto node = m_nodes.find(id);
            if (node != std::end(m_nodes))
            {
                node->second->setOrder(++order);
            }
        }
    }

    void Model::visitNodes(Visitor & visitor)
    {
        updateGraphAndOrderIfNeeded();
        visitor.setModel(this);
        for (const auto id : m_outputOrder)
        {
            auto node = m_nodes.find(id);
            if (node != std::end(m_nodes))
            {
                try
                {
                    node->second->accept(visitor);
                }
                catch (const std::exception & e)
                {
                    std::cerr << e.what() << '\n';
                    throw e;
                }
            }
        }
    }

    bool Model::removeLink(const PortId startId, const ParameterId endId)
    {
        const auto sourcePort = m_outPorts.find(startId);
        if (sourcePort == std::end(m_outPorts))
        {
            return false;
        }
        const auto targetParameter = m_inputParameter.find(endId);
        if (targetParameter == std::end(m_inputParameter))
        {
            return false;
        }

        targetParameter->second->getSource().reset();
        m_graphRequiresUpdate = true;
        m_typesRequireUpdate = true;
        return true;
    }

    bool Model::isLinkValid(Port & source, VariantParameter & target)
    {
        if (source.getParentId() == target.getParentId())
        {
            if (m_logger)
            {
                m_logger->addEvent({"Cannot link parameter to itself", events::Severity::Warning});
            }
            return false;
        }

        updateGraphAndOrderIfNeeded();
        auto const isSourceSuccessor =
          isDependingOn(getGraph(), source.getParentId(), target.getParentId());

        return !isSourceSuccessor;
    }

    bool Model::isLinkValid(PortId const sourceId, ParameterId const targetId)
    {
        const auto sourcePort = m_outPorts.find(sourceId);
        if (sourcePort == std::end(m_outPorts))
        {
            return false;
        }
        const auto targetParameter = m_inputParameter.find(targetId);
        if (targetParameter == std::end(m_inputParameter))
        {
            return false;
        }

        auto const variantTarget = dynamic_cast<VariantParameter *>(targetParameter->second);

        if ((variantTarget != nullptr) && (!isLinkValid(*sourcePort->second, *variantTarget)))
        {
            return false;
        }

        return true;
    }

    bool Model::addLink(const PortId startId, const ParameterId endId, bool skipCheck)
    {
        const auto sourcePort = m_outPorts.find(startId);

        if (sourcePort == std::end(m_outPorts))
        {
            if (m_logger)
            {
                m_logger->addEvent(
                  {fmt::format("Soure port {} not found", startId), events::Severity::Error});
            }
            return false;
        }
        const auto targetParameter = m_inputParameter.find(endId);
        if (targetParameter == std::end(m_inputParameter))
        {
            if (m_logger)
            {
                m_logger->addEvent({fmt::format("Target parameter with id {} not found", endId),
                                    events::Severity::Error});
            }
            return false;
        }

        auto const variantTarget = dynamic_cast<VariantParameter *>(targetParameter->second);

        if ((variantTarget == nullptr))
        {
            return false;
        }

        if (!skipCheck)
        {
            if (!isLinkValid(*sourcePort->second, *variantTarget))
            {
                return false;
            }
        }

        targetParameter->second->setInputFromPort(*sourcePort->second);
        invalidateGraph();

        if (!skipCheck)
        {
            return updateTypes();
        }

        return true;
    }

    void Model::remove(NodeId id)
    {
        const auto nodeToRemove = m_nodes.find(id);

        // First check if the node exists
        if (nodeToRemove == std::end(m_nodes))
        {
            return;
        }

        // Protect begin node from deletion
        if (m_beginNode && nodeToRemove->second->getId() == m_beginNode->getId())
        {
            return;
        }

        // Protect end node from deletion
        if (m_endNode && nodeToRemove->second->getId() == m_endNode->getId())
        {
            return;
        }

        if (nodeToRemove != std::end(m_nodes))
        {
            updateGraphAndOrderIfNeeded();
            const auto successor = determineSuccessor(m_graph, id);
            for (auto consumerId : successor)
            {
                auto consumerNode = m_nodes.find(consumerId);

                if (consumerNode != std::end(m_nodes))
                {

                    for (auto & in : consumerNode->second->parameter())
                    {
                        if (in.second.getSource())
                        {
                            const auto srcPort = m_outPorts[in.second.getSource().value().portId];
                            if ((srcPort != nullptr) && srcPort->getParentId() == id)
                            {
                                in.second.getSource().reset();
                            }
                        }
                    }
                }
            }

            for (auto & [name, param] : nodeToRemove->second->parameter())
            {
                auto paramIter = m_inputParameter.find(param.getId());
                m_numericWidgetLayoutModes.erase(param.getId());
                m_vectorDisplayModes.erase(param.getId());
                if (param.getParentId() != nodeToRemove->second->getId())
                {
                    // Log warning instead of throwing - this can happen in edge cases
                    if (m_logger)
                    {
                        m_logger->addEvent(
                          {fmt::format("Parameter {} has incorrect parent ID {} instead of {}",
                                       name,
                                       param.getParentId(),
                                       nodeToRemove->second->getId()),
                           events::Severity::Warning});
                    }
                    continue; // Skip this parameter instead of throwing
                }
                if (paramIter != std::end(m_inputParameter))
                {
                    m_inputParameter.erase(paramIter);
                }
            }

            m_nodes.erase(nodeToRemove);
        }
        m_graphRequiresUpdate = true;
        m_typesRequireUpdate = true;
        // Graph rebuild deferred — callers (updateInputsAndOutputs, Validator)
        // will trigger it via updateGraphAndOrderIfNeeded() when they need it.
    }

    void Model::removeNodeWithoutLinks(NodeId idOfNodeWithoutLinks)
    {
        const auto nodeToRemove = m_nodes.find(idOfNodeWithoutLinks);
        if (nodeToRemove != std::end(m_nodes))
        {
            m_nodes.erase(nodeToRemove);
        }
        m_graphRequiresUpdate = true;
        m_typesRequireUpdate = true;
    }

    void Model::createBeginEnd()
    {
        m_beginNode = create<Begin>();
        m_beginNode->setDisplayName("inputs");
        m_beginNode->screenPos() =
          nodes::float2(0.0f, 0.0f); ///< Set initial screen position for Begin node
        m_endNode = create<End>();
        m_endNode->setDisplayName("outputs");
        m_endNode->screenPos() =
          nodes::float2(400.0f, 0.0f); ///< Set initial screen position for End node
    }

    void Model::createBeginEndWithDefaultInAndOuts()
    {
        createBeginEnd();
        // Use addArgument instead of directly calling addOutputPort so that
        // the Begin node's parameter map is also populated. This ensures
        // getArguments() returns the correct entries (used by MCP snippet tools).
        addArgument(FieldNames::Pos, VariantParameter(float3{0.0f, 0.0f, 0.0f}));
        m_endNode->parameter()[FieldNames::Shape] = VariantParameter(float{-1.f});
        m_endNode->parameter()[FieldNames::Color] = VariantParameter(float3{0.5f, 0.5f, 0.5f});

        registerInputs(*m_endNode);
        m_beginNode->updateNodeIds();
        m_endNode->updateNodeIds();
    }

    void Model::createValidVoid()
    {
        createBeginEndWithDefaultInAndOuts();
        auto constNode = create<ConstantScalar>();
        constNode->parameter()[FieldNames::Value] = VariantParameter(float{FLT_MAX});
        addLink(constNode->getOutputs().begin()->second.getId(),
                m_endNode->parameter().at(FieldNames::Shape).getId());
    }

    graph::AdjacencyListDirectedGraph & Model::buildGraph()
    {
        ZoneScopedN("BuildGraph");
        m_allInputReferencesAreValid = false;
        m_graph = graph::AdjacencyListDirectedGraph(m_lastId);

        // Add all nodes as vertices to ensure they are included in topological sort
        // even if they have no connections
        for (auto const & [id, node] : m_nodes)
        {
            m_graph.addVertex(id);
        }

        for (auto & [id, node] : m_nodes)
        {
            for (auto & parameter : node->parameter())
            {
                if (parameter.second.getSource().has_value())
                {
                    auto srcIter = m_outPorts.find(parameter.second.getSource().value().portId);
                    if (srcIter != std::end(m_outPorts))
                    {
                        const auto srcId = srcIter->second->getParentId();
                        m_graph.addDependency(id, srcId);
                    }
                    else
                    {
                        std::cerr << "could not find "
                                  << parameter.second.getSource().value().uniqueName << " ("
                                  << parameter.second.getSource().value().portId
                                  << ")in m_output_ports\n";
                        m_graph = graph::AdjacencyListDirectedGraph(m_lastId);
                        return m_graph;
                    }
                }
            }
        }
        m_allInputReferencesAreValid = true;
        return m_graph;
    }

    void Model::registerOutputs(NodeBase & node)
    {
        for (auto & outPort : node.getOutputs())
        {
            registerOutput(outPort.second);
        }
    }

    void Model::registerInputs(NodeBase & node)
    {
        for (auto & inputPort : node.parameter())
        {
            registerInput(inputPort.second);
        }
    }

    size_t Model::getSize() const
    {
        return m_nodes.size();
    }

    bool Model::updateTypes()
    {
        if (!m_typesRequireUpdate)
        {
            return m_isValid;
        }

        bool isValid = true;

        for (auto & [id, node] : m_nodes)
        {
            bool nodeIsValid = node->updateTypes(*this);
            if (!nodeIsValid)
            {
                if (m_logger)
                {
                    m_logger->addEvent(
                      events::Event{"For node " + node->getDisplayName() +
                                      " no matching combination of the requested inputs or "
                                      "output (types) could be found \n",
                                    events::Severity::Error});
                }
            }
            isValid = isValid && nodeIsValid;
        }
        m_typesRequireUpdate = false;
        return isValid;
    }

    bool Model::isValid()
    {
        return m_isValid;
    }

    void Model::updateValidityState()
    {
        // Reset to true initially, then accumulate validation results
        m_isValid = true;

        if (!m_allInputReferencesAreValid)
        {
            if (m_logger)
            {
                m_logger->addEvent(
                  events::Event{"Not all input references are valid", events::Severity::Error});
            }
            m_isValid = false;
            return;
        }

        if (graph::isCyclic(m_graph))
        {
            if (m_logger)
            {
                m_logger->addEvent(events::Event{"Graph is cyclic", events::Severity::Error});
            }
            m_isValid = false;
            return;
        }

        m_isValid = updateTypes();
    }

    void Model::setDisplayName(std::string const & name)
    {
        m_displayName = name;
    }

    std::optional<std::string> Model::getDisplayName() const
    {
        return m_displayName;
    }

    void Model::setLogger(events::SharedLogger logger)
    {
        m_logger = logger;
    }

    void Model::setResourceId(ResourceId resourceId)
    {
        m_resourceId = resourceId;
        m_name = fmt::format("function_{}", m_resourceId);
    }

    ResourceId Model::getResourceId() const
    {
        return m_resourceId;
    }

    Ports & Model::getInputs()
    {
        if (m_beginNode == nullptr)
        {
            createBeginEnd();
        }
        return m_beginNode->getOutputs();
    }

    ParameterMap & Model::getOutputs()
    {
        if (m_endNode == nullptr)
        {
            createBeginEnd();
        }
        return m_endNode->parameter();
    }

    void Model::setManaged(bool managed)
    {
        m_isManaged = managed;
    }

    bool Model::isManaged() const
    {
        return m_isManaged;
    }

    std::string Model::getSourceName(PortId portId)
    {
        auto portIter = m_outPorts.find(portId);
        if (portIter == std::end(m_outPorts))
        {
            return {};
        }

        const auto sourceNode = getNode(portIter->second->getParentId());
        if (!sourceNode.has_value())
        {
            return {};
        }

        auto sourceName =
          (sourceNode.value() == getBeginNode()) ? "inputs" : sourceNode.value()->getUniqueName();
        sourceName += ".";
        sourceName += portIter->second->getShortName();
        return sourceName;
    }

    Port * Model::getPort(PortId portId)
    {
        auto portIter = m_outPorts.find(portId);
        if (portIter == std::end(m_outPorts))
        {
            return nullptr;
        }
        return portIter->second;
    }

    void Model::invalidateGraph()
    {
        m_graphRequiresUpdate = true;
        m_typesRequireUpdate = true;
    }

    void Model::markAsLayouted()
    {
        m_nodesHaveBeenLayouted = true;
    }

    bool Model::hasBeenLayouted() const
    {
        return m_nodesHaveBeenLayouted;
    }

    bool Model::needsAutoLayout() const
    {
        if (m_nodesHaveBeenLayouted)
        {
            return false;
        }

        // Fewer than 3 nodes means only Begin/End — nothing to layout.
        if (m_nodes.size() < 3)
        {
            return false;
        }

        // Check whether all non-Begin/End nodes share the same position.
        // This catches the typical case where nodes are created at (0,0).
        std::optional<float2> referencePos;
        for (auto const & [id, node] : m_nodes)
        {
            if (!node || node.get() == m_beginNode || node.get() == m_endNode)
            {
                continue;
            }
            auto const & pos = node->screenPos();
            if (!referencePos)
            {
                referencePos = pos;
                continue;
            }
            // Exact equality is intentional: node positions are either the literal
            // default (0, 0) assigned at construction or values explicitly set by the
            // node editor — no arithmetic is ever performed on them, so no rounding
            // drift can occur.
            if (pos.x != referencePos->x || pos.y != referencePos->y)
            {
                return false;
            }
        }
        return referencePos.has_value();
    }

    void Model::setIsValid(bool isValid)
    {
        m_isValid = isValid;
    }

    /**
     * @brief Simplifies the model by removing nodes that are not connected to the end node.
     *
     * This method identifies all nodes that cannot influence the end node (i.e.,
     * there is no path from these nodes to the end node) and removes them.
     * This helps optimize the model by eliminating unused nodes.
     *
     * @return The number of nodes removed during simplification
     */
    size_t Model::simplifyModel()
    {
        // Make sure the graph is up-to-date
        updateGraphAndOrderIfNeeded();

        // If there's no end node, there's nothing to simplify
        if (!m_endNode)
        {
            return 0;
        }

        // Get the node ID of the end node
        NodeId const endNodeId = m_endNode->getId();

        // Create a set of all nodes that are needed (directly or indirectly contribute to the end
        // node)
        std::set<NodeId> neededNodes;

        // First, include the end node itself
        neededNodes.insert(endNodeId);

        // Then add all nodes that the end node depends on
        // We need to carefully handle the dependencies
        if (m_graph.getSize() > 0 && endNodeId < static_cast<NodeId>(m_graph.getSize()))
        {
            auto endNodeDependencies = graph::determineAllDependencies(m_graph, endNodeId);
            neededNodes.insert(endNodeDependencies.begin(), endNodeDependencies.end());
        }

        // Always include the Begin node if it exists
        if (m_beginNode)
        {
            neededNodes.insert(m_beginNode->getId());
        }

        // Find nodes to remove (those not in the needed set)
        std::vector<NodeId> nodesToRemove;
        for (const auto & [nodeId, node] : m_nodes)
        {
            // Skip begin and end nodes (redundant check, but for clarity)
            if (nodeId == endNodeId || (m_beginNode && nodeId == m_beginNode->getId()))
            {
                continue;
            }

            if (neededNodes.find(nodeId) == neededNodes.end())
            {
                nodesToRemove.push_back(nodeId);
            }
        }

        // Remove the unneeded nodes
        size_t removedCount = nodesToRemove.size();
        for (auto nodeId : nodesToRemove)
        {
            remove(nodeId);
        }

        // Update the graph after removing nodes
        m_graphRequiresUpdate = true;
        m_typesRequireUpdate = true;
        updateGraphAndOrderIfNeeded();

        return removedCount;

        return removedCount;
    }

    void Model::clear()
    {
        // Clear all nodes and registries
        m_nodes.clear();
        m_outPorts.clear();
        m_inputParameter.clear();

        // Reset node pointers
        m_beginNode = nullptr;
        m_endNode = nullptr;

        // Reset IDs
        m_lastParameterId = 0;
        m_lastId = 1;

        // Reset graph
        m_graph = graph::AdjacencyListDirectedGraph(0);
        m_outputOrder.clear();
        m_graphRequiresUpdate = true;
        m_typesRequireUpdate = true;

        // Reset state flags
        m_allInputReferencesAreValid = false;
        m_nodesHaveBeenLayouted = false;
        m_isValid = true;
        m_numericWidgetLayoutModes.clear();
    }

    FunctionCall * Model::createFunctionCallNode(ResourceId functionId, Model & sourceModel)
    {
        auto * node = create<FunctionCall>();
        node->setFunctionId(functionId);
        node->updateInputsAndOutputs(sourceModel);
        registerInputs(*node);
        registerOutputs(*node);

        if (sourceModel.getDisplayName().has_value())
        {
            node->setDisplayName(sourceModel.getDisplayName().value());
        }

        return node;
    }

} // namespace gladius::nodes

