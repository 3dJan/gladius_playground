#include "NodeLayoutEngine.h"
#include "imgui.h"
#include "nodes/graph/GraphAlgorithms.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <unordered_set>
#include <utility>

namespace gladius::ui
{
    void NodeLayoutEngine::performAutoLayout(nodes::Model & model, const LayoutConfig & config)
    {
        model.updateGraphAndOrderIfNeeded();

        auto * beginNode = model.getBeginNode();
        auto * endNode = model.getEndNode();

        if (!beginNode)
        {
            return;
        }

        // Special case: exactly two nodes (Begin and End)
        if (model.getSize() == 2 && beginNode && endNode)
        {
            beginNode->screenPos() = nodes::float2(0.0f, 0.0f);
            endNode->screenPos() = nodes::float2(400.0f, 0.0f);
            model.markAsLayouted();
            return;
        }

        auto const & graph = model.getGraph();

        if (graph.getSize() < 2)
        {
            return;
        }

        auto const basePositions = capturePositions(model);

        struct LayoutCandidate
        {
            LayoutQualityAnalyzer::Metrics metrics{};
            PositionSnapshot positions{};
            float score{std::numeric_limits<float>::infinity()};
            bool valid{false};
            std::string strategyName{};
        };

        LayoutCandidate bestCandidate;
        std::vector<PositionSnapshot> candidateSnapshots;

        auto makeAnalyzer = [this]()
        {
            if (m_nodeSizeProvider)
            {
                return LayoutQualityAnalyzer(m_nodeSizeProvider);
            }
            return LayoutQualityAnalyzer();
        };

        auto const * strategiesPtr = [&]() -> std::vector<LayoutStrategy> const *
        {
            if (m_useCustomStrategies && !m_customStrategies.empty())
            {
                return &m_customStrategies;
            }

            static const std::vector<LayoutStrategy> defaultStrategies = {
                // CHAMPION: 44.8% better area on benchmarks - with proper spacing to avoid overlaps
                LayoutStrategy{"BalancedGrid Compact",
                               GroupLayoutMode::BalancedGrid,
                               0.90F,  // nodeDistanceScale
                               0.95F,  // layerSpacingScale
                               140,    // maxOptimizationIterations
                               0.85F,  // convergenceScale
                               true,   // enableExtraRelaxation
                               2,      // extraRelaxationPasses
                               true,   // enableGroupCompaction
                               true},  // alignToOrigin
                // RUNNER-UP: Best Y-scatter - adjusted spacing
                LayoutStrategy{"WeightedMedian Compact",
                               GroupLayoutMode::VerticalStack,
                               0.90F,  // nodeDistanceScale
                               1.0F,   // layerSpacingScale
                               150,    // maxOptimizationIterations
                               0.75F,  // convergenceScale
                               true,   // enableExtraRelaxation
                               4,      // extraRelaxationPasses
                               true,   // enableGroupCompaction
                               true},  // alignToOrigin
                LayoutStrategy{"MedianSweep TightY",
                               GroupLayoutMode::VerticalStack,
                               0.85F,  // nodeDistanceScale
                               1.0F,   // layerSpacingScale
                               140,    // maxOptimizationIterations
                               0.8F,   // convergenceScale
                               true,   // enableExtraRelaxation
                               3,      // extraRelaxationPasses
                               true,   // enableGroupCompaction
                               true},  // alignToOrigin
                LayoutStrategy{"HybridLayered Tight",
                               GroupLayoutMode::VerticalStack,
                               0.85F,  // nodeDistanceScale
                               1.05F,  // layerSpacingScale
                               135,    // maxOptimizationIterations
                               0.82F,  // convergenceScale
                               true,   // enableExtraRelaxation
                               3,      // extraRelaxationPasses
                               true,   // enableGroupCompaction
                               true},  // alignToOrigin
                LayoutStrategy{"LayeredRow Sweep",
                               GroupLayoutMode::HorizontalRow,
                               0.90F,  // nodeDistanceScale
                               1.1F,   // layerSpacingScale
                               120,    // maxOptimizationIterations
                               0.9F,   // convergenceScale
                               true,   // enableExtraRelaxation
                               1,      // extraRelaxationPasses
                               true,   // enableGroupCompaction
                               true},  // alignToOrigin
                LayoutStrategy{"ForceRefined Hybrid",
                               GroupLayoutMode::VerticalStack,
                               0.85F,  // nodeDistanceScale
                               1.05F,  // layerSpacingScale
                               160,    // maxOptimizationIterations
                               0.7F,   // convergenceScale
                               true,   // enableExtraRelaxation
                               3,      // extraRelaxationPasses
                               true,   // enableGroupCompaction
                               true},  // alignToOrigin
                // BASELINE: Kept for comparison
                LayoutStrategy{"LayeredStack Classic"}};

            return &defaultStrategies;
        }();

        auto const & strategies = *strategiesPtr;

        m_lastResults.clear();
        m_lastChampion.reset();
        m_lastResults.reserve(strategies.size());

        candidateSnapshots.reserve(strategies.size());

        for (auto const & strategy : strategies)
        {
            StrategyResult result{};
            result.name = strategy.name;
            result.score = std::numeric_limits<float>::infinity();

            restorePositions(model, basePositions, false);

            LayoutConfig strategyConfig = config;
            strategyConfig.nodeDistance =
              std::max(10.0F, config.nodeDistance * strategy.nodeDistanceScale);
            strategyConfig.layerSpacing =
              std::max(50.0F, config.layerSpacing * strategy.layerSpacingScale);
            if (strategy.maxOptimizationIterations > 0)
            {
                strategyConfig.maxOptimizationIterations = strategy.maxOptimizationIterations;
            }
            strategyConfig.convergenceThreshold =
              std::max(0.1F, config.convergenceThreshold * strategy.convergenceScale);

            // Enable median-based ordering for MedianSweep and WeightedMedian strategies
            if (strategy.name.find("MedianSweep") != std::string::npos ||
                strategy.name.find("WeightedMedian") != std::string::npos ||
                strategy.name.find("HybridLayered") != std::string::npos)
            {
                strategyConfig.useMedianForOrdering = true;
            }

            if (!performAutoLayoutVariant(model, strategyConfig, strategy.groupMode))
            {
                // Skipping strategy due to failed layout pass; no console logging in production
                m_lastResults.push_back(result);
                candidateSnapshots.emplace_back(PositionSnapshot{});
                continue;
            }

            applyPostProcessing(model, strategyConfig, strategy);

            auto analyzer = makeAnalyzer();
            auto metrics = analyzer.analyze(model);
            float const score = computeLayoutScore(metrics);

            result.applied = true;
            result.metrics = metrics;
            result.score = score;

            m_lastResults.push_back(result);

            // Capture positions for this candidate so we can restore the chosen one later
            candidateSnapshots.push_back(capturePositions(model));



            // Selection of champion is performed after evaluating all candidates
        }

        // Choose best candidate with a hard preference for zero-overlap layouts
        int chosenIndex = -1;
        float chosenScore = std::numeric_limits<float>::infinity();

        // First pass: consider only candidates with no overlaps
        for (size_t i = 0; i < m_lastResults.size(); ++i)
        {
            auto const & r = m_lastResults[i];
            if (!r.applied)
            {
                continue;
            }
            bool const hasOverlap = !r.metrics.nodeOverlaps.empty() || !r.metrics.groupOverlaps.empty();
            if (hasOverlap)
            {
                continue;
            }
            if (r.score < chosenScore)
            {
                chosenScore = r.score;
                chosenIndex = static_cast<int>(i);
            }
        }

        // Second pass: if all candidates overlap, pick best overall
        if (chosenIndex < 0)
        {
            for (size_t i = 0; i < m_lastResults.size(); ++i)
            {
                auto const & r = m_lastResults[i];
                if (!r.applied)
                {
                    continue;
                }
                if (r.score < chosenScore)
                {
                    chosenScore = r.score;
                    chosenIndex = static_cast<int>(i);
                }
            }
        }

        if (chosenIndex < 0)
        {
            restorePositions(model, basePositions, true);
            model.markAsLayouted();
            return;
        }

        // Restore the positions for the chosen candidate
        restorePositions(model, candidateSnapshots[static_cast<size_t>(chosenIndex)], true);
        m_lastChampion = m_lastResults[static_cast<size_t>(chosenIndex)];
        model.markAsLayouted();
    }

    void NodeLayoutEngine::setNodeSizeProvider(NodeSizeProvider provider)
    {
        m_nodeSizeProvider = std::move(provider);
    }

    void NodeLayoutEngine::setNodePositionWriter(NodePositionWriter writer)
    {
        m_nodePositionWriter = std::move(writer);
    }

    void NodeLayoutEngine::setStrategies(std::vector<LayoutStrategy> strategies)
    {
        m_customStrategies = std::move(strategies);
        m_useCustomStrategies = true;
    }

    void NodeLayoutEngine::clearCustomStrategies()
    {
        m_customStrategies.clear();
        m_useCustomStrategies = false;
    }

    std::vector<NodeLayoutEngine::StrategyResult> const &
    NodeLayoutEngine::getLastResults() const
    {
        return m_lastResults;
    }

    std::optional<NodeLayoutEngine::StrategyResult> const &
    NodeLayoutEngine::getLastChampion() const
    {
        return m_lastChampion;
    }

