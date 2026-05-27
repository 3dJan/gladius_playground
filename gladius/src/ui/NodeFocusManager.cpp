#include "NodeFocusManager.h"

#include "imguinodeeditor.h"

#include <algorithm>
#include <cmath>

namespace gladius::ui
{

namespace
{

// Helper: Calculate distance squared between two points
float distanceSquared(ImVec2 const & a, ImVec2 const & b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// Helper: Get direction vector sign
int sign(float value) {
    if (value > 0.0f) return 1;
    if (value < 0.0f) return -1;
    return 0;
}

} // anonymous namespace

// ============================================================================
// NodeFocusManager implementation
// ============================================================================

NodeFocusManager::NodeFocusManager() = default;

void NodeFocusManager::updateNodePositions() {
    // Clear existing positions and query from editor context
    m_nodePositions.clear();
    
    ed::EditorContext * editorContext = ed::GetCurrentEditor();
    if (editorContext == nullptr) {
        return;
    }

    // Query all nodes from the editor context
    int nodeCount = ed::GetNodeCount();
    
    if (nodeCount <= 0) {
        return;
    }

    std::vector<ed::NodeId> nodeIds(static_cast<size_t>(nodeCount));
    ed::GetOrderedNodeIds(nodeIds.data(), nodeCount);

    for (int i = 0; i < nodeCount; ++i) {
        ed::NodeId edNodeId = nodeIds[i];
        if (edNodeId == ed::NodeId(0)) {
            continue; // Skip the background/legend node
        }

        nodes::NodeId nodeId = static_cast<nodes::NodeId>(static_cast<uint64_t>(edNodeId));
        ImVec2 pos = getNodePosition(nodeId);
        ImVec2 size = getNodeSize(nodeId);
        
        m_nodePositions.push_back({
            nodeId,
            ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f), // Center
            size
        });
    }
}

void NodeFocusManager::setFocusedNode(nodes::NodeId nodeId) {
    m_focusedNode = nodeId;
    
    // Also update ImGui selection
    ed::EditorContext * editorContext = ed::GetCurrentEditor();
    if (editorContext != nullptr) {
        ed::SelectNode(ed::NodeId(static_cast<uint64_t>(nodeId)));
        ed::NavigateToSelection(true);
    }
}

nodes::NodeId NodeFocusManager::focusedNode() const {
    return m_focusedNode;
}

bool NodeFocusManager::hasFocus() const {
    return m_focusedNode != static_cast<nodes::NodeId>(0);
}

void NodeFocusManager::clearFocus() {
    m_focusedNode = static_cast<nodes::NodeId>(0);
}

void NodeFocusManager::navigateFocus(NavigationDirection dir) {
    nodes::NodeId nextNode = nearestNodeInDirection(m_focusedNode, dir);
    if (nextNode != static_cast<nodes::NodeId>(0)) {
        setFocusedNode(nextNode);
    }
}

void NodeFocusManager::navigateFocus(GamepadState const & gamepad, GamepadActionMap const & actionMap) {
    if (actionMap.isActionPressed(gamepad, GamepadAction::NavigateUp)) {
        navigateFocus(NavigationDirection::Up);
    } else if (actionMap.isActionPressed(gamepad, GamepadAction::NavigateDown)) {
        navigateFocus(NavigationDirection::Down);
    } else if (actionMap.isActionPressed(gamepad, GamepadAction::NavigateLeft)) {
        navigateFocus(NavigationDirection::Left);
    } else if (actionMap.isActionPressed(gamepad, GamepadAction::NavigateRight)) {
        navigateFocus(NavigationDirection::Right);
    }
}

void NodeFocusManager::selectNode(nodes::NodeId nodeId, ed::EditorContext * editorContext, bool additive) {
    if (editorContext == nullptr) {
        return;
    }

    if (!additive) {
        // Clear existing selection
        ed::ClearSelection();
        m_selection.clear();
    }

    // Add to selection
    ed::SelectNode(ed::NodeId(static_cast<uint64_t>(nodeId)), true);
    m_selection.push_back(nodeId);
    
    // Set focus
    m_focusedNode = nodeId;
    
    // Navigate to selection
    ed::NavigateToSelection(true);
}

void NodeFocusManager::deselectNode(nodes::NodeId nodeId, ed::EditorContext * editorContext) {
    if (editorContext == nullptr) {
        return;
    }

    // Deselect in editor context (pass 0 for additive=false)
    ed::SelectNode(ed::NodeId(static_cast<uint64_t>(nodeId)), false);
    
    // Remove from our selection list
    m_selection.erase(
        std::remove(m_selection.begin(), m_selection.end(), nodeId),
        m_selection.end()
    );

    // If we were focusing this node, clear focus
    if (m_focusedNode == nodeId) {
        m_focusedNode = static_cast<nodes::NodeId>(0);
    }
}

void NodeFocusManager::clearSelection(ed::EditorContext * editorContext) {
    if (editorContext == nullptr) {
        return;
    }

    // Deselect all nodes in editor
    for (nodes::NodeId nodeId : m_selection) {
        ed::SelectNode(ed::NodeId(static_cast<uint64_t>(nodeId)), false);
    }
    
    m_selection.clear();
    m_focusedNode = static_cast<nodes::NodeId>(0);
}

void NodeFocusManager::toggleNodeSelection(nodes::NodeId nodeId, ed::EditorContext * editorContext) {
    if (editorContext == nullptr) {
        return;
    }

    if (isNodeSelected(nodeId)) {
        deselectNode(nodeId, editorContext);
    } else {
        selectNode(nodeId, editorContext, true);
    }
}

void NodeFocusManager::selectAll(ed::EditorContext * editorContext) {
    if (editorContext == nullptr) {
        return;
    }

    // Clear current selection
    m_selection.clear();

    // Query all nodes
    int nodeCount = ed::GetNodeCount();
    
    if (nodeCount <= 0) {
        return;
    }

    std::vector<ed::NodeId> nodeIds(static_cast<size_t>(nodeCount));
    ed::GetOrderedNodeIds(nodeIds.data(), nodeCount);

    for (int i = 0; i < nodeCount; ++i) {
        ed::NodeId edNodeId = nodeIds[i];
        if (edNodeId == ed::NodeId(0)) {
            continue; // Skip background/legend node
        }

        nodes::NodeId modelNodeId = static_cast<nodes::NodeId>(static_cast<uint64_t>(edNodeId.Get()));
        ed::SelectNode(edNodeId, true);
        m_selection.push_back(modelNodeId);
    }

    // Set focus to first selected node
    if (!m_selection.empty()) {
        m_focusedNode = m_selection[0];
    }

    ed::NavigateToSelection(true);
}

void NodeFocusManager::deselectAll(ed::EditorContext * editorContext) {
    clearSelection(editorContext);
}

std::vector<nodes::NodeId> NodeFocusManager::selectedNodes() const {
    return m_selection;
}

bool NodeFocusManager::isNodeSelected(nodes::NodeId nodeId) const {
    return std::find(m_selection.begin(), m_selection.end(), nodeId) != m_selection.end();
}

size_t NodeFocusManager::selectionCount() const {
    return m_selection.size();
}

bool NodeFocusManager::hasSelection() const {
    return !m_selection.empty();
}

ImVec2 NodeFocusManager::getNodeCenter(nodes::NodeId nodeId) const {
    for (auto const & np : m_nodePositions) {
        if (np.id == nodeId) {
            return np.center;
        }
    }
    return ImVec2{0, 0};
}

nodes::NodeId NodeFocusManager::nearestNodeInDirection(nodes::NodeId from, NavigationDirection dir) const {
    ImVec2 fromPos;
    
    // Get starting position
    if (from != static_cast<nodes::NodeId>(0)) {
        fromPos = getNodeCenter(from);
    } else {
        // If no focus, use center of editor view (approximate)
        fromPos = ImVec2{0, 0}; // Will be refined by editor context
    }

    if (fromPos.x == 0 && fromPos.y == 0) {
        // Try to find any node as starting point
        if (!m_nodePositions.empty()) {
            fromPos = m_nodePositions[0].center;
        } else {
            return static_cast<nodes::NodeId>(0);
        }
    }

    // Direction vectors
    struct DirectionVector {
        float dx;
        float dy;
        float minDot; // Minimum dot product threshold
    };

    DirectionVector dirVec;
    switch (dir) {
        case NavigationDirection::Up:
            dirVec = {0.0f, -1.0f, -0.5f}; // Allow some horizontal tolerance
            break;
        case NavigationDirection::Down:
            dirVec = {0.0f, 1.0f, -0.5f};
            break;
        case NavigationDirection::Left:
            dirVec = {-1.0f, 0.0f, -0.5f};
            break;
        case NavigationDirection::Right:
            dirVec = {1.0f, 0.0f, -0.5f};
            break;
    }

    nodes::NodeId bestNode{static_cast<nodes::NodeId>(0)};
    float bestScore = std::numeric_limits<float>::max();

    for (auto const & np : m_nodePositions) {
        if (np.id == from) {
            continue; // Don't select the current node
        }

        ImVec2 const toNode = ImVec2{np.center.x - fromPos.x, np.center.y - fromPos.y};
        float const dist = std::sqrt(toNode.x * toNode.x + toNode.y * toNode.y);
        if (dist < 1e-4f)
        {
            continue;
        }
        ImVec2 const toNodeNorm = ImVec2{toNode.x / dist, toNode.y / dist};

        // Check if node is in the right direction
        float const dot = toNodeNorm.x * dirVec.dx + toNodeNorm.y * dirVec.dy;
        if (dot < dirVec.minDot)
        {
            continue; // Not in the right direction
        }

        // Calculate perpendicular distance (how aligned is it with the direction)
        float const perpDist = std::abs(toNodeNorm.x * dirVec.dy - toNodeNorm.y * dirVec.dx);

        // Score: lower is better (closer to direction, closer in distance)
        float const score = dist + perpDist * 50.0f;

        if (score < bestScore) {
            bestScore = score;
            bestNode = np.id;
        }
    }

    return bestNode;
}

ImVec2 NodeFocusManager::getNodePosition(nodes::NodeId nodeId) const
{
    return ed::GetNodePosition(ed::NodeId(static_cast<uint64_t>(nodeId)));
}

ImVec2 NodeFocusManager::getNodeSize(nodes::NodeId nodeId) const
{
    return ed::GetNodeSize(ed::NodeId(static_cast<uint64_t>(nodeId)));
}

// ============================================================================
// Utility functions
// ============================================================================

std::string navigationDirectionToString(NavigationDirection dir) {
    switch (dir) {
        case NavigationDirection::Up:     return "Up";
        case NavigationDirection::Down:   return "Down";
        case NavigationDirection::Left:   return "Left";
        case NavigationDirection::Right:  return "Right";
        default:                          return "Unknown";
    }
}

} // namespace gladius::ui
