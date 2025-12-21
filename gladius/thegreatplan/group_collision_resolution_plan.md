# Group Collision Resolution Plan

## Problem Statement
Current layout attempts to prevent overlaps during group placement, but this causes groups to be pushed to extreme positions. Instead, we should:
1. Layout each group independently (ignoring other groups)
2. Treat ungrouped nodes as a single "pseudo-group"
3. Detect collisions between all group rectangles
4. Resolve collisions by moving entire groups in 4 directions (left/right/top/bottom)
5. Choose the direction with the best layout metrics
6. Position Begin/End nodes optimally after group layout

## Current Implementation Issues
- `layoutGroupsBalancedGrid` tries to place groups in a grid while avoiding overlaps
- Overlap detection happens during placement, which pushes groups far apart
- Ungrouped nodes are laid out separately and can be pushed to extreme Y positions
- Begin/End nodes are treated specially but not positioned optimally

## New Algorithm Overview

### Phase 1: Independent Group Layout
**Goal**: Layout each group's internal nodes without considering other groups

For each group:
1. Call `layoutNodesInGroup(group, config, {})` with empty `occupiedRects`
2. This allows each group to optimize its internal layout freely
3. Call `updateGroupBounds(group)` to get the final bounding box

For ungrouped nodes:
1. Create a pseudo-group containing all ungrouped nodes
2. Layout these nodes together as if they were a group
3. Compute the bounding box for this pseudo-group

**Result**: All groups (including pseudo-group) have optimized internal layouts with known bounding boxes

### Phase 2: Collision Detection
**Goal**: Identify which groups overlap with each other

1. Create a list of all group rectangles (including pseudo-group)
2. For each pair of groups, check if their bounding boxes overlap using `Rect::overlaps()`
3. Build a collision graph: `std::map<int, std::vector<int>>` where key is group index, value is list of overlapping group indices

**Result**: We know which groups collide with which other groups

### Phase 3: Collision Resolution
**Goal**: Move groups to eliminate overlaps while optimizing layout quality

#### Strategy Selection
For each colliding group pair (groupA, groupB):
1. **Try 4 movement directions**:
   - Move groupB **left** of groupA: `newX = groupA.position.x - groupB.size.x - groupPadding`
   - Move groupB **right** of groupA: `newX = groupA.position.x + groupA.size.x + groupPadding`
   - Move groupB **above** groupA: `newY = groupA.position.y - groupB.size.y - groupPadding`
   - Move groupB **below** groupA: `newY = groupA.position.y + groupA.size.y + groupPadding`

2. **Evaluate each option** using layout metrics:
   - Edge crossings (primary metric)
   - Total layout area (bounding box)
   - Average edge length
   - Vertical spread
   - Compute a combined score similar to `LayoutMetrics::computeScore()`

3. **Choose best option**: Select the direction with the lowest score

4. **Apply movement**: Move all nodes in groupB by the delta

5. **Re-check collisions**: After moving, check if groupB now collides with other groups

#### Iterative Resolution
Since moving one group might create new collisions:
1. Iterate through collision pairs in order (e.g., sorted by group depth)
2. Resolve each collision with the 4-direction strategy
3. After each resolution, update the collision graph
4. Continue until no collisions remain or max iterations reached (e.g., 100)

#### Collision Order Heuristics
To minimize total movements:
- **Depth-first**: Resolve collisions for groups with smaller minDepth first (left-to-right flow)
- **Size-first**: Resolve collisions involving larger groups first (less movement overall)
- **Collision-count-first**: Resolve groups with most collisions first (biggest bottlenecks)

**Initial approach**: Use depth-first to maintain left-to-right flow

### Phase 4: Begin/End Node Positioning
**Goal**: Place Begin and End nodes optimally after all groups are positioned

#### Begin Node
1. Find the leftmost X position among all nodes: `minX = min(node.position.x for all nodes)`
2. Find the vertical center of all nodes: `centerY = (minY + maxY) / 2.0`
3. Position Begin node at: `(minX - beginNodeWidth - nodePadding, centerY - beginNodeHeight/2.0)`

#### End Node
1. Find the rightmost X position: `maxX = max(node.position.x + node.size.x for all nodes)`
2. Use the same `centerY` as Begin node
3. Position End node at: `(maxX + nodePadding, centerY - endNodeHeight/2.0)`

