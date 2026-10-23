#pragma once

#include "Model.h"
#include "Parameter.h"
#include "nodesfwd.h"

namespace gladius::nodes
{
    /**
     * @brief Checks if a Model satisfies the criteria to be used as a levelset
     *
     * A Model is qualified for levelset if:
     * 1. It has a Begin node with only a float3 input parameter named "pos"
     * 2. It has an End node with at least one scalar (float) parameter
     *
     * @param model The Model to check
     * @return true if the Model qualifies as a levelset, false otherwise
     */
    bool isQualifiedForLevelset(Model & model);

    /**
     * @brief Checks if a Model satisfies the criteria to be used as a volume color function
     *
     * A Model is qualified for volume color if:
     * 1. It has a Begin node with a float3 input parameter named "pos"
     * 2. It has an End node with at least one float3 output parameter
     *
     * @param model The Model to check
     * @return true if the Model qualifies as a volume color function, false otherwise
     */
    bool isQualifiedForVolumeColor(Model & model);

    /**
     * @brief Checks if a Model satisfies the criteria to provide a translucency channel
     *
     * A Model is qualified for translucency if:
     * 1. It has a Begin node with a float3 input parameter named "pos"
     * 2. It has an End node with a scalar (float) output parameter named "translucency"
     *
     * The scalar value is interpreted as the ISO/TS 19307 one-parameter translucency
     * alpha and must be clamped into [0, 1] by the consumer.
     *
     * @param model The Model to check
     * @return true if the Model provides a translucency channel, false otherwise
     */
    bool isQualifiedForTranslucency(Model & model);

    /**
     * @brief Checks if a Model exposes a scalar output parameter with the given name
     *
     * @param model The Model to check
     * @param channelName Name of the expected output parameter
     * @return true if the End node has a scalar parameter called channelName
     */
    [[nodiscard]] bool hasScalarOutputChannel(Model & model, std::string_view channelName);

    /**
     * @brief Checks if a Model can provide an arbitrary scalar volumetric property channel
     *
     * A Model is qualified for a scalar property if:
     * 1. It has a Begin node with a float3 input parameter named "pos"
     * 2. It has an End node with at least one scalar (float) parameter
     *
     * Unlike a levelset, the number of Begin outputs is unrestricted, and the scalar
     * output may carry any name - use collectScalarOutputChannelNames to list the options.
     *
     * @param model The Model to check
     * @return true if the Model exposes at least one selectable scalar channel
     */
    [[nodiscard]] bool isQualifiedForScalarPropertyChannel(Model & model);

    /**
     * @brief Collects the scalar output channels a Model can feed a volumetric property
     *
     * @param model The Model to inspect
     * @return Sorted names of the End node scalar (float) parameters, empty if none
     */
    [[nodiscard]] std::vector<std::string> collectScalarOutputChannelNames(Model & model);
} // namespace gladius::nodes