    // ========== Generic Layout Algorithm ==========
    template <typename T>
    void NodeLayoutEngine::performLayeredLayout(std::vector<LayoutEntity<T>> & entities,
                                                const LayoutConfig & config,
                                                const std::vector<Rect> & occupiedRects,
                                                const nodes::graph::IDirectedGraph * graph)
    {
        if (entities.empty())
        {
            return;
        }

        // Step 1: Arrange entities in layers based on depth
            auto layers = arrangeInLayers(entities, graph);

        // Step 2: Calculate layer X positions
        // Ensure layerSpacing is measured from the rightmost edge of each layer
        // to guarantee no overlaps even if nodes have varying widths
        // Note: Depths are inverted so that inputs (low depth) are on left, outputs (high depth) on right
        std::map<int, float> layerXPositions;
        std::map<int, float> layerMaxRightEdge; // Track rightmost extent of each layer
        float currentX = 0.0f;
        
        // Iterate layers in normal depth order (low depth = inputs on left, high depth = outputs on right)
        for (auto & [depth, layerEntities] : layers)
        {
            layerXPositions[depth] = currentX;
            float maxWidth = 0.0f;
            for (auto * entity : layerEntities)
            {
                maxWidth = std::max(maxWidth, entity->size.x);
            }
            // Store the rightmost edge for this layer (all nodes start at currentX)
            layerMaxRightEdge[depth] = currentX + maxWidth;
            // Next layer starts after the rightmost extent + layerSpacing
            currentX = layerMaxRightEdge[depth] + config.layerSpacing;
        }

        // Step 3: Position entities in Y direction within each layer, avoiding occupied spaces
        for (auto & [depth, layerEntities] : layers)
        {
            float currentY = 0.0f;
            for (auto * entity : layerEntities)
            {
                entity->position.x = layerXPositions[depth];
                entity->position.y = currentY;

                // Check for overlap with occupied spaces and shift down if needed
                bool overlap = true;
                int maxTries = 1000; // avoid infinite loop
                while (overlap && maxTries-- > 0)
                {
                    overlap = false;
                    Rect entityRect(ImVec2(entity->position.x, entity->position.y),
                                    ImVec2(entity->position.x + entity->size.x,
                                           entity->position.y + entity->size.y));

                    for (const auto & occRect : occupiedRects)
                    {
                        if (entityRect.overlaps(occRect))
                        {
                            // Overlap detected, shift entity down
                            entity->position.y = occRect.max.y + config.nodeDistance;
                            currentY = entity->position.y;
                            overlap = true;
                            break;
                        }
                    }
                }
                currentY = entity->position.y + entity->size.y + config.nodeDistance;
            }
        }

        // Step 4: Optimize positions to minimize crossings using the simple approach
        optimizeLayerPositions(layers, config);

        // Step 5: Apply positions back to entities (handled by specific layout methods)
    }

    // ========== Group Analysis ==========

    std::vector<NodeLayoutEngine::GroupInfo>
    NodeLayoutEngine::analyzeGroups(nodes::Model & model,
                                    const std::unordered_map<nodes::NodeId, int> & depthMap)
    {
        std::unordered_map<std::string, GroupInfo> groupMap;

        // Collect nodes by group
        for (auto & [id, node] : model)
        {
            const std::string & tag = node->getTag();
            if (tag.empty())
            {
                continue; // Skip ungrouped nodes
            }

            auto depthIter = depthMap.find(id);
            int nodeDepth = (depthIter != depthMap.end()) ? depthIter->second : 0;

            if (groupMap.find(tag) == groupMap.end())
            {
                GroupInfo newGroup;
                newGroup.tag = tag;
                newGroup.nodes = {node.get()};
                newGroup.minDepth = nodeDepth;
                newGroup.maxDepth = nodeDepth;
                newGroup.position = ImVec2(0, 0);
                newGroup.size = ImVec2(0, 0);
                groupMap[tag] = std::move(newGroup);
            }
            else
            {
                auto & groupInfo = groupMap[tag];
                groupInfo.nodes.push_back(node.get());
                groupInfo.minDepth = std::min(groupInfo.minDepth, nodeDepth);
                groupInfo.maxDepth = std::max(groupInfo.maxDepth, nodeDepth);
            }
        }

        std::vector<GroupInfo> result;
        for (auto & [tag, info] : groupMap)
        {
            result.push_back(std::move(info));
        }

        return result;
    }

    // ========== Specific Layout Methods ==========

    void
    NodeLayoutEngine::layoutUngroupedNodes(const std::vector<nodes::NodeBase *> & ungroupedNodes,
                                           const std::unordered_map<nodes::NodeId, int> & depthMap,
                                           const LayoutConfig & config,
                                           const std::vector<Rect> & occupiedRects,
                                           const nodes::graph::IDirectedGraph * graph)
    {
        if (ungroupedNodes.empty())
        {
            return;
        }

        // Create entities for generic layout
        std::vector<NodeEntity> entities;
        entities.reserve(ungroupedNodes.size());

        for (auto * node : ungroupedNodes)
        {
            auto depthIter = depthMap.find(node->getId());
            int depth = (depthIter != depthMap.end()) ? depthIter->second : 0;

            entities.emplace_back(node, depth);
            auto & entity = entities.back();
            entity.size = calculateEntitySize(entity);
        }

        // Apply generic layered layout
    performLayeredLayout(entities, config, occupiedRects, graph);

        // Apply results back to nodes
                for (size_t i = 0; i < entities.size(); ++i)
                {
                        ungroupedNodes[i]->screenPos() =
                            nodes::float2(entities[i].position.x, entities[i].position.y);

                        if (m_nodePositionWriter)
                        {
                            m_nodePositionWriter(ungroupedNodes[i]->getId(),
                                         ImVec2(entities[i].position.x, entities[i].position.y));
                        }
                }
    }

    void
    NodeLayoutEngine::layoutNodesInGroup(GroupInfo & groupInfo,
                                         const std::unordered_map<nodes::NodeId, int> & depthMap,
                                         const LayoutConfig & config,
                                         const std::vector<Rect> & occupiedRects,
                                         const nodes::graph::IDirectedGraph * graph)
    {
        if (groupInfo.nodes.empty())
        {
            return;
        }

        // Create entities for nodes in this group
        std::vector<NodeEntity> entities;
        entities.reserve(groupInfo.nodes.size());

        for (auto * node : groupInfo.nodes)
        {
            auto depthIter = depthMap.find(node->getId());
            int depth = (depthIter != depthMap.end()) ? depthIter->second : 0;

            entities.emplace_back(node, depth);
            auto & entity = entities.back();
            entity.size = calculateEntitySize(entity);
        }

        // Apply generic layered layout
        LayoutConfig groupConfig = config;
        // Note: We keep full spacing to avoid overlaps, compactness comes from optimization
        // groupConfig.nodeDistance *= 0.7f; // Removed - was causing overlaps
        // groupConfig.layerSpacing *= 0.8f;  // Removed - was causing overlaps

    performLayeredLayout(entities, groupConfig, occupiedRects, graph);

        // Apply results back to nodes
                for (size_t i = 0; i < entities.size(); ++i)
                {
                        groupInfo.nodes[i]->screenPos() =
                            nodes::float2(entities[i].position.x, entities[i].position.y);

                        if (m_nodePositionWriter)
                        {
                            m_nodePositionWriter(groupInfo.nodes[i]->getId(),
                                         ImVec2(entities[i].position.x, entities[i].position.y));
                        }
                }

        // Update group bounds
        updateGroupBounds(groupInfo);
    }

    void NodeLayoutEngine::layoutGroups(std::vector<GroupInfo> & groups,
                                        const LayoutConfig & config,
                                        GroupLayoutMode mode)
    {
        switch (mode)
        {
            case GroupLayoutMode::VerticalStack:
                layoutGroupsStacked(groups, config);
                break;
            case GroupLayoutMode::HorizontalRow:
                layoutGroupsRowAligned(groups, config);
                break;
            case GroupLayoutMode::BalancedGrid:
                layoutGroupsBalancedGrid(groups, config);
                break;
            default:
                layoutGroupsStacked(groups, config);
                break;
        }
    }

    void NodeLayoutEngine::layoutGroupsStacked(std::vector<GroupInfo> & groups,
                                               const LayoutConfig & config)
    {
        if (groups.empty())
        {
            return;
        }

        std::sort(groups.begin(),
                  groups.end(),
                  [](GroupInfo const & a, GroupInfo const & b)
                  { return a.minDepth < b.minDepth; });

        ImVec2 currentPos(0.0f, 0.0f);
        float maxGroupHeight = 0.0f;
        int currentDepth = std::numeric_limits<int>::min();

        for (auto & group : groups)
        {
            if (group.minDepth != currentDepth)
            {
                if (currentDepth != std::numeric_limits<int>::min())
                {
                    currentPos.x += config.layerSpacing;
                    currentPos.y = 0.0f;
                }

                currentDepth = group.minDepth;
            }
            else
            {
                currentPos.y += maxGroupHeight + config.groupPadding;
            }

            ImVec2 const groupOffset = currentPos;

            for (auto * node : group.nodes)
            {
                if (!node)
                {
                    continue;
                }

                auto & nodePos = node->screenPos();
                nodePos.x += groupOffset.x;
                nodePos.y += groupOffset.y;

                if (m_nodePositionWriter)
                {
                    m_nodePositionWriter(node->getId(), ImVec2(nodePos.x, nodePos.y));
                }
            }

            updateGroupBounds(group);
            maxGroupHeight = std::max(maxGroupHeight, group.size.y);
        }
    }