**Result**: Begin and End nodes are horizontally aligned and vertically centered

## Implementation Plan

### Step 1: Refactor `performAutoLayoutVariant`
**Current flow** (lines 920-970):
```cpp
// Layout nodes inside each group
for (auto & group : groups) {
    layoutNodesInGroup(group, config, occupiedRects);
}

// Layout groups relative to each other
layoutGroups(groups, config);

// Update group bounds and add to occupiedRects
for (auto & group : groups) {
    updateGroupBounds(group);
    occupiedRects.push_back(Rect(...));
}

// Layout ungrouped nodes
layoutUngroupedNodes(..., occupiedRects);
```

**New flow**:
```cpp
// Phase 1: Independent group layout
for (auto & group : groups) {
    layoutNodesInGroup(group, config, {}); // Empty occupiedRects
    updateGroupBounds(group);
}

// Create pseudo-group for ungrouped nodes
GroupInfo ungroupedPseudoGroup = createPseudoGroupForUngroupedNodes(ungroupedNodes);
layoutNodesInGroup(ungroupedPseudoGroup, config, {});
updateGroupBounds(ungroupedPseudoGroup);

// Add pseudo-group to groups list
std::vector<GroupInfo> allGroups = groups;
allGroups.push_back(ungroupedPseudoGroup);

// Phase 2: Detect collisions
auto collisionGraph = detectGroupCollisions(allGroups);

// Phase 3: Resolve collisions
resolveGroupCollisions(allGroups, collisionGraph, config);

// Phase 4: Position Begin/End nodes
positionBeginEndNodes(allGroups, config);

// Apply post-processing (balancing, compacting, etc.)
if (applyPostProcessing) {
    // ... existing post-processing
}
```

### Step 2: Implement Helper Functions

#### `createPseudoGroupForUngroupedNodes`
```cpp
GroupInfo NodeLayoutEngine::createPseudoGroupForUngroupedNodes(
    std::vector<NodeProxy*> const & ungroupedNodes)
{
    GroupInfo pseudoGroup;
    pseudoGroup.nodes = ungroupedNodes;
    pseudoGroup.position = ImVec2(0, 0);
    pseudoGroup.size = ImVec2(0, 0);
    pseudoGroup.minDepth = computeMinDepth(ungroupedNodes);
    // ... initialize other fields
    return pseudoGroup;
}
```

#### `detectGroupCollisions`
```cpp
std::map<int, std::vector<int>> NodeLayoutEngine::detectGroupCollisions(
    std::vector<GroupInfo> const & groups)
{
    std::map<int, std::vector<int>> collisionGraph;
    
    for (size_t i = 0; i < groups.size(); ++i) {
        Rect rectA(groups[i].position, 
                   ImVec2(groups[i].position.x + groups[i].size.x,
                          groups[i].position.y + groups[i].size.y));
        
        for (size_t j = i + 1; j < groups.size(); ++j) {
            Rect rectB(groups[j].position,
                      ImVec2(groups[j].position.x + groups[j].size.x,
                             groups[j].position.y + groups[j].size.y));
            
            if (rectA.overlaps(rectB)) {
                collisionGraph[i].push_back(j);
                collisionGraph[j].push_back(i);
            }
        }
    }
    
    return collisionGraph;
}
```

#### `resolveGroupCollisions`
```cpp
void NodeLayoutEngine::resolveGroupCollisions(
    std::vector<GroupInfo> & groups,
    std::map<int, std::vector<int>> & collisionGraph,
    LayoutConfig const & config)
{
    int maxIterations = 100;
    int iteration = 0;
    
    while (!collisionGraph.empty() && iteration < maxIterations) {
        // Sort collision pairs by depth (left-to-right priority)
        std::vector<std::pair<int, int>> collisionPairs;
        for (auto const & [groupIdx, colliders] : collisionGraph) {
            for (int collidingIdx : colliders) {
                if (groupIdx < collidingIdx) { // Avoid duplicates
                    collisionPairs.emplace_back(groupIdx, collidingIdx);
                }
            }
        }
        
        std::sort(collisionPairs.begin(), collisionPairs.end(),
            [&groups](auto const & a, auto const & b) {
                return groups[a.first].minDepth < groups[b.first].minDepth;
            });
        
        if (collisionPairs.empty()) break;
        
        // Resolve first collision
        auto [idxA, idxB] = collisionPairs[0];
        resolveSingleCollision(groups, idxA, idxB, config);
        
        // Rebuild collision graph
        collisionGraph = detectGroupCollisions(groups);
        
        ++iteration;
    }
}
```

