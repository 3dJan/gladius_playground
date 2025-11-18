#pragma once

#include "nodes/Model.h"
#include <imgui.h>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gladius::ui
{
    /**
     * @brief Evaluates geometric metrics for a laid out node graph.
     *
     * The analyzer inspects node positions as stored on the model and derives
     * a set of metrics that are useful for benchmarking layout algorithms in
     * unit tests.
     */
    class LayoutQualityAnalyzer
    {
      public:
        /**
         * @brief Aggregate metrics for a specific group.
         */
        struct GroupMetrics
        {
            float width{0.0f};
            float height{0.0f};
            float occupiedArea{0.0f};
            float sumEdgeLength{0.0f};
            float maxEdgeLength{0.0f};
            std::size_t edgeCount{0};
            std::size_t edgeCrossings{0};
            std::size_t nodeCount{0};
        };

        /**
         * @brief Pair of overlapping node identifiers.
         */
        struct NodeOverlap
        {
            nodes::NodeId first{};
            nodes::NodeId second{};
        };

        /**
         * @brief Pair of overlapping group tags.
         */
        struct GroupOverlap
        {
            std::string first;
            std::string second;
        };

        /**
         * @brief Final metric bundle for the whole graph.
         */
        struct Metrics
        {
            float width{0.0f};
            float height{0.0f};
            float occupiedArea{0.0f};
            float sumEdgeLength{0.0f};
            float maxEdgeLength{0.0f};
            std::size_t edgeCount{0};
            std::size_t edgeCrossings{0};
            std::vector<NodeOverlap> nodeOverlaps{};
            std::vector<GroupOverlap> groupOverlaps{};
            std::unordered_map<std::string, GroupMetrics> groupMetrics{};
        };

        using NodeSizeProvider = std::function<ImVec2(nodes::NodeId)>;

        explicit LayoutQualityAnalyzer(NodeSizeProvider sizeProvider = {});
        Metrics analyze(nodes::Model & model) const;

      private:
        NodeSizeProvider m_sizeProvider;

        static ImVec2 defaultNodeSizeProvider(nodes::NodeId nodeId);

        struct NodeInfo
        {
            ImVec2 min{};
            ImVec2 max{};
            ImVec2 center{};
            std::string tag;
        };

        struct EdgeSegment
        {
            nodes::NodeId source{};
            nodes::NodeId target{};
            ImVec2 from{};
            ImVec2 to{};
        };

        static bool rectanglesOverlap(ImVec2 const & minA,
                                      ImVec2 const & maxA,
                                      ImVec2 const & minB,
                                      ImVec2 const & maxB);
        static bool segmentsCross(const EdgeSegment & first, const EdgeSegment & second);
        static float segmentLength(const EdgeSegment & segment);
    };
} // namespace gladius::ui