    void NodeLayoutEngine::layoutGroupsRowAligned(std::vector<GroupInfo> & groups,
                                                  const LayoutConfig & config)
    {
        if (groups.empty())
        {
            return;
        }

        std::map<int, std::vector<GroupInfo *>> groupsByDepth;
        for (auto & group : groups)
        {
            updateGroupBounds(group);
            groupsByDepth[group.minDepth].push_back(&group);
        }

        float depthOffsetX = 0.0f;

        for (auto & [depth, grouped] : groupsByDepth)
        {
            if (grouped.empty())
            {
                continue;
            }

            std::sort(grouped.begin(),
                      grouped.end(),
                      [](GroupInfo const * lhs, GroupInfo const * rhs)
                      {
                          if (lhs->minDepth == rhs->minDepth)
                          {
                              return lhs < rhs;
                          }
                          return lhs->minDepth < rhs->minDepth;
                      });

            float const startX = depthOffsetX;
            float localX = depthOffsetX;

            for (auto * groupPtr : grouped)
            {
                if (groupPtr == nullptr)
                {
                    continue;
                }

                auto & group = *groupPtr;
                updateGroupBounds(group);

                float const groupWidth = std::max(group.size.x, config.groupPadding);

                ImVec2 const targetPosition(localX, 0.0f);
                ImVec2 const delta(targetPosition.x - group.position.x,
                                   targetPosition.y - group.position.y);

                if (delta.x != 0.0f || delta.y != 0.0f)
                {
                    for (auto * node : group.nodes)
                    {
                        if (node == nullptr)
                        {
                            continue;
                        }

                        auto & nodePos = node->screenPos();
                        nodePos.x += delta.x;
                        nodePos.y += delta.y;

                        if (m_nodePositionWriter)
                        {
                            m_nodePositionWriter(node->getId(), ImVec2(nodePos.x, nodePos.y));
                        }
                    }

                    updateGroupBounds(group);
                }

                localX += groupWidth + config.groupPadding;
            }

            float depthWidth = localX - startX;
            if (depthWidth > 0.0f)
            {
                depthWidth = std::max(depthWidth - config.groupPadding, 0.0f);
            }

            depthOffsetX = startX + depthWidth + config.layerSpacing;
        }
    }

    void NodeLayoutEngine::layoutGroupsBalancedGrid(std::vector<GroupInfo> & groups,
                                                    const LayoutConfig & config)
    {
        if (groups.empty())
        {
            return;
        }

        // Depth-aware placement: keep groups with the same minDepth on the same row.
        // Within a depth row, preserve relative Y by sorting on current centerY to reduce crossings.
        std::sort(groups.begin(), groups.end(), [](GroupInfo const & lhs, GroupInfo const & rhs) {
            if (lhs.minDepth != rhs.minDepth)
            {
                return lhs.minDepth < rhs.minDepth;
            }
            float const lhsCenter = lhs.position.y + lhs.size.y * 0.5F;
            float const rhsCenter = rhs.position.y + rhs.size.y * 0.5F;
            return lhsCenter < rhsCenter;
        });

        // Track already-placed groups to detect overlaps
        std::vector<Rect> placedGroupRects;

        float currentY = 0.0F;
        std::size_t i = 0U;
        while (i < groups.size())
        {
            int const depth = groups[i].minDepth;
            float currentX = 0.0F;
            float rowHeight = 0.0F;

            // Place all groups with this depth on the same row
            for (; i < groups.size() && groups[i].minDepth == depth; ++i)
            {
                auto & group = groups[i];
                updateGroupBounds(group);

                ImVec2 targetPosition(currentX, currentY);
                
                // Check for overlaps with already-placed groups and adjust Y if needed
                bool hasOverlap = true;
                int maxAdjustments = 100;
                while (hasOverlap && maxAdjustments-- > 0)
                {
                    hasOverlap = false;
                    Rect proposedRect(targetPosition,
                                     ImVec2(targetPosition.x + group.size.x,
                                            targetPosition.y + group.size.y));
                    
                    for (const auto & placedRect : placedGroupRects)
                    {
                        if (proposedRect.overlaps(placedRect))
                        {
                            // Shift down below the overlapping group
                            targetPosition.y = placedRect.max.y + config.groupPadding;
                            hasOverlap = true;
                            break;
                        }
                    }
                }

                ImVec2 const delta(targetPosition.x - group.position.x,
                                   targetPosition.y - group.position.y);

                if (delta.x != 0.0F || delta.y != 0.0F)
                {
                    for (auto * node : group.nodes)
                    {
                        if (node == nullptr)
                        {
                            continue;
                        }

                        auto & nodePos = node->screenPos();
                        nodePos.x += delta.x;
                        nodePos.y += delta.y;

                        if (m_nodePositionWriter)
                        {
                            m_nodePositionWriter(node->getId(), ImVec2(nodePos.x, nodePos.y));
                        }
                    }

                    updateGroupBounds(group);
                }

                // Add this group to the placed list
                placedGroupRects.push_back(Rect(group.position,
                                               ImVec2(group.position.x + group.size.x,
                                                      group.position.y + group.size.y)));

                currentX += group.size.x + config.groupPadding;
                rowHeight = std::max(rowHeight, group.size.y);
            }

            currentY += rowHeight + config.groupPadding;
        }
    }

    auto NodeLayoutEngine::capturePositions(nodes::Model & model) -> PositionSnapshot
    {
        PositionSnapshot snapshot;
        snapshot.reserve(model.getSize());

        for (auto & [id, node] : model)
        {
            if (!node)
            {
                continue;
            }

            snapshot.emplace(id, node->screenPos());
        }

        return snapshot;
    }

    void NodeLayoutEngine::restorePositions(nodes::Model & model,
                                            const PositionSnapshot & snapshot,
                                            bool notifyWriter)
    {
        for (auto & [id, node] : model)
        {
            if (!node)
            {
                continue;
            }

            auto const iter = snapshot.find(id);
            if (iter == snapshot.end())
            {
                continue;
            }

            node->screenPos() = iter->second;

            if (notifyWriter && m_nodePositionWriter)
            {
                m_nodePositionWriter(id, ImVec2(iter->second.x, iter->second.y));
            }
        }
    }

    bool NodeLayoutEngine::performAutoLayoutVariant(nodes::Model & model,
                                                    const LayoutConfig & config,
                                                    GroupLayoutMode mode)
    {
        // Since depth is now measured backward from output, we start from the end node
        auto * endNode = model.getEndNode();
        if (!endNode)
        {
            return false;
        }

        auto const & graph = model.getGraph();
        if (graph.getSize() < 2)
        {
            return false;
        }

        auto const endId = endNode->getId();
        auto depthMap = determineDepth(graph, endId);
        
        // For nodes not connected to the output (e.g., unused nodes), compute depth from begin node
        // This provides a fallback depth so they don't break the layout
        auto * beginNode = model.getBeginNode();
        if (beginNode)
        {
            auto const beginId = beginNode->getId();
            auto fallbackDepthMap = determineDepth(graph, beginId);
            
            // Add any nodes not in the main depth map
            for (const auto & [id, fallbackDepth] : fallbackDepthMap)
            {
                if (depthMap.find(id) == depthMap.end())
                {
                    depthMap[id] = fallbackDepth;
                }
            }
        }
        
        // For any remaining nodes not in either depth map (truly disconnected), assign default depth
        // This ensures all nodes get laid out even if they're not connected
        for (auto & [id, node] : model)
        {
            if (depthMap.find(id) == depthMap.end())
            {
                depthMap[id] = 0;
            }
        }
        
        // Invert depth values so that inputs have low depth (left) and output has high depth (right)
        // Find max depth first
        int maxDepth = 0;
        for (const auto & [id, depth] : depthMap)
        {
            maxDepth = std::max(maxDepth, depth);
        }
        // Invert all depths
        for (auto & [id, depth] : depthMap)
        {
            depth = maxDepth - depth;
        }

        std::vector<nodes::NodeBase *> ungroupedNodesPreFilter;
        ungroupedNodesPreFilter.reserve(model.getSize());

        for (auto & [id, node] : model)
        {
            if (!node)
            {
                continue;
            }

            if (node->getTag().empty())
            {
                ungroupedNodesPreFilter.push_back(node.get());
            }
        }

        auto groups = analyzeGroups(model, depthMap);

        std::vector<nodes::NodeBase *> ungroupedNodes;
        ungroupedNodes.reserve(ungroupedNodesPreFilter.size());

        for (auto * node : ungroupedNodesPreFilter)
        {
            bool inAGroup = false;
            for (auto const & group : groups)
            {
                if (std::find(group.nodes.begin(), group.nodes.end(), node) != group.nodes.end())
                {
                    inAGroup = true;
                    break;
                }
            }

            if (!inAGroup)
            {
                ungroupedNodes.push_back(node);
            }
        }

        std::vector<Rect> occupiedRects;

        for (auto & group : groups)
        {
            layoutNodesInGroup(group, depthMap, config, occupiedRects, &graph);
            updateGroupBounds(group);
        }

        layoutGroups(groups, config, mode);

        float maxGroupRight = 0.0f;
        for (auto & group : groups)
        {
            updateGroupBounds(group);

            maxGroupRight = std::max(maxGroupRight, group.position.x + group.size.x);
            occupiedRects.push_back(Rect(group.position,
                                         ImVec2(group.position.x + group.size.x,
                                                group.position.y + group.size.y)));
        }

        if (!ungroupedNodes.empty())
        {
            layoutUngroupedNodes(ungroupedNodes, depthMap, config, occupiedRects, &graph);

            float minUngroupedX = std::numeric_limits<float>::max();
            for (auto * node : ungroupedNodes)
            {
                minUngroupedX = std::min(minUngroupedX, node->screenPos().x);
            }

            if (minUngroupedX == std::numeric_limits<float>::max())
            {
                minUngroupedX = 0.0f;
            }

            float const targetX = groups.empty() ? 0.0f : maxGroupRight + config.layerSpacing;
            float const deltaX = targetX - minUngroupedX;

            for (auto * node : ungroupedNodes)
            {
                auto & nodePos = node->screenPos();
                nodePos.x += deltaX;

                if (m_nodePositionWriter)
                {
                    m_nodePositionWriter(node->getId(), ImVec2(nodePos.x, nodePos.y));
                }
            }
        }

        return true;
    }