#### `resolveSingleCollision`
```cpp
void NodeLayoutEngine::resolveSingleCollision(
    std::vector<GroupInfo> & groups,
    int idxA,
    int idxB,
    LayoutConfig const & config)
{
    auto & groupA = groups[idxA];
    auto & groupB = groups[idxB];
    
    // Try 4 directions and evaluate each
    struct Option {
        ImVec2 newPosition;
        float score;
    };
    
    std::vector<Option> options;
    
    // Left
    options.push_back({
        ImVec2(groupA.position.x - groupB.size.x - config.groupPadding,
               groupB.position.y),
        evaluateGroupPosition(groups, idxB, 
            ImVec2(groupA.position.x - groupB.size.x - config.groupPadding,
                   groupB.position.y))
    });
    
    // Right
    options.push_back({
        ImVec2(groupA.position.x + groupA.size.x + config.groupPadding,
               groupB.position.y),
        evaluateGroupPosition(groups, idxB,
            ImVec2(groupA.position.x + groupA.size.x + config.groupPadding,
                   groupB.position.y))
    });
    
    // Top
    options.push_back({
        ImVec2(groupB.position.x,
               groupA.position.y - groupB.size.y - config.groupPadding),
        evaluateGroupPosition(groups, idxB,
            ImVec2(groupB.position.x,
                   groupA.position.y - groupB.size.y - config.groupPadding))
    });
    
    // Bottom
    options.push_back({
        ImVec2(groupB.position.x,
               groupA.position.y + groupA.size.y + config.groupPadding),
        evaluateGroupPosition(groups, idxB,
            ImVec2(groupB.position.x,
                   groupA.position.y + groupA.size.y + config.groupPadding))
    });
    
    // Choose best option
    auto bestOption = std::min_element(options.begin(), options.end(),
        [](Option const & a, Option const & b) {
            return a.score < b.score;
        });
    
    // Apply movement
    moveGroup(groupB, bestOption->newPosition);
}
```

#### `evaluateGroupPosition`
```cpp
float NodeLayoutEngine::evaluateGroupPosition(
    std::vector<GroupInfo> const & groups,
    int groupIdx,
    ImVec2 proposedPosition) const
{
    // Temporarily move the group to proposed position
    // Calculate layout metrics:
    // - Edge crossings (use existing detectEdgeCrossings)
    // - Total layout area (bounding box of all nodes)
    // - Average edge length
    // - Vertical spread
    
    // Return a weighted score similar to LayoutMetrics::computeScore()
    // Lower score = better layout
    
    float const crossings = detectEdgeCrossingsForConfiguration(...);
    float const area = computeTotalArea(groups);
    float const avgEdgeLength = computeAverageEdgeLength(groups);
    
    return crossings * CROSSING_WEIGHT + area * AREA_WEIGHT + avgEdgeLength * EDGE_WEIGHT;
}
```

#### `positionBeginEndNodes`
```cpp
void NodeLayoutEngine::positionBeginEndNodes(
    std::vector<GroupInfo> const & groups,
    LayoutConfig const & config)
{
    // Find bounding box of all nodes
    float minX = FLT_MAX, maxX = -FLT_MAX;
    float minY = FLT_MAX, maxY = -FLT_MAX;
    
    for (auto const & group : groups) {
        for (auto * node : group.nodes) {
            minX = std::min(minX, node->screenPos().x);
            maxX = std::max(maxX, node->screenPos().x + node->size().x);
            minY = std::min(minY, node->screenPos().y);
            maxY = std::max(maxY, node->screenPos().y + node->size().y);
        }
    }
    
    float const centerY = (minY + maxY) / 2.0F;
    
    // Position Begin node
    if (m_beginNode) {
        float const beginX = minX - m_beginNode->size().x - config.nodeDistance;
        float const beginY = centerY - m_beginNode->size().y / 2.0F;
        m_beginNode->setScreenPos(ImVec2(beginX, beginY));
        if (m_nodePositionWriter) {
            m_nodePositionWriter(m_beginNode->getId(), ImVec2(beginX, beginY));
        }
    }
    
    // Position End node
    if (m_endNode) {
        float const endX = maxX + config.nodeDistance;
        float const endY = centerY - m_endNode->size().y / 2.0F;
        m_endNode->setScreenPos(ImVec2(endX, endY));
        if (m_nodePositionWriter) {
            m_nodePositionWriter(m_endNode->getId(), ImVec2(endX, endY));
        }
    }
}
```

