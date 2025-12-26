#include "LayoutQualityAnalyzer.h"

#include "imgui-node-editor/imgui_node_editor.h"
#include "nodes/graph/GraphAlgorithms.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ed = ax::NodeEditor;

namespace gladius::ui
{
    namespace
    {
        constexpr float DEFAULT_NODE_WIDTH = 500.0F;
        constexpr float DEFAULT_NODE_HEIGHT = 400.0F;
        constexpr float PARALLEL_EPSILON = 1e-3F;

        ImVec2 subtract(ImVec2 const & a, ImVec2 const & b)
        {
            return ImVec2(a.x - b.x, a.y - b.y);
        }

        float cross(ImVec2 const & a, ImVec2 const & b)
        {
            return a.x * b.y - a.y * b.x;
        }
    } // namespace

    LayoutQualityAnalyzer::LayoutQualityAnalyzer(NodeSizeProvider sizeProvider)
        : m_sizeProvider(std::move(sizeProvider))
    {
        if (!m_sizeProvider)
        {
            m_sizeProvider = [](nodes::NodeId nodeId) { return defaultNodeSizeProvider(nodeId); };
        }
    }

    ImVec2 LayoutQualityAnalyzer::defaultNodeSizeProvider(nodes::NodeId nodeId)
    {
        auto size = ed::GetNodeSize(nodeId);
        if (size.x <= 0.0F)
        {
            size.x = DEFAULT_NODE_WIDTH;
        }
        if (size.y <= 0.0F)
        {
            size.y = DEFAULT_NODE_HEIGHT;
        }
        return size;
    }

