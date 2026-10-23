#include "EvaluatorChannel.h"

#include <typeindex>

namespace gladius::nodes
{
    void ChannelDependencySelector::setChannel(EvaluatorChannel const channel)
    {
        m_channel = channel;
        m_collected = false;
        m_dependencies.clear();
    }

    EvaluatorChannel ChannelDependencySelector::getChannel() const noexcept
    {
        return m_channel;
    }

    void ChannelDependencySelector::setModel(Model * const model)
    {
        m_model = model;
        m_collected = false;
        m_dependencies.clear();
    }

    void ChannelDependencySelector::setEmittedChannels(std::vector<std::string> channelNames)
    {
        m_emittedChannels = std::move(channelNames);
        m_collected = false;
        m_dependencies.clear();
    }

    void ChannelDependencySelector::collect() const
    {
        m_collected = true;
        m_dependencies.clear();

        if (m_model == nullptr)
        {
            return;
        }

        auto const * const endNode = m_model->getEndNode();
        if (endNode == nullptr)
        {
            return;
        }

        auto const channelNames = m_emittedChannels.empty()
              ? std::vector<std::string>{std::string{FieldNames::Shape}}
              : m_emittedChannels;

        std::vector<IParameter const *> channelParameters;
        for (auto const & channelName : channelNames)
        {
            auto const channelIt = endNode->constParameter().find(channelName);
            if (channelIt != endNode->constParameter().end())
            {
                channelParameters.push_back(&channelIt->second);
            }
        }
        if (channelParameters.empty())
        {
            return;
        }

        // The position argument is always required, even for a constant channel value.
        if (auto * const beginNode = m_model->getBeginNode(); beginNode != nullptr)
        {
            m_dependencies.insert(beginNode->getId());
        }

        std::vector<NodeId> pending{endNode->getId()};
        m_dependencies.insert(endNode->getId());
        while (!pending.empty())
        {
            auto const nodeId = pending.back();
            pending.pop_back();

            auto const nodeOpt = m_model->getNode(nodeId);
            if (!nodeOpt.has_value())
            {
                continue;
            }
            // The End node only contributes the selected channels - the remaining channels are
            // emitted by their own evaluator entry point.
            if (nodeId == endNode->getId())
            {
                for (auto const * const channelParameter : channelParameters)
                {
                    followSource(*channelParameter, nodeId, pending);
                }
                continue;
            }
            for (auto const & [portName, parameter] : nodeOpt.value()->parameter())
            {
                followSource(parameter, nodeId, pending);
            }
        }
    }

    void ChannelDependencySelector::followSource(IParameter const & parameter,
                                                 NodeId const consumerNodeId,
                                                 std::vector<NodeId> & pending) const
    {
        auto const & source = parameter.getConstSource();
        if (!source.has_value() || source->nodeId == consumerNodeId)
        {
            return;
        }
        if (m_dependencies.insert(source->nodeId).second)
        {
            pending.push_back(source->nodeId);
        }
    }

    bool ChannelDependencySelector::isNeededByChannel(NodeId const nodeId) const
    {
        if (m_channel == EvaluatorChannel::ShapeAndColor)
        {
            return true;
        }
        if (!m_collected)
        {
            collect();
        }
        return m_dependencies.contains(nodeId);
    }

    bool ChannelDependencySelector::providesSelectedChannel() const
    {
        if (m_channel == EvaluatorChannel::ShapeAndColor)
        {
            return true;
        }
        if (m_model == nullptr)
        {
            return false;
        }
        auto const * const endNode = m_model->getEndNode();
        if (endNode == nullptr)
        {
            return false;
        }
        auto const & params = endNode->constParameter();
        auto const it = params.find(std::string{FieldNames::Translucency});
        return it != params.end() && it->second.getTypeIndex() == ParameterTypeIndex::Float;
    }
}
