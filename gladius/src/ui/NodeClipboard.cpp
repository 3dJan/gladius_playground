#include "NodeClipboard.h"

#include "nodes/NodeBase.h"
#include "nodes/Port.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace gladius::ui
{

    bool NodeClipboard::hasContent() const
    {
        return static_cast<bool>(m_clipboardModel);
    }

    void NodeClipboard::copyNodes(nodes::Model & sourceModel,
                                  std::set<nodes::NodeId> const & selectedNodeIds)
    {
        if (selectedNodeIds.empty())
        {
            return;
        }

        m_clipboardModel = std::make_unique<nodes::Model>();

        std::unordered_map<nodes::NodeId, nodes::NodeBase *> cloneMap;

        // Clone nodes
        for (auto const & [id, nodePtr] : sourceModel)
        {
            if (!nodePtr || selectedNodeIds.find(id) == selectedNodeIds.end())
            {
                continue;
            }
            auto cloned = nodePtr->clone();
            cloned->screenPos() = nodePtr->screenPos();
            auto * inserted = m_clipboardModel->insert(std::move(cloned));
            cloneMap[id] = inserted;
        }

        // Recreate intra-selection links
        for (auto const & [origId, clonedNode] : cloneMap)
        {
            (void) clonedNode;
            auto origOpt = sourceModel.getNode(origId);
            if (!origOpt.has_value())
            {
                continue;
            }
            nodes::NodeBase const * origNode = origOpt.value();
            for (auto const & [paramName, param] : origNode->constParameter())
            {
                if (!param.getConstSource().has_value())
                {
                    continue;
                }
                auto const & src = param.getConstSource().value();
                nodes::Port const * srcPort = sourceModel.getPort(src.portId);
                if (!srcPort)
                {
                    continue;
                }
                nodes::NodeId const srcNodeId = srcPort->getParentId();
                if (cloneMap.find(srcNodeId) == cloneMap.end())
                {
                    continue;
                }

                nodes::Port * clonedSrcPort =
                  cloneMap[srcNodeId]->findOutputPort(srcPort->getShortName());
                nodes::VariantParameter * clonedTarget = cloneMap[origId]->getParameter(paramName);
                if (clonedSrcPort && clonedTarget)
                {
                    m_clipboardModel->addLink(clonedSrcPort->getId(), clonedTarget->getId(), true);
                }
            }
        }
    }

    std::unordered_map<std::string, nodes::NodeBase *>
    NodeClipboard::pasteNodes(nodes::Model & targetModel, ImVec2 canvasPosition)
    {
        std::unordered_map<std::string, nodes::NodeBase *> pastedMap;

        if (!m_clipboardModel)
        {
            return pastedMap;
        }

        // Calculate bounding box center of clipboard nodes
        bool first = true;
        ImVec2 minPos{0, 0};
        ImVec2 maxPos{0, 0};
        for (auto const & [id, nodePtr] : *m_clipboardModel)
        {
            (void) id;
            if (!nodePtr)
            {
                continue;
            }
            ImVec2 p{nodePtr->screenPos().x, nodePtr->screenPos().y};
            if (first)
            {
                minPos = maxPos = p;
                first = false;
            }
            else
            {
                minPos.x = std::min(minPos.x, p.x);
                minPos.y = std::min(minPos.y, p.y);
                maxPos.x = std::max(maxPos.x, p.x);
                maxPos.y = std::max(maxPos.y, p.y);
            }
        }

        ImVec2 const center{(minPos.x + maxPos.x) * 0.5f, (minPos.y + maxPos.y) * 0.5f};
        ImVec2 const delta{canvasPosition.x - center.x, canvasPosition.y - center.y};

        // Clone and insert nodes into target model
        for (auto const & [id, nodePtr] : *m_clipboardModel)
        {
            (void) id;
            if (!nodePtr)
            {
                continue;
            }
            auto cloned = nodePtr->clone();
            cloned->screenPos().x = nodePtr->screenPos().x + delta.x;
            cloned->screenPos().y = nodePtr->screenPos().y + delta.y;
            nodes::NodeBase * inserted = targetModel.insert(std::move(cloned));
            pastedMap[nodePtr->getUniqueName()] = inserted;
        }

        // Build lookup for clipboard nodes by unique name
        std::unordered_map<std::string, nodes::NodeBase const *> clipboardByName;
        for (auto const & [id, nodePtr] : *m_clipboardModel)
        {
            (void) id;
            if (nodePtr)
            {
                clipboardByName[nodePtr->getUniqueName()] = nodePtr.get();
            }
        }

        // Recreate links in target model
        for (auto const & [origName, newNode] : pastedMap)
        {
            auto it = clipboardByName.find(origName);
            if (it == clipboardByName.end())
            {
                continue;
            }
            nodes::NodeBase const * origNode = it->second;
            for (auto const & [paramName, param] : origNode->constParameter())
            {
                if (!param.getConstSource().has_value())
                {
                    continue;
                }
                auto const & src = param.getConstSource().value();
                nodes::Port const * origSrcPort = m_clipboardModel->getPort(src.portId);
                if (!origSrcPort)
                {
                    continue;
                }
                std::string const srcNodeUnique = origSrcPort->getParent()->getUniqueName();
                auto pastedSrcIt = pastedMap.find(srcNodeUnique);
                if (pastedSrcIt == pastedMap.end())
                {
                    continue;
                }
                nodes::Port * newSrcPort =
                  pastedSrcIt->second->findOutputPort(origSrcPort->getShortName());
                nodes::VariantParameter * newTarget = newNode->getParameter(paramName);
                if (newSrcPort && newTarget)
                {
                    targetModel.addLink(newSrcPort->getId(), newTarget->getId(), true);
                }
            }
        }

        return pastedMap;
    }

    void NodeClipboard::updatePastePosition(ImVec2 canvasPosition)
    {
        m_lastPasteCanvasPos = canvasPosition;
        m_hadLastPastePos = true;
    }

    ImVec2 NodeClipboard::getAdjustedPastePosition(ImVec2 mouseCanvasPos)
    {
        ImVec2 canvas = mouseCanvasPos;

        if (m_hadLastPastePos && std::abs(canvas.x - m_lastPasteCanvasPos.x) < 1.0f &&
            std::abs(canvas.y - m_lastPasteCanvasPos.y) < 1.0f)
        {
            ++m_consecutivePasteCount;
            canvas.x += m_pasteOffsetStep * static_cast<float>(m_consecutivePasteCount % 5);
            canvas.y += m_pasteOffsetStep * static_cast<float>(m_consecutivePasteCount % 5);
        }
        else
        {
            m_consecutivePasteCount = 0;
        }

        return canvas;
    }

    void NodeClipboard::clear()
    {
        m_clipboardModel.reset();
        m_hadLastPastePos = false;
        m_consecutivePasteCount = 0;
    }

} // namespace gladius::ui