    float NodeLayoutEngine::computeLayoutScore(const LayoutQualityAnalyzer::Metrics & metrics) const
    {
        // Scoring balances crossings, occupied area, and edge lengths.
        // Previous weights over-emphasized sumEdgeLength, drowning out area.
        // Tuned weights below make area competitive when crossing counts are close,
        // while still strongly penalizing overlaps and crossings.
        constexpr float EDGE_LENGTH_WEIGHT = 500.0f;         // was 1e5
        constexpr float MAX_EDGE_WEIGHT   = 1.0f;            // was 5.0
        constexpr float CROSSING_WEIGHT   = 250E3f;      // was 1'000'000, then 250k; lower to let area win with small crossing deltas
        constexpr float OVERLAP_WEIGHT    = 1E6f;   // unchanged magnitude

        float score = metrics.occupiedArea;

        // Normalize sumEdgeLength by edge count to avoid penalizing larger graphs disproportionately
        float const normalizedSumLen = (metrics.edgeCount > 0U)
                         ? (metrics.sumEdgeLength / static_cast<float>(metrics.edgeCount))
                         : metrics.sumEdgeLength;

        score += normalizedSumLen * EDGE_LENGTH_WEIGHT;
        score += metrics.maxEdgeLength * MAX_EDGE_WEIGHT;
        score += static_cast<float>(metrics.edgeCrossings) * CROSSING_WEIGHT;
        score += static_cast<float>(metrics.nodeOverlaps.size() + metrics.groupOverlaps.size()) *
             OVERLAP_WEIGHT;

        return score;
    }

    void NodeLayoutEngine::applyPostProcessing(nodes::Model & model,
                                               const LayoutConfig & config,
                                               const LayoutStrategy & strategy)
    {
        bool const requiresDepth = strategy.enableExtraRelaxation || strategy.enableGroupCompaction;
        std::unordered_map<nodes::NodeId, int> depthMap;

        if (requiresDepth)
        {
            // Since depth is now measured backward from output, we start from the end node
            auto * endNode = model.getEndNode();
            if (endNode != nullptr)
            {
                depthMap = determineDepth(model.getGraph(), endNode->getId());
                
                // For nodes not connected to the output (e.g., unused nodes), compute depth from begin node
                auto * beginNode = model.getBeginNode();
                if (beginNode != nullptr)
                {
                    auto const beginId = beginNode->getId();
                    auto fallbackDepthMap = determineDepth(model.getGraph(), beginId);
                    
                    // Add any nodes not in the main depth map
                    for (const auto & [id, fallbackDepth] : fallbackDepthMap)
                    {
                        if (depthMap.find(id) == depthMap.end())
                        {
                            depthMap[id] = fallbackDepth;
                        }
                    }
                }
                
                // For any remaining disconnected nodes, assign default depth
                for (auto & [id, node] : model)
                {
                    if (depthMap.find(id) == depthMap.end())
                    {
                        depthMap[id] = 0;
                    }
                }
                
                // Invert depth values so that inputs have low depth (left) and output has high depth (right)
                int maxDepth = 0;
                for (const auto & [id, depth] : depthMap)
                {
                    maxDepth = std::max(maxDepth, depth);
                }
                for (auto & [id, depth] : depthMap)
                {
                    depth = maxDepth - depth;
                }
            }
        }

        if (strategy.enableExtraRelaxation && !depthMap.empty())
        {
            balanceLayers(model, depthMap, config, strategy.extraRelaxationPasses);
        }

        if (strategy.enableGroupCompaction && !depthMap.empty())
        {
            compactLayersHorizontally(model, depthMap, config);
        }

        if (strategy.alignToOrigin)
        {
            shiftLayoutToOrigin(model);
        }
    }

    void NodeLayoutEngine::balanceLayers(nodes::Model & model,
                                         const std::unordered_map<nodes::NodeId, int> & depthMap,
                                         const LayoutConfig & config,
                                         int passes)
    {
        if (passes <= 0 || depthMap.empty())
        {
            return;
        }

        std::map<int, std::vector<nodes::NodeBase *>> layers;
        for (auto & [id, node] : model)
        {
            if (!node)
            {
                continue;
            }

            auto const depthIter = depthMap.find(id);
            if (depthIter == depthMap.end())
            {
                // This should no longer happen since we assign depth to all nodes
                continue;
            }
            int const depth = depthIter->second;
            layers[depth].push_back(node.get());
        }

        for (int pass = 0; pass < passes; ++pass)
        {
            // First pass: compute global vertical center to anchor the layout
            float globalMinY = std::numeric_limits<float>::max();
            float globalMaxY = -std::numeric_limits<float>::max();
            
            for (auto & [depth, layerNodes] : layers)
            {
                for (auto * node : layerNodes)
                {
                    ImVec2 const size = resolveNodeSize(*node);
                    float const nodeY = node->screenPos().y;
                    globalMinY = std::min(globalMinY, nodeY);
                    globalMaxY = std::max(globalMaxY, nodeY + size.y);
                }
            }
            
            float const globalCenter = (globalMinY + globalMaxY) * 0.5F;
            
            // Second pass: re-center each layer around the global center
            for (auto & [depth, layerNodes] : layers)
            {
                if (layerNodes.empty())
                {
                    continue;
                }

                std::sort(layerNodes.begin(),
                          layerNodes.end(),
                          [](nodes::NodeBase * lhs, nodes::NodeBase * rhs)
                          { return lhs->screenPos().y < rhs->screenPos().y; });

                std::vector<float> heights;
                heights.reserve(layerNodes.size());
                float totalHeight = -config.nodeDistance;

                for (auto * node : layerNodes)
                {
                    ImVec2 const size = resolveNodeSize(*node);
                    heights.push_back(size.y);
                    totalHeight += size.y + config.nodeDistance;
                }

                if (layerNodes.size() == 1U)
                {
                    totalHeight = heights.front();
                }

                totalHeight = std::max(totalHeight, 0.0F);

                // Center this layer's content around the global center
                float const startY = globalCenter - totalHeight * 0.5F;
                float currentY = startY;

                for (size_t index = 0U; index < layerNodes.size(); ++index)
                {
                    auto * node = layerNodes[index];
                    float const height = heights[index];
                    auto & pos = node->screenPos();
                    pos.y = currentY;
                    currentY += height + config.nodeDistance;

                    if (m_nodePositionWriter)
                    {
                        m_nodePositionWriter(node->getId(), ImVec2(pos.x, pos.y));
                    }
                }
            }
        }
    }

    void NodeLayoutEngine::compactLayersHorizontally(
      nodes::Model & model,
      const std::unordered_map<nodes::NodeId, int> & depthMap,
      const LayoutConfig & config)
    {
        if (depthMap.empty())
        {
            return;
        }

        // Collect nodes per depth (ordered by depth)
        std::map<int, std::vector<nodes::NodeBase *>> layers;
        for (auto & [id, node] : model)
        {
            if (!node)
            {
                continue;
            }

            auto const depthIter = depthMap.find(id);
            if (depthIter == depthMap.end())
            {
                // Skip nodes that don't have a depth assigned - they're not part of the layout
                continue;
            }
            int const depth = depthIter->second;
            layers[depth].push_back(node.get());
        }

        float previousRight = -std::numeric_limits<float>::infinity();
        bool firstLayer = true;

        for (auto & [depth, layerNodes] : layers)
        {
            if (layerNodes.empty())
            {
                continue;
            }

            // Compute current layer bounds
            float minX = std::numeric_limits<float>::infinity();
            float maxRight = -std::numeric_limits<float>::infinity();
            for (auto * node : layerNodes)
            {
                auto const & pos = node->screenPos();
                minX = std::min(minX, pos.x);
                maxRight = std::max(maxRight, pos.x + resolveNodeSize(*node).x);
            }

            if (!std::isfinite(minX) || !std::isfinite(maxRight))
            {
                continue;
            }

            float deltaX = 0.0F;
            if (firstLayer)
            {
                // Anchor first layer at its current minX (or at 0 if desired later)
                previousRight = maxRight;
                firstLayer = false;
            }
            else
            {
                // Place this layer so its left edge is at least previousRight + layerSpacing
                float const requiredMinX = previousRight + config.layerSpacing;
                if (minX < requiredMinX - 1e-3F)
                {
                    deltaX = requiredMinX - minX;
                }
                // If minX is already >= requiredMinX, we keep it (no left shift to avoid overlaps)
            }

            if (std::abs(deltaX) > 1e-6F)
            {
                for (auto * node : layerNodes)
                {
                    auto & pos = node->screenPos();
                    pos.x += deltaX;

                    if (m_nodePositionWriter)
                    {
                        m_nodePositionWriter(node->getId(), ImVec2(pos.x, pos.y));
                    }
                }

                // Update bounds after shifting
                minX += deltaX;
                maxRight += deltaX;
            }

            // Track rightmost extent for next layer placement
            previousRight = std::max(previousRight, maxRight);
        }
    }

