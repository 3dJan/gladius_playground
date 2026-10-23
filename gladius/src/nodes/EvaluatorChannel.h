#pragma once

#include "Model.h"
#include "NodeBase.h"
#include "nodesfwd.h"

#include <set>
#include <string>
#include <vector>

namespace gladius::nodes
{
    /**
     * @brief Which End-node channel a generated evaluator entry point returns.
     *
     * The renderer ABI keeps the shape/color evaluator at vec4 = (rgb, sdf) because the kernels
     * call it in a hundred places. Additional End-node channels are therefore emitted as a
     * separate entry point from a second visitor pass over the same (flattened) model.
     */
    enum class EvaluatorChannel
    {
        ShapeAndColor, ///< model()/evaluateModel() - the existing shape + color entry point
        Translucency   ///< translucencyModel()/evaluateTranslucency() - scalar translucency alpha
    };

    /**
     * @brief Resolves which nodes feed a single End-node output channel.
     *
     * A second evaluator pass must not emit the whole graph again, so this helper collects the
     * transitive dependency set of one named End-node parameter.
     */
    class ChannelDependencySelector
    {
      public:
        void setChannel(EvaluatorChannel channel);
        [[nodiscard]] EvaluatorChannel getChannel() const noexcept;
        void setModel(Model * model);

        /// @brief Restrict emission to the nodes feeding the given End-node output channels.
        /// An empty list disables filtering, so every node of the model is emitted.
        void setEmittedChannels(std::vector<std::string> channelNames);

        /// @brief True when the node contributes to the selected channel (or the channel is the
        /// default shape/color one, in which case every node qualifies).
        [[nodiscard]] bool isNeededByChannel(NodeId nodeId) const;

        /// @brief True when the current model's End node exposes the selected channel as a scalar.
        [[nodiscard]] bool providesSelectedChannel() const;

      private:
        void collect() const;
        void followSource(IParameter const & parameter,
                          NodeId consumerNodeId,
                          std::vector<NodeId> & pending) const;

        EvaluatorChannel m_channel{EvaluatorChannel::ShapeAndColor};
        std::vector<std::string> m_emittedChannels;
        Model * m_model{};
        mutable bool m_collected{};
        mutable std::set<NodeId> m_dependencies;
    };
}