### Step 3: Update `layoutNodesInGroup`
Currently this function might use `occupiedRects` for internal collision avoidance. We need to ensure it can work with an empty `occupiedRects` list:

- Review lines 470-530 to see how `occupiedRects` is used
- If it's used for internal group layout, that's fine
- If it's used to avoid other groups, we need to make it optional

### Step 4: Remove Old Group Layout Methods
After implementing the new approach, we can remove or deprecate:
- `layoutGroupsBalancedGrid` (lines 715-780)
- `layoutGroupsStacked` (lines 570-610)
- `layoutGroupsRow` (if exists)
- The `layoutGroups` switch statement (lines 550-568)

These will be replaced by the collision resolution approach.

### Step 5: Testing Strategy
1. **Unit tests**: Test each helper function independently
   - `createPseudoGroupForUngroupedNodes`: Verify correct bounds and depth
   - `detectGroupCollisions`: Test with overlapping and non-overlapping groups
   - `resolveSingleCollision`: Verify best option is chosen
   - `positionBeginEndNodes`: Verify alignment and centering

2. **Integration tests**: Test full layout flow
   - Simple 2-group collision
   - Multiple group collisions (chain reaction)
   - Groups with different depths
   - Large number of ungrouped nodes

3. **Visual validation**: Compare before/after screenshots
   - Check `groupOverlaps` metric drops to 0
   - Verify Begin/End nodes are aligned
   - Confirm no extreme Y positions

## Expected Outcomes
1. **Zero group overlaps**: All groups properly separated
2. **Optimal Begin/End placement**: Horizontally aligned, vertically centered
3. **Better layout quality**: Metrics-driven collision resolution
4. **Predictable behavior**: No extreme positions or infinite loops
5. **Cleaner code**: Single collision resolution algorithm replaces multiple layout modes

## Potential Issues & Solutions

### Issue 1: Circular Dependencies
Groups A, B, C might form a circular collision pattern where resolving A-B creates B-C collision.

**Solution**: Iterative approach with max iterations limit. If not resolved in 100 iterations, log warning and accept best-effort result.

### Issue 2: Evaluation Performance
Evaluating 4 options for each collision might be expensive if `detectEdgeCrossings` is slow.

**Solution**: 
- Cache edge list and only recompute crossings for moved group's edges
- Use approximate metrics (bounding box distance) for initial filtering
- Optimize crossing detection algorithm

### Issue 3: Pseudo-group Fragmentation
Ungrouped nodes might be scattered, making their pseudo-group bounding box huge.

**Solution**: 
- Pre-cluster ungrouped nodes by spatial proximity before creating pseudo-group
- Or treat each ungrouped node as its own mini-group
- Or use existing `layoutUngroupedNodes` but wrap result in pseudo-group

### Issue 4: Begin/End Node Edges
Begin/End nodes might be positioned far from their connected nodes, creating very long edges.

**Solution**:
- Include edge length to Begin/End nodes in evaluation metric
- Adjust Begin/End position within a reasonable range to minimize edge lengths
- Consider Begin/End node positions during collision resolution

## Implementation Order
1. ✅ Create this plan document
2. ⏳ Implement helper functions (`createPseudoGroupForUngroupedNodes`, `detectGroupCollisions`, etc.)
3. ⏳ Implement collision resolution (`resolveGroupCollisions`, `resolveSingleCollision`, `evaluateGroupPosition`)
4. ⏳ Implement Begin/End positioning (`positionBeginEndNodes`)
5. ⏳ Refactor `performAutoLayoutVariant` to use new flow
6. ⏳ Add unit tests for each component
7. ⏳ Test with real layouts and compare metrics
8. ⏳ Clean up old code (remove obsolete functions)

## Success Criteria
- [ ] `groupOverlaps` metric = 0 for all test cases
- [ ] Begin/End nodes have same Y coordinate
- [ ] No nodes at extreme positions (e.g., Y > 10000)
- [ ] Layout metrics comparable or better than current implementation
- [ ] All 495 existing tests still pass
- [ ] Visual inspection shows clean, organized layouts