    void NodeLayoutEngine::shiftLayoutToOrigin(nodes::Model & model)
    {
        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();

        for (auto & [id, node] : model)
        {
            if (!node)
            {
                continue;
            }

            auto const pos = node->screenPos();
            minX = std::min(minX, pos.x);
            minY = std::min(minY, pos.y);
        }

        if (!std::isfinite(minX) || !std::isfinite(minY))
        {
            return;
        }

        if (std::abs(minX) < 1e-3F && std::abs(minY) < 1e-3F)
        {
            return;
        }

        for (auto & [id, node] : model)
        {
            if (!node)
            {
                continue;
            }

            auto & pos = node->screenPos();
            pos.x -= minX;
            pos.y -= minY;

            if (m_nodePositionWriter)
            {
                m_nodePositionWriter(id, ImVec2(pos.x, pos.y));
            }
        }
    }

    // ========== Helper Methods ==========

    std::unordered_map<nodes::NodeId, int>
    NodeLayoutEngine::determineDepth(const nodes::graph::IDirectedGraph & graph,
                                     nodes::NodeId beginId)
    {
        return nodes::graph::determineDepth(graph, beginId);
    }

    template <typename T>
    std::map<int, std::vector<NodeLayoutEngine::LayoutEntity<T> *>>
        NodeLayoutEngine::arrangeInLayers(std::vector<LayoutEntity<T>> & entities,
                                          const nodes::graph::IDirectedGraph * graph)
    {
        std::map<int, std::vector<NodeLayoutEngine::LayoutEntity<T> *>> layers;

        for (auto & entity : entities)
        {
            layers[entity.depth].push_back(&entity);
        }

        // Sort within each layer (for consistent ordering)
        for (auto & [depth, layerEntities] : layers)
        {
            std::sort(layerEntities.begin(),
                      layerEntities.end(),
                      [](const LayoutEntity<T> * a, const LayoutEntity<T> * b)
                      {
                          return a->item < b->item; // Pointer comparison for consistency
                      });
        }

        // Establish connections between entities based on the graph structure
        if constexpr (std::is_same_v<T, nodes::NodeBase>)
        {
            if (graph != nullptr)
            {
                std::unordered_map<nodes::NodeId, LayoutEntity<T> *> entityByNodeId;
                entityByNodeId.reserve(entities.size());
                for (auto & entity : entities)
                {
                    entityByNodeId[entity.item->getId()] = &entity;
                }

                for (auto & entity : entities)
                {
                    entity.dependencies.clear();
                    entity.dependents.clear();
                }

                for (auto & entity : entities)
                {
                    nodes::NodeId const nodeId = entity.item->getId();
                    auto const dependencies =
                      nodes::graph::determineDirectDependencies(*graph, nodeId);

                    for (auto const dependencyId : dependencies)
                    {
                        auto const depIt = entityByNodeId.find(dependencyId);
                        if (depIt == entityByNodeId.end())
                        {
                            continue;
                        }

                        auto * dependencyEntity = depIt->second;

                        if (std::find(entity.dependencies.begin(),
                                      entity.dependencies.end(),
                                      dependencyEntity) == entity.dependencies.end())
                        {
                            entity.dependencies.push_back(dependencyEntity);
                        }

                        if (std::find(dependencyEntity->dependents.begin(),
                                      dependencyEntity->dependents.end(),
                                      &entity) == dependencyEntity->dependents.end())
                        {
                            dependencyEntity->dependents.push_back(&entity);
                        }
                    }
                }
            }
        }

        return layers;
    }

    template <typename T>
    void NodeLayoutEngine::optimizeLayerPositions(
          std::map<int, std::vector<NodeLayoutEngine::LayoutEntity<T> *>> & layers,
          const LayoutConfig & config)
    {
        if (layers.empty())
            return;

            std::vector<int> layerDepths;
            layerDepths.reserve(layers.size());
            for (auto & [depth, _] : layers)
            {
                layerDepths.push_back(depth);
            }
            std::sort(layerDepths.begin(), layerDepths.end());

            for (int depth : layerDepths)
            {
                auto & layerEntities = layers[depth];
                if (layerEntities.size() == 1)
                {
                    layerEntities[0]->position.y = 0.0f;
                }
            }

            int const maxIterations = std::max(1, config.maxOptimizationIterations);
            for (int iteration = 0; iteration < maxIterations; ++iteration)
            {
                float totalMovement = 0.0f;

                for (int depth : layerDepths)
                {
                    auto & layerEntities = layers[depth];
                    if (layerEntities.size() <= 1)
                    {
                        continue;
                    }

                    totalMovement +=
                      optimizeLayerByConnectionOrder(layerEntities, layers, depth, config);
                }

                for (auto depthIt = layerDepths.rbegin(); depthIt != layerDepths.rend(); ++depthIt)
                {
                    auto & layerEntities = layers[*depthIt];
                    if (layerEntities.size() <= 1)
                    {
                        continue;
                    }

                    totalMovement +=
                      optimizeLayerByConnectionOrder(layerEntities, layers, *depthIt, config);
                }

                // Local transpose: try to reduce crossings by adjacent swaps after ordering
                for (int depth : layerDepths)
                {
                    auto & layerEntities = layers[depth];
                    if (layerEntities.size() <= 1)
                    {
                        continue;
                    }
                    transposeReduceCrossings(layerEntities, layers, depth, config);
                }

                if (totalMovement < config.convergenceThreshold)
                {
                    break;
                }
            }

            for (int relaxPass = 0; relaxPass < 3; ++relaxPass)
            {
                float relaxationMovement = 0.0f;
                for (int depth : layerDepths)
                {
                    auto & layerEntities = layers[depth];
                    if (layerEntities.size() <= 1)
                    {
                        continue;
                    }

                    relaxationMovement +=
                      relaxLayerPositions(layerEntities, layers, depth, config);
                }

                if (relaxationMovement < config.convergenceThreshold)
                {
                    break;
                }
            }
    }

    // Build a rank map (by vertical center) for entities in a given neighbor layer
    template <typename T>
    static std::unordered_map<NodeLayoutEngine::LayoutEntity<T> *, int>
      buildRankMap(
        const std::map<int, std::vector<NodeLayoutEngine::LayoutEntity<T> *>> & allLayers,
        int neighborDepth)
    {
        std::unordered_map<NodeLayoutEngine::LayoutEntity<T> *, int> rank;
        auto it = allLayers.find(neighborDepth);
        if (it == allLayers.end())
        {
            return rank;
        }
        auto entities = it->second;
        std::sort(entities.begin(),
                  entities.end(),
                  [](auto * a, auto * b)
                  {
                      float ca = a->position.y + a->size.y * 0.5f;
                      float cb = b->position.y + b->size.y * 0.5f;
                      if (ca == cb)
                          return a < b;
                      return ca < cb;
                  });
        for (size_t i = 0; i < entities.size(); ++i)
        {
            rank[entities[i]] = static_cast<int>(i);
        }
        return rank;
    }

    // Count crossing contribution for an adjacent pair (A,B) with neighbor rank maps
    template <typename T>
    static int countPairCrossings(NodeLayoutEngine::LayoutEntity<T> * A,
                                  NodeLayoutEngine::LayoutEntity<T> * B,
                                  const std::unordered_map<NodeLayoutEngine::LayoutEntity<T> *, int> & prevRank,
                                  const std::unordered_map<NodeLayoutEngine::LayoutEntity<T> *, int> & nextRank)
    {
        auto isAbove = [](auto * lhs, auto * rhs)
        { return (lhs->position.y + lhs->size.y * 0.5f) < (rhs->position.y + rhs->size.y * 0.5f); };

        bool const AAboveB = isAbove(A, B);
        int crossings = 0;

        // Previous layer dependencies
        if (!prevRank.empty())
        {
            for (auto * aDep : A->dependencies)
            {
                if (prevRank.find(aDep) == prevRank.end())
                    continue;
                for (auto * bDep : B->dependencies)
                {
                    auto ita = prevRank.find(aDep);
                    auto itb = prevRank.find(bDep);
                    if (itb == prevRank.end())
                        continue;
                    bool inversion = (ita->second > itb->second);
                    crossings += (AAboveB && inversion) || (!AAboveB && !inversion);
                }
            }
        }

        // Next layer dependents
        if (!nextRank.empty())
        {
            for (auto * aDep : A->dependents)
            {
                if (nextRank.find(aDep) == nextRank.end())
                    continue;
                for (auto * bDep : B->dependents)
                {
                    auto ita = nextRank.find(aDep);
                    auto itb = nextRank.find(bDep);
                    if (itb == nextRank.end())
                        continue;
                    bool inversion = (ita->second > itb->second);
                    crossings += (AAboveB && inversion) || (!AAboveB && !inversion);
                }
            }
        }

        return crossings;
    }

    // Compute total crossings contributed by a whole ordered layer with neighbor ranks
    template <typename T>
    static int countLayerCrossings(
      const std::vector<NodeLayoutEngine::LayoutEntity<T> *> & ordered,
      const std::unordered_map<NodeLayoutEngine::LayoutEntity<T> *, int> & prevRank,
      const std::unordered_map<NodeLayoutEngine::LayoutEntity<T> *, int> & nextRank)
    {
        int total = 0;
        if (ordered.size() <= 1)
        {
            return 0;
        }
        for (size_t i = 0; i + 1 < ordered.size(); ++i)
        {
            total += countPairCrossings<T>(ordered[i], ordered[i + 1], prevRank, nextRank);
        }
        return total;
    }

