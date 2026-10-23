#include "ModelUtils.h"

#include <algorithm>
#include <vector>

namespace gladius::nodes
{
    bool isQualifiedForLevelset(Model & model)
    {
        // Check for Begin node with float3 pos parameter
        const Begin * beginNode = model.getBeginNode();
        if (!beginNode)
        {
            return false;
        }

        // Check if Begin node has a "pos" output port of type float3
        const auto & outputs = beginNode->getOutputs();
        auto posPortIt = outputs.find(FieldNames::Pos);
        if (posPortIt == outputs.end() ||
            posPortIt->second.getTypeIndex() != ParameterTypeIndex::Float3)
        {
            return false;
        }

        // Check if the "pos" port is the only output port
        if (outputs.size() != 1)
        {
            return false;
        }

        // Check for End node with at least one float parameter
        End * endNode = model.getEndNode();
        if (!endNode)
        {
            return false;
        }

        // Check if End node has at least one float parameter
        const auto & params = endNode->parameter();
        bool hasScalarParam = false;

        for (const auto & [name, param] : params)
        {
            if (param.getTypeIndex() == ParameterTypeIndex::Float)
            {
                hasScalarParam = true;
                break;
            }
        }

        return hasScalarParam;
    }

    bool isQualifiedForVolumeColor(Model & model)
    {
        // Check for Begin node with float3 pos parameter
        const Begin * beginNode = model.getBeginNode();
        if (!beginNode)
        {
            return false;
        }

        // Check if Begin node has a "pos" input port of type float3
        const auto & inputs = beginNode->getOutputs();
        auto posPortIt = inputs.find(FieldNames::Pos);
        if (posPortIt == inputs.end() ||
            posPortIt->second.getTypeIndex() != ParameterTypeIndex::Float3)
        {
            return false;
        }

        // Check for End node with at least one float3 output parameter
        End * endNode = model.getEndNode();
        if (!endNode)
        {
            return false;
        }

        // Check if End node has at least one float3 output parameter
        const auto & params = endNode->parameter();
        bool hasScalarParam = false;

        for (const auto & [name, param] : params)
        {
            if (param.getTypeIndex() == ParameterTypeIndex::Float3)
            {
                hasScalarParam = true;
                break;
            }
        }

        return hasScalarParam;
    }

    bool hasScalarOutputChannel(Model & model, std::string_view channelName)
    {
        End * endNode = model.getEndNode();
        if (endNode == nullptr)
        {
            return false;
        }
        auto const & params = endNode->parameter();
        auto const it = params.find(std::string{channelName});
        return it != params.end() && it->second.getTypeIndex() == ParameterTypeIndex::Float;
    }

    bool isQualifiedForTranslucency(Model & model)
    {
        // Translucency is evaluated at the same positions as the levelset, so require
        // a Begin node exposing exactly one float3 "pos" output.
        Begin * beginNode = model.getBeginNode();
        if (beginNode == nullptr)
        {
            return false;
        }
        auto const & outputs = beginNode->getOutputs();
        auto const posPortIt = outputs.find(FieldNames::Pos);
        if (posPortIt == outputs.end() ||
            posPortIt->second.getTypeIndex() != ParameterTypeIndex::Float3)
        {
            return false;
        }
        return hasScalarOutputChannel(model, FieldNames::Translucency);
    }

    bool isQualifiedForScalarPropertyChannel(Model & model)
    {
        Begin * beginNode = model.getBeginNode();
        if (beginNode == nullptr)
        {
            return false;
        }
        auto const & outputs = beginNode->getOutputs();
        auto const posPortIt = outputs.find(FieldNames::Pos);
        if (posPortIt == outputs.end() ||
            posPortIt->second.getTypeIndex() != ParameterTypeIndex::Float3)
        {
            return false;
        }
        return !collectScalarOutputChannelNames(model).empty();
    }

    std::vector<std::string> collectScalarOutputChannelNames(Model & model)
    {
        End * endNode = model.getEndNode();
        if (endNode == nullptr)
        {
            return {};
        }
        std::vector<std::string> channelNames;
        for (auto const & [name, param] : endNode->parameter())
        {
            if (param.getTypeIndex() == ParameterTypeIndex::Float)
            {
                channelNames.push_back(name);
            }
        }
        std::ranges::sort(channelNames);
        return channelNames;
    }
} // namespace gladius::nodes