    LayoutQualityAnalyzer::Metrics LayoutQualityAnalyzer::analyze(nodes::Model & model) const
    {
        Metrics metrics;

        if (model.getSize() == 0)
        {
            return metrics;
        }

        auto const & graph = model.getGraph();

        std::unordered_map<nodes::NodeId, NodeInfo> nodeInfos;
        nodeInfos.reserve(model.getSize());

        struct GroupAccumulator
        {
            ImVec2 min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
            ImVec2 max{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
            std::vector<EdgeSegment> edges;
            float sumEdgeLength{0.0F};
            float maxEdgeLength{0.0F};
            std::size_t edgeCount{0U};
            std::size_t nodeCount{0U};
        };

        std::unordered_map<std::string, GroupAccumulator> groupAccumulators;

        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float maxY = std::numeric_limits<float>::lowest();

        for (auto & [id, nodePtr] : model)
        {
            if (!nodePtr)
            {
                continue;
            }

            auto * node = nodePtr.get();
            ImVec2 size = m_sizeProvider(node->getId());
            if (size.x <= 0.0F)
            {
                size.x = DEFAULT_NODE_WIDTH;
            }
            if (size.y <= 0.0F)
            {
                size.y = DEFAULT_NODE_HEIGHT;
            }

            auto const pos = node->screenPos();
            ImVec2 min{pos.x, pos.y};
            ImVec2 max{pos.x + size.x, pos.y + size.y};
            ImVec2 center{pos.x + (size.x * 0.5F), pos.y + (size.y * 0.5F)};

            NodeInfo info{};
            info.min = min;
            info.max = max;
            info.center = center;
            info.tag = node->getTag();

            nodeInfos.emplace(node->getId(), info);

            minX = std::min(minX, min.x);
            minY = std::min(minY, min.y);
            maxX = std::max(maxX, max.x);
            maxY = std::max(maxY, max.y);

            if (!info.tag.empty())
            {
                auto & accumulator = groupAccumulators[info.tag];
                accumulator.min.x = std::min(accumulator.min.x, min.x);
                accumulator.min.y = std::min(accumulator.min.y, min.y);
                accumulator.max.x = std::max(accumulator.max.x, max.x);
                accumulator.max.y = std::max(accumulator.max.y, max.y);
                ++accumulator.nodeCount;
            }
        }

        if (nodeInfos.empty())
        {
            return metrics;
        }

        metrics.width = maxX - minX;
        metrics.height = maxY - minY;
        metrics.occupiedArea = metrics.width * metrics.height;

        std::vector<EdgeSegment> edges;

        for (auto const & [nodeId, info] : nodeInfos)
        {
            auto successors = nodes::graph::determineSuccessor(graph, nodeId);
            for (auto successorId : successors)
            {
                auto successorIt = nodeInfos.find(successorId);
                if (successorIt == nodeInfos.end())
                {
                    continue;
                }

                EdgeSegment segment{};
                segment.source = nodeId;
                segment.target = successorId;
                segment.from = info.center;
                segment.to = successorIt->second.center;

                float const length = segmentLength(segment);
                metrics.sumEdgeLength += length;
                metrics.maxEdgeLength = std::max(metrics.maxEdgeLength, length);
                ++metrics.edgeCount;
                edges.push_back(segment);

                if (!info.tag.empty() && info.tag == successorIt->second.tag)
                {
                    auto groupIt = groupAccumulators.find(info.tag);
                    if (groupIt != groupAccumulators.end())
                    {
                        auto & accumulator = groupIt->second;
                        accumulator.edges.push_back(segment);
                        accumulator.sumEdgeLength += length;
                        accumulator.maxEdgeLength =
                          std::max(accumulator.maxEdgeLength, length);
                        ++accumulator.edgeCount;
                    }
                }
            }
        }

        metrics.edgeCrossings = 0U;
        for (std::size_t i = 0; i < edges.size(); ++i)
        {
            for (std::size_t j = i + 1; j < edges.size(); ++j)
            {
                if (segmentsCross(edges[i], edges[j]))
                {
                    ++metrics.edgeCrossings;
                }
            }
        }

        for (auto first = nodeInfos.begin(); first != nodeInfos.end(); ++first)
        {
            auto second = first;
            ++second;
            for (; second != nodeInfos.end(); ++second)
            {
                if (rectanglesOverlap(first->second.min,
                                      first->second.max,
                                      second->second.min,
                                      second->second.max))
                {
                    metrics.nodeOverlaps.push_back({first->first, second->first});
                }
            }
        }

        std::vector<std::pair<std::string, GroupAccumulator const *>> activeGroups;
        activeGroups.reserve(groupAccumulators.size());

        for (auto & [tag, accumulator] : groupAccumulators)
        {
            if (accumulator.nodeCount == 0)
            {
                continue;
            }

            GroupMetrics groupMetrics{};
            groupMetrics.nodeCount = accumulator.nodeCount;
            groupMetrics.width = accumulator.max.x - accumulator.min.x;
            groupMetrics.height = accumulator.max.y - accumulator.min.y;
            groupMetrics.occupiedArea = groupMetrics.width * groupMetrics.height;
            groupMetrics.sumEdgeLength = accumulator.sumEdgeLength;
            groupMetrics.maxEdgeLength = accumulator.maxEdgeLength;
            groupMetrics.edgeCount = accumulator.edgeCount;

            std::size_t crossings = 0U;
            for (std::size_t i = 0; i < accumulator.edges.size(); ++i)
            {
                for (std::size_t j = i + 1; j < accumulator.edges.size(); ++j)
                {
                    if (segmentsCross(accumulator.edges[i], accumulator.edges[j]))
                    {
                        ++crossings;
                    }
                }
            }
            groupMetrics.edgeCrossings = crossings;

            metrics.groupMetrics.emplace(tag, groupMetrics);
            activeGroups.emplace_back(tag, &accumulator);
        }

        for (std::size_t i = 0; i < activeGroups.size(); ++i)
        {
            for (std::size_t j = i + 1; j < activeGroups.size(); ++j)
            {
                auto const * first = activeGroups[i].second;
                auto const * second = activeGroups[j].second;
                if (rectanglesOverlap(first->min, first->max, second->min, second->max))
                {
                    GroupOverlap overlap{};
                    overlap.first = activeGroups[i].first;
                    overlap.second = activeGroups[j].first;
                    metrics.groupOverlaps.push_back(std::move(overlap));
                }
            }
        }

        return metrics;
    }

    bool LayoutQualityAnalyzer::rectanglesOverlap(ImVec2 const & minA,
                                                  ImVec2 const & maxA,
                                                  ImVec2 const & minB,
                                                  ImVec2 const & maxB)
    {
        return maxA.x > minB.x && minA.x < maxB.x && maxA.y > minB.y && minA.y < maxB.y;
    }

    bool LayoutQualityAnalyzer::segmentsCross(const EdgeSegment & first, const EdgeSegment & second)
    {
        if (first.source == second.source || first.source == second.target ||
            first.target == second.source || first.target == second.target)
        {
            return false;
        }

        auto const p = first.from;
        auto const q = second.from;
        auto const r = subtract(first.to, first.from);
        auto const s = subtract(second.to, second.from);

        float const denominator = cross(r, s);
        if (std::fabs(denominator) < PARALLEL_EPSILON)
        {
            return false;
        }

        auto const qp = subtract(q, p);
        float const t = cross(qp, s) / denominator;
        float const u = cross(qp, r) / denominator;

        return t > PARALLEL_EPSILON && t < 1.0F - PARALLEL_EPSILON && u > PARALLEL_EPSILON &&
               u < 1.0F - PARALLEL_EPSILON;
    }

    float LayoutQualityAnalyzer::segmentLength(const EdgeSegment & segment)
    {
        float const dx = segment.to.x - segment.from.x;
        float const dy = segment.to.y - segment.from.y;
        return std::sqrt((dx * dx) + (dy * dy));
    }
} // namespace gladius::ui