    // Try to reduce crossings with adjacent swaps inside a layer; stable and local
    template <typename T>
    void NodeLayoutEngine::transposeReduceCrossings(
      std::vector<NodeLayoutEngine::LayoutEntity<T> *> & layerEntities,
      const std::map<int, std::vector<NodeLayoutEngine::LayoutEntity<T> *>> & allLayers,
      int currentDepth,
      const LayoutConfig & config)
    {
        if (layerEntities.size() <= 1)
        {
            return;
        }

        // Build neighbor rank maps once per call
        // Note: Depths are inverted so inputs have low depth, outputs have high depth
        // Previous layer (dependencies/inputs) has lower depth, next layer (dependents/outputs) has higher depth
        auto prevRank = buildRankMap<T>(allLayers, currentDepth - 1);  // dependencies (inputs, left)
        auto nextRank = buildRankMap<T>(allLayers, currentDepth + 1);  // dependents (outputs, right)

        // Start from current vertical order by position.y (top to bottom)
        std::sort(layerEntities.begin(),
                  layerEntities.end(),
                  [](auto * a, auto * b) { return a->position.y < b->position.y; });

        bool improved = true;
        int const MAX_SWEEPS = 6;
        int sweeps = 0;
        while (improved && sweeps++ < MAX_SWEEPS)
        {
            improved = false;
            // Evaluate adjacent swaps using hypothetical order without touching positions
            for (size_t i = 0; i + 1 < layerEntities.size(); ++i)
            {
                int before = countPairCrossings<T>(layerEntities[i],
                                                   layerEntities[i + 1],
                                                   prevRank,
                                                   nextRank);
                if (before <= 0)
                {
                    continue;
                }

                std::swap(layerEntities[i], layerEntities[i + 1]);
                int after = countPairCrossings<T>(layerEntities[i],
                                                  layerEntities[i + 1],
                                                  prevRank,
                                                  nextRank);
                if (after < before)
                {
                    improved = true;
                }
                else
                {
                    // Revert if not an improvement
                    std::swap(layerEntities[i], layerEntities[i + 1]);
                }
            }
        }

        // After potential reordering, repack Y positions to enforce non-overlap
        float previousBottom = -std::numeric_limits<float>::infinity();
        for (auto * entity : layerEntities)
        {
            float const halfH = entity->size.y * 0.5f;
            float targetTop = entity->position.y; // start from current
            if (std::isfinite(previousBottom))
            {
                float const minTop = previousBottom + config.nodeDistance;
                if (targetTop < minTop)
                {
                    targetTop = minTop;
                }
            }
            entity->position.y = targetTop;
            previousBottom = targetTop + entity->size.y;
        }
    }

    template <typename T>
    float NodeLayoutEngine::optimizeLayerByConnectionOrder(
          std::vector<NodeLayoutEngine::LayoutEntity<T> *> & layerEntities,
          const std::map<int, std::vector<NodeLayoutEngine::LayoutEntity<T> *>> & allLayers,
          int currentDepth,
          const LayoutConfig & config)
    {
                    if (layerEntities.empty())
                            return 0.0f;

            struct Entry
            {
                NodeLayoutEngine::LayoutEntity<T> * entity;
                float desiredCenter;
                float originalCenter;
            };

            std::vector<Entry> entries;
            entries.reserve(layerEntities.size());

            float desiredCenterSum = 0.0f;
                        for (auto * entity : layerEntities)
                        {
                                float connectedY;
                                if (config.useMedianForOrdering)
                                {
                                    // Use weighted median for strategies that request it explicitly
                                    if (config.convergenceThreshold < 0.8f)
                                    {
                                        connectedY = calculateWeightedMedianConnectedY(entity,
                                                                                       allLayers,
                                                                                       currentDepth,
                                                                                       config);
                                    }
                                    else
                                    {
                                        connectedY = calculateMedianConnectedY(entity,
                                                                               allLayers,
                                                                               currentDepth,
                                                                               config);
                                    }
                                }
                                else
                                {
                                    connectedY = calculateAverageConnectedY(entity,
                                                                            allLayers,
                                                                            currentDepth,
                                                                            config);
                                }
                                float const originalCenter = entity->position.y + entity->size.y / 2.0f;
                                entries.push_back({entity, connectedY, originalCenter});
                                desiredCenterSum += connectedY;
                        }

            std::sort(entries.begin(),
                      entries.end(),
                      [](Entry const & a, Entry const & b)
                      {
                          if (a.desiredCenter == b.desiredCenter)
                          {
                              return a.originalCenter < b.originalCenter;
                          }
                          return a.desiredCenter < b.desiredCenter;
                      });

            float previousBottom = -std::numeric_limits<float>::infinity();
            float actualCenterSum = 0.0f;

            for (auto & entry : entries)
            {
                auto * entity = entry.entity;
                float const halfHeight = entity->size.y * 0.5f;
                float targetTop = entry.desiredCenter - halfHeight;

                if (!std::isfinite(targetTop))
                {
                    targetTop = entity->position.y;
                }

                if (std::isfinite(previousBottom))
                {
                    float const minTop = previousBottom + config.nodeDistance;
                    if (targetTop < minTop)
                    {
                        targetTop = minTop;
                    }
                }

                entity->position.y = targetTop;
                previousBottom = targetTop + entity->size.y;
                actualCenterSum += targetTop + halfHeight;
            }

            float const desiredAverage = desiredCenterSum / static_cast<float>(entries.size());
            float const actualAverage = actualCenterSum / static_cast<float>(entries.size());
            float const layerShift = desiredAverage - actualAverage;

            for (auto & entry : entries)
            {
                entry.entity->position.y += layerShift;
            }

            // After shifting, re-check for overlaps and fix them
            std::sort(entries.begin(),
                      entries.end(),
                      [](Entry const & a, Entry const & b)
                      { return a.entity->position.y < b.entity->position.y; });

            previousBottom = -std::numeric_limits<float>::infinity();
            for (auto & entry : entries)
            {
                auto * entity = entry.entity;
                if (std::isfinite(previousBottom))
                {
                    float const minTop = previousBottom + config.nodeDistance;
                    if (entity->position.y < minTop)
                    {
                        entity->position.y = minTop;
                    }
                }
                previousBottom = entity->position.y + entity->size.y;
            }

            std::vector<NodeLayoutEngine::LayoutEntity<T> *> reordered;
            reordered.reserve(entries.size());
            for (auto & entry : entries)
            {
                reordered.push_back(entry.entity);
            }
            layerEntities = std::move(reordered);

            float totalMovement = 0.0f;
            for (auto & entry : entries)
            {
                float const newCenter = entry.entity->position.y + entry.entity->size.y / 2.0f;
                totalMovement += std::abs(newCenter - entry.originalCenter);
            }

            return totalMovement;
    }

    template <typename T>
    float NodeLayoutEngine::calculateAverageConnectedY(
      NodeLayoutEngine::LayoutEntity<T> * entity,
      const std::map<int, std::vector<NodeLayoutEngine::LayoutEntity<T> *>> & allLayers,
      int currentDepth,
      const LayoutConfig & config)
    {
        if constexpr (std::is_same_v<T, nodes::NodeBase>)
        {
            std::vector<float> connectedYPositions;

            // Use the dependencies and dependents we already established
            for (auto * dep : entity->dependencies)
            {
                float connectedCenterY = dep->position.y + dep->size.y / 2.0f;
                connectedYPositions.push_back(connectedCenterY);
            }

            for (auto * dep : entity->dependents)
            {
                float connectedCenterY = dep->position.y + dep->size.y / 2.0f;
                connectedYPositions.push_back(connectedCenterY);
            }

            if (!connectedYPositions.empty())
            {
                // Return average Y position of all connected nodes
                float sum = 0.0f;
                for (float y : connectedYPositions)
                {
                    sum += y;
                }
                return sum / static_cast<float>(connectedYPositions.size());
            }
        }

        // If no connections found, return current center Y
        return entity->position.y + entity->size.y / 2.0f;
    }

    template <typename T>
    float NodeLayoutEngine::calculateMedianConnectedY(
      NodeLayoutEngine::LayoutEntity<T> * entity,
      const std::map<int, std::vector<NodeLayoutEngine::LayoutEntity<T> *>> & allLayers,
      int currentDepth,
      const LayoutConfig & config)
    {
        if constexpr (std::is_same_v<T, nodes::NodeBase>)
        {
            std::vector<float> ys;

            // Collect centers of connected neighbors (dependencies and dependents)
            for (auto * dep : entity->dependencies)
            {
                ys.push_back(dep->position.y + dep->size.y * 0.5f);
            }
            for (auto * dep : entity->dependents)
            {
                ys.push_back(dep->position.y + dep->size.y * 0.5f);
            }

            if (!ys.empty())
            {
                std::sort(ys.begin(), ys.end());
                size_t const n = ys.size();
                if (n % 2 == 1)
                {
                    return ys[n / 2];
                }
                else
                {
                    return 0.5f * (ys[n / 2 - 1] + ys[n / 2]);
                }
            }
        }

        // Fallback: use current center Y when no connections or unsupported type
        return entity->position.y + entity->size.y / 2.0f;
    }

    template <typename T>
    float NodeLayoutEngine::calculateWeightedMedianConnectedY(
      NodeLayoutEngine::LayoutEntity<T> * entity,
      const std::map<int, std::vector<NodeLayoutEngine::LayoutEntity<T> *>> & allLayers,
      int currentDepth,
      const LayoutConfig & config)
    {
        if constexpr (std::is_same_v<T, nodes::NodeBase>)
        {
            // Build weighted list: weight by edge span (depth difference)
            std::vector<std::pair<float, float>> weightedYs; // (y, weight)

            for (auto * dep : entity->dependencies)
            {
                float const centerY = dep->position.y + dep->size.y * 0.5f;
                int const depthDelta = std::abs(dep->depth - entity->depth);
                float const weight = 1.0f + 0.3f * static_cast<float>(depthDelta);
                weightedYs.push_back({centerY, weight});
            }
            for (auto * dep : entity->dependents)
            {
                float const centerY = dep->position.y + dep->size.y * 0.5f;
                int const depthDelta = std::abs(dep->depth - entity->depth);
                float const weight = 1.0f + 0.3f * static_cast<float>(depthDelta);
                weightedYs.push_back({centerY, weight});
            }

            if (!weightedYs.empty())
            {
                // Sort by Y position
                std::sort(weightedYs.begin(), weightedYs.end(),
                          [](auto const & a, auto const & b) { return a.first < b.first; });

                // Compute total weight
                float totalWeight = 0.0f;
                for (auto const & [y, w] : weightedYs)
                {
                    totalWeight += w;
                }

                // Find weighted median
                float cumWeight = 0.0f;
                float const halfWeight = totalWeight * 0.5f;
                for (size_t i = 0; i < weightedYs.size(); ++i)
                {
                    cumWeight += weightedYs[i].second;
                    if (cumWeight >= halfWeight)
                    {
                        // Interpolate between i-1 and i if we're not exactly at the boundary
                        if (i > 0 && cumWeight - weightedYs[i].second < halfWeight)
                        {
                            float const ratio = (halfWeight - (cumWeight - weightedYs[i].second)) / weightedYs[i].second;
                            return weightedYs[i - 1].first * (1.0f - ratio) + weightedYs[i].first * ratio;
                        }
                        return weightedYs[i].first;
                    }
                }
            }
        }

        return entity->position.y + entity->size.y / 2.0f;
    }

template <typename T>
float NodeLayoutEngine::relaxLayerPositions(
      std::vector<NodeLayoutEngine::LayoutEntity<T> *> & layerEntities,
      const std::map<int, std::vector<NodeLayoutEngine::LayoutEntity<T> *>> & allLayers,
      int currentDepth,
      const LayoutConfig & config)
    {
        size_t const count = layerEntities.size();
        if (count <= 1)
        {
            return 0.0f;
        }

        struct PavBlock
        {
            size_t start;
            size_t end;
            float weight;
            float value;
        };

        std::vector<float> heights(count);
        std::vector<float> halfHeights(count);
        std::vector<float> originalCenters(count);
        std::vector<float> weights(count);
        std::vector<float> targetCenters(count);

        for (size_t i = 0; i < count; ++i)
        {
            auto * entity = layerEntities[i];
            float const height = std::max(entity->size.y, 1.0f);
            heights[i] = height;
            halfHeights[i] = height * 0.5f;
            originalCenters[i] = entity->position.y + halfHeights[i];

            float weightedSum = originalCenters[i] * 0.1f;
            float totalWeight = 0.1f;

            if constexpr (std::is_same_v<T, nodes::NodeBase>)
            {
                auto accumulateNeighbor = [&](auto const & connections)
                {
                    for (auto * neighbor : connections)
                    {
                        if (neighbor == nullptr)
                        {
                            continue;
                        }

                        float const neighborCenter =
                          neighbor->position.y + neighbor->size.y * 0.5f;
                        int const depthDelta = std::abs(neighbor->depth - layerEntities[i]->depth);
                        float const spanWeight = 1.0f + 0.5f * static_cast<float>(depthDelta);
                        weightedSum += neighborCenter * spanWeight;
                        totalWeight += spanWeight;
                    }
                };

                accumulateNeighbor(layerEntities[i]->dependencies);
                accumulateNeighbor(layerEntities[i]->dependents);
            }

            if (totalWeight <= 0.0f)
            {
                totalWeight = 1.0f;
                weightedSum = originalCenters[i];
            }

            targetCenters[i] = weightedSum / totalWeight;
            weights[i] = totalWeight;
        }

        std::vector<float> offsets(count, 0.0f);
        for (size_t i = 1; i < count; ++i)
        {
            float const gap = heights[i - 1] + config.nodeDistance;
            offsets[i] = offsets[i - 1] + gap;
        }

        std::vector<float> adjustedTargets(count);
        for (size_t i = 0; i < count; ++i)
        {
            float const targetTop = targetCenters[i] - halfHeights[i];
            adjustedTargets[i] = targetTop - offsets[i];
        }

        std::vector<PavBlock> blockStack;
        blockStack.reserve(count);

        for (size_t i = 0; i < count; ++i)
        {
            float const weight = std::max(weights[i], 1e-3f);
            PavBlock block{i, i, weight, adjustedTargets[i]};
            blockStack.push_back(block);

            while (blockStack.size() >= 2)
            {
                auto & last = blockStack.back();
                auto & prev = blockStack[blockStack.size() - 2];
                if (prev.value <= last.value)
                {
                    break;
                }

                float const combinedWeight = prev.weight + last.weight;
                float const combinedValue =
                  (prev.value * prev.weight + last.value * last.weight) / combinedWeight;
                prev.end = last.end;
                prev.weight = combinedWeight;
                prev.value = combinedValue;
                blockStack.pop_back();
            }
        }

        std::vector<float> monotoneValues(count);
        for (auto const & block : blockStack)
        {
            for (size_t idx = block.start; idx <= block.end; ++idx)
            {
                monotoneValues[idx] = block.value;
            }
        }

        float totalMovement = 0.0f;
        for (size_t i = 0; i < count; ++i)
        {
            float const topPosition = monotoneValues[i] + offsets[i];
            float const newCenter = topPosition + halfHeights[i];
            totalMovement += std::abs(newCenter - originalCenters[i]);
            layerEntities[i]->position.y = topPosition;
        }

        return totalMovement;
    }

    template <typename T>
    bool NodeLayoutEngine::areNodesConnected(T * node1, T * node2)
    {
        if constexpr (std::is_same_v<T, nodes::NodeBase>)
        {
            nodes::NodeId id1 = node1->getId();
            nodes::NodeId id2 = node2->getId();

            // For now, use a simple approach based on the dependency relationships
            // we established earlier in arrangeInLayers
            // In a real implementation, we would need access to the model/graph
            // to check actual port connections

            // This is a placeholder - we'll need to pass the model/graph
            // to this method to check actual connections
            return false; // Will be implemented properly when we have graph access
        }

        return false;
    }

    template <typename T>
    void NodeLayoutEngine::optimizeSingleLayer(
      std::vector<NodeLayoutEngine::LayoutEntity<T> *> & layerEntities,
      const std::map<int, std::vector<NodeLayoutEngine::LayoutEntity<T> *>> & allLayers,
      int currentDepth,
      const LayoutConfig & config)
    {
        const int MAX_ITERATIONS = 50;
        const float CONVERGENCE_THRESHOLD = 1.0f;

        // Group entities by their group membership (for nodes)
        std::vector<std::vector<NodeLayoutEngine::LayoutEntity<T> *>> entityGroups;
        std::vector<NodeLayoutEngine::LayoutEntity<T> *> ungroupedEntities;

        if constexpr (std::is_same_v<T, nodes::NodeBase>)
        {
            groupEntitiesByTag(
              reinterpret_cast<const std::vector<LayoutEntity<nodes::NodeBase> *> &>(layerEntities),
              reinterpret_cast<std::vector<std::vector<LayoutEntity<nodes::NodeBase> *>> &>(
                entityGroups),
              reinterpret_cast<std::vector<LayoutEntity<nodes::NodeBase> *> &>(ungroupedEntities));
        }
        else
        {
            // For non-node entities, treat each as individual
            for (auto * entity : layerEntities)
            {
                ungroupedEntities.push_back(entity);
            }
        }

        // Combine groups and individual entities for optimization
        std::vector<OptimizationUnit<T>> optimizationUnits;

        // Add groups as units
        for (auto & group : entityGroups)
        {
            optimizationUnits.emplace_back(group);
        }

        // Add individual entities as units
        for (auto * entity : ungroupedEntities)
        {
            optimizationUnits.emplace_back(
              std::vector<NodeLayoutEngine::LayoutEntity<T> *>{entity});
        }

        // Iterative optimization
        for (int iteration = 0; iteration < MAX_ITERATIONS; ++iteration)
        {
            float totalMovement = 0.0f;

            for (auto & unit : optimizationUnits)
            {
                float oldY = calculateUnitCenterY(unit);
                float optimalY = calculateOptimalYPosition(unit, allLayers, currentDepth, config);

                // Move the unit to optimal position
                moveOptimizationUnit(unit, optimalY);

                float newY = calculateUnitCenterY(unit);
                totalMovement += std::abs(newY - oldY);
            }

            // Resolve overlaps while preserving group clustering
            resolveLayerOverlaps(optimizationUnits, config);

            // Check for convergence
            if (totalMovement < CONVERGENCE_THRESHOLD)
            {
                break;
            }
        }
    }

    void NodeLayoutEngine::groupEntitiesByTag(
      const std::vector<LayoutEntity<nodes::NodeBase> *> & entities,
      std::vector<std::vector<LayoutEntity<nodes::NodeBase> *>> & groups,
      std::vector<LayoutEntity<nodes::NodeBase> *> & ungrouped)
    {
        std::unordered_map<std::string, std::vector<LayoutEntity<nodes::NodeBase> *>> tagToEntities;

        for (auto * entity : entities)
        {
            const std::string & tag = entity->item->getTag();
            if (tag.empty())
            {
                ungrouped.push_back(entity);
            }
            else
            {
                tagToEntities[tag].push_back(entity);
            }
        }

        // Convert map to vector of groups
        for (auto & [tag, groupEntities] : tagToEntities)
        {
            groups.push_back(std::move(groupEntities));
        }
    }

    template <typename T>
    float NodeLayoutEngine::calculateUnitCenterY(const OptimizationUnit<T> & unit)
    {
        if (unit.entities.empty())
            return 0.0f;

        float sumY = 0.0f;
        for (auto * entity : unit.entities)
        {
            sumY += entity->position.y + entity->size.y / 2.0f;
        }
        return sumY / static_cast<float>(unit.entities.size());
    }

    template <typename T>
    float NodeLayoutEngine::calculateOptimalYPosition(
      const OptimizationUnit<T> & unit,
      const std::map<int, std::vector<NodeLayoutEngine::LayoutEntity<T> *>> & allLayers,
      int currentDepth,
      const LayoutConfig & config)
    {
        float totalWeight = 0.0f;
        float weightedSum = 0.0f;

        // Calculate weighted average of connected nodes' positions
        for (auto * entity : unit.entities)
        {
            if constexpr (std::is_same_v<T, nodes::NodeBase>)
            {
                // Track both direct connections and group connections
                // Direct connections have higher weight
                auto directConnections = getNodeConnections(entity->item, allLayers, currentDepth);

                // Process direct connections
                for (auto & [connectedEntity, weight] : directConnections)
                {
                    // Check if the connected entity is part of a group
                    std::string connectedTag;
                    if (connectedEntity->item)
                    {
                        connectedTag = connectedEntity->item->getTag();
                    }

                    // For connections to entities in the same group, increase weight
                    // to keep groups more tightly connected
                    std::string thisTag;
                    if (entity->item)
                    {
                        thisTag = entity->item->getTag();
                    }

                    if (!thisTag.empty() && thisTag == connectedTag)
                    {
                        // Same group connections are weighted higher to keep groups together
                        weight *= 2.0f;
                    }

                    float connectedCenterY =
                      connectedEntity->position.y + connectedEntity->size.y / 2.0f;
                    weightedSum += connectedCenterY * weight;
                    totalWeight += weight;
                }

                // For grouped nodes, also consider connections to other nodes in the same group
                // that might not be directly connected
                std::string entityTag;
                if (entity->item)
                {
                    entityTag = entity->item->getTag();
                }

                if (!entityTag.empty())
                {
                    for (auto & [layerDepth, layerEntities] : allLayers)
                    {
                        if (std::abs(layerDepth - currentDepth) > 1)
                            continue; // Only adjacent layers

                        for (auto * otherEntity : layerEntities)
                        {
                            // Skip entities already processed via direct connections
                            bool alreadyProcessed = false;
                            for (auto & [processedEntity, w] : directConnections)
                            {
                                if (processedEntity == otherEntity)
                                {
                                    alreadyProcessed = true;
                                    break;
                                }
                            }
                            if (alreadyProcessed)
                                continue;

                            // Check if entity is in the same group
                            std::string otherTag;
                            if (otherEntity->item)
                            {
                                otherTag = otherEntity->item->getTag();
                            }

                            if (entityTag == otherTag && !otherTag.empty())
                            {
                                // Add a weaker connection to nodes in the same group
                                float groupWeight = 0.5f;
                                float otherCenterY =
                                  otherEntity->position.y + otherEntity->size.y / 2.0f;
                                weightedSum += otherCenterY * groupWeight;
                                totalWeight += groupWeight;
                            }
                        }
                    }
                }
            }
        }

        if (totalWeight > 0.0f)
        {
            float optimalCenterY = weightedSum / totalWeight;
            // Convert center Y to top Y for the unit
            float unitHeight = unit.getHeight();

            return optimalCenterY - unitHeight / 2.0f;
        }

        return calculateUnitCenterY(unit) - unit.getHeight() / 2.0f;
    }

    template <typename T>
    std::vector<std::pair<NodeLayoutEngine::LayoutEntity<T> *, float>>
    NodeLayoutEngine::getNodeConnections(
      T * node,
      const std::map<int, std::vector<NodeLayoutEngine::LayoutEntity<T> *>> & allLayers,
      int currentDepth)
    {
        std::vector<std::pair<NodeLayoutEngine::LayoutEntity<T> *, float>> connections;

        if constexpr (std::is_same_v<T, nodes::NodeBase>)
        {
            nodes::NodeId nodeId = node->getId();

            // Look in adjacent layers for connected nodes
            for (int deltaDepth : {-1, 1})
            {
                int targetDepth = currentDepth + deltaDepth;
                auto layerIt = allLayers.find(targetDepth);
                if (layerIt == allLayers.end())
                    continue;

                for (auto * candidateEntity : layerIt->second)
                {
                    nodes::NodeId candidateId = candidateEntity->item->getId();
                    bool isConnected = false;
                    float weight = 1.0f;

                    // Check if this node depends on the candidate (incoming edge)
                    for (auto * dep : candidateEntity->dependents)
                    {
                        if (dep->item->getId() == nodeId)
                        {
                            isConnected = true;
                            // Give slightly higher weight to direct connections
                            weight = 1.2f;
                            break;
                        }
                    }

                    // Check if candidate depends on this node (outgoing edge)
                    if (!isConnected)
                    {
                        for (auto * dep : candidateEntity->dependencies)
                        {
                            if (dep->item->getId() == nodeId)
                            {
                                isConnected = true;
                                // Give slightly higher weight to direct connections
                                weight = 1.2f;
                                break;
                            }
                        }
                    }

                    if (isConnected)
                    {
                        connections.emplace_back(candidateEntity, weight);
                    }
                }
            }
        }

        return connections;
    }

    template <typename T>
    void NodeLayoutEngine::moveOptimizationUnit(OptimizationUnit<T> & unit, float targetTopY)
    {
        if (unit.entities.empty())
            return;

        float currentTopY = unit.minY;
        float deltaY = targetTopY - currentTopY;

        // Move all entities in the unit by the same delta
        for (auto * entity : unit.entities)
        {
            entity->position.y += deltaY;
        }

        unit.updateBounds();
    }

    template <typename T>
    void NodeLayoutEngine::resolveLayerOverlaps(std::vector<OptimizationUnit<T>> & units,
                                                const LayoutConfig & config)
    {
        if (units.size() <= 1)
            return;

        // Sort units by their center Y position
        std::sort(units.begin(),
                  units.end(),
                  [](const OptimizationUnit<T> & a, const OptimizationUnit<T> & b)
                  { return a.getCenterY() < b.getCenterY(); });

        // Resolve overlaps by pushing units apart
        for (size_t i = 1; i < units.size(); ++i)
        {
            auto & prevUnit = units[i - 1];
            auto & currentUnit = units[i];

            float minRequiredTop = prevUnit.maxY + config.nodeDistance;
            if (currentUnit.minY < minRequiredTop)
            {
                float pushDistance = minRequiredTop - currentUnit.minY;
                moveOptimizationUnit(currentUnit, currentUnit.minY + pushDistance);
            }
        }
    }

    ImVec2 NodeLayoutEngine::resolveNodeSize(nodes::NodeBase & node) const
    {
        if (m_nodeSizeProvider)
        {
            auto size = m_nodeSizeProvider(node.getId());
            if (size.x > 0.0f && size.y > 0.0f)
            {
                return size;
            }
        }

        return ImVec2(500.0f, 400.0f);
    }

    ImVec2 NodeLayoutEngine::calculateEntitySize(NodeEntity & entity)
    {
        return resolveNodeSize(*entity.item);
    }

    ImVec2 NodeLayoutEngine::calculateGroupSize(const GroupInfo & groupInfo)
    {
        if (groupInfo.nodes.empty())
        {
            return ImVec2(0, 0);
        }

        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float maxY = std::numeric_limits<float>::lowest();

        for (auto * node : groupInfo.nodes)
        {
            auto pos = node->screenPos();
            auto size = resolveNodeSize(*node);

            minX = std::min(minX, pos.x);
            minY = std::min(minY, pos.y);
            maxX = std::max(maxX, pos.x + size.x);
            maxY = std::max(maxY, pos.y + size.y);
        }

        return ImVec2(maxX - minX, maxY - minY);
    }

    void NodeLayoutEngine::updateGroupBounds(GroupInfo & groupInfo)
    {
        if (groupInfo.nodes.empty())
        {
            return;
        }

        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float maxY = std::numeric_limits<float>::lowest();

        for (auto * node : groupInfo.nodes)
        {
            auto pos = node->screenPos();
            auto size = resolveNodeSize(*node);

            minX = std::min(minX, pos.x);
            minY = std::min(minY, pos.y);
            maxX = std::max(maxX, pos.x + size.x);
            maxY = std::max(maxY, pos.y + size.y);
        }

        groupInfo.position = ImVec2(minX, minY);
        groupInfo.size = ImVec2(maxX - minX, maxY - minY);
    } // We'll let the compiler implicitly instantiate the templates as needed

    // ========== OptimizationUnit Template Implementations ==========

    template <typename T>
    NodeLayoutEngine::OptimizationUnit<T>::OptimizationUnit(
      std::vector<NodeLayoutEngine::LayoutEntity<T> *> entities_)
        : entities(std::move(entities_))
    {
        updateBounds();
    }

    template <typename T>
    void NodeLayoutEngine::OptimizationUnit<T>::updateBounds()
    {
        if (entities.empty())
        {
            minY = 0.0f;
            maxY = 0.0f;
            return;
        }

        minY = std::numeric_limits<float>::max();
        maxY = std::numeric_limits<float>::lowest();

        for (auto * entity : entities)
        {
            minY = std::min(minY, entity->position.y);
            maxY = std::max(maxY, entity->position.y + entity->size.y);
        }
    }

    template <typename T>
    float NodeLayoutEngine::OptimizationUnit<T>::getHeight() const
    {
        return maxY - minY;
    }

    template <typename T>
    float NodeLayoutEngine::OptimizationUnit<T>::getCenterY() const
    {
        return (minY + maxY) / 2.0f;
    }

} // namespace gladius::ui
