#include "DesignGraph.h"

#include <sstream>

namespace gladius::design
{
    std::string DesignGraph::addRequirement(std::string description,
                                            std::string category,
                                            bool mandatory)
    {
        std::string const id = "req_" + std::to_string(m_nextReqId++);
        m_requirements.push_back(Requirement{id, std::move(description), std::move(category), mandatory});
        return id;
    }

    std::string DesignGraph::addNode(DesignNodeType type,
                                     std::string name,
                                     std::string const & parentNodeId,
                                     std::vector<std::string> addressedRequirements)
    {
        std::string const id = "node_" + std::to_string(m_nextNodeId++);
        DesignNode node;
        node.id = id;
        node.type = type;
        node.name = std::move(name);
        node.addressedRequirements = std::move(addressedRequirements);
        m_nodes.emplace(id, std::move(node));

        if (!parentNodeId.empty())
        {
            auto it = m_nodes.find(parentNodeId);
            if (it != m_nodes.end())
            {
                it->second.children.push_back(id);
            }
        }
        return id;
    }

    std::string DesignGraph::createVariant(std::string const & nodeId,
                                           std::string name,
                                           std::string rationale,
                                           FidelityLevel fidelity,
                                           nlohmann::json parameters)
    {
        auto nodeIt = m_nodes.find(nodeId);
        if (nodeIt == m_nodes.end())
        {
            return "";
        }

        std::string const id = "var_" + std::to_string(m_nextVariantId++);
        DesignVariant variant;
        variant.id = id;
        variant.nodeId = nodeId;
        variant.name = std::move(name);
        variant.rationale = std::move(rationale);
        variant.fidelity = fidelity;
        variant.parameters = std::move(parameters);

        m_variants.emplace(id, std::move(variant));
        nodeIt->second.variantIds.push_back(id);
        return id;
    }

    bool DesignGraph::rejectVariant(std::string const & variantId, std::string const & reason)
    {
        auto it = m_variants.find(variantId);
        if (it == m_variants.end())
        {
            return false;
        }
        it->second.status = DesignStatus::Rejected;
        if (!reason.empty())
        {
            it->second.evaluation.heuristicNotes.push_back("Rejected: " + reason);
        }
        return true;
    }

    bool DesignGraph::freezeNode(std::string const & nodeId)
    {
        auto it = m_nodes.find(nodeId);
        if (it == m_nodes.end())
        {
            return false;
        }
        it->second.status = DesignStatus::Frozen;
        return true;
    }

    bool DesignGraph::unfreezeNode(std::string const & nodeId)
    {
        auto it = m_nodes.find(nodeId);
        if (it == m_nodes.end())
        {
            return false;
        }
        it->second.status = DesignStatus::Active;
        return true;
    }

    bool DesignGraph::isNodeFrozen(std::string const & nodeId) const
    {
        auto it = m_nodes.find(nodeId);
        return it != m_nodes.end() && it->second.status == DesignStatus::Frozen;
    }

    bool DesignGraph::defineInterface(std::string const & variantId, Interface iface)
    {
        auto it = m_variants.find(variantId);
        if (it == m_variants.end())
        {
            return false;
        }
        it->second.interfaces.push_back(std::move(iface));
        return true;
    }

    bool DesignGraph::setVariantEvaluation(std::string const & variantId, Evaluation eval)
    {
        auto it = m_variants.find(variantId);
        if (it == m_variants.end())
        {
            return false;
        }
        it->second.evaluation = std::move(eval);
        return true;
    }

    bool DesignGraph::setVariantPreview(std::string const & variantId, std::vector<std::byte> pngBytes)
    {
        auto it = m_variants.find(variantId);
        if (it == m_variants.end())
        {
            return false;
        }
        it->second.previewPng = std::move(pngBytes);
        return true;
    }

    bool DesignGraph::setConfigurationPreview(std::string const & configId, std::vector<std::byte> pngBytes)
    {
        auto it = m_configurations.find(configId);
        if (it == m_configurations.end())
        {
            return false;
        }
        it->second.previewPng = std::move(pngBytes);
        return true;
    }

    std::string DesignGraph::createConfiguration(std::string name,
                                                 std::unordered_map<std::string, std::string> selectedVariants)
    {
        std::string const id = "cfg_" + std::to_string(m_nextConfigId++);
        Configuration cfg;
        cfg.id = id;
        cfg.name = std::move(name);
        cfg.selectedVariants = std::move(selectedVariants);
        m_configurations.emplace(id, std::move(cfg));
        return id;
    }

    bool DesignGraph::activateConfiguration(std::string const & configId)
    {
        auto targetIt = m_configurations.find(configId);
        if (targetIt == m_configurations.end())
        {
            return false;
        }

        // Preserve frozen nodes from currently active configuration
        std::unordered_map<std::string, std::string> frozenSelections;
        for (auto & [cid, cfg] : m_configurations)
        {
            if (cfg.active)
            {
                for (auto const & [nid, vid] : cfg.selectedVariants)
                {
                    if (isNodeFrozen(nid))
                    {
                        frozenSelections[nid] = vid;
                    }
                }
            }
            cfg.active = false;
        }

        for (auto const & [nid, vid] : frozenSelections)
        {
            targetIt->second.selectedVariants[nid] = vid;
        }
        targetIt->second.active = true;
        return true;
    }

    std::optional<std::string>
    DesignGraph::hybridizeConfigurations(std::string const & newName,
                                         std::string const & baseConfigId,
                                         std::string const & overrideConfigId,
                                         std::vector<std::string> const & overrideNodeIds)
    {
        auto baseIt = m_configurations.find(baseConfigId);
        auto overIt = m_configurations.find(overrideConfigId);
        if (baseIt == m_configurations.end() || overIt == m_configurations.end())
        {
            return std::nullopt;
        }

        auto merged = baseIt->second.selectedVariants;
        for (auto const & nid : overrideNodeIds)
        {
            if (isNodeFrozen(nid))
            {
                continue;
            }
            auto selIt = overIt->second.selectedVariants.find(nid);
            if (selIt != overIt->second.selectedVariants.end())
            {
                merged[nid] = selIt->second;
            }
        }
        return createConfiguration(newName, std::move(merged));
    }

    std::optional<Configuration> DesignGraph::activeConfiguration() const
    {
        for (auto const & [id, cfg] : m_configurations)
        {
            if (cfg.active)
            {
                return cfg;
            }
        }
        return std::nullopt;
    }

    nlohmann::json DesignGraph::toJson() const
    {
        nlohmann::json reqs = nlohmann::json::array();
        for (auto const & r : m_requirements)
        {
            reqs.push_back({{"id", r.id}, {"description", r.description}, {"category", r.category}, {"mandatory", r.mandatory}});
        }

        nlohmann::json nodesArr = nlohmann::json::array();
        for (auto const & [id, n] : m_nodes)
        {
            nodesArr.push_back({
                {"id", n.id},
                {"name", n.name},
                {"frozen", n.status == DesignStatus::Frozen},
                {"variantIds", n.variantIds},
                {"children", n.children}
            });
        }

        nlohmann::json varsArr = nlohmann::json::array();
        for (auto const & [id, v] : m_variants)
        {
            varsArr.push_back({
                {"id", v.id},
                {"nodeId", v.nodeId},
                {"name", v.name},
                {"fidelity", toString(v.fidelity)},
                {"rejected", v.status == DesignStatus::Rejected},
                {"parameters", v.parameters}
            });
        }

        nlohmann::json cfgsArr = nlohmann::json::array();
        for (auto const & [id, c] : m_configurations)
        {
            cfgsArr.push_back({
                {"id", c.id},
                {"name", c.name},
                {"active", c.active},
                {"selectedVariants", c.selectedVariants}
            });
        }

        return {
            {"phase", toString(m_phase)},
            {"requirements", std::move(reqs)},
            {"nodes", std::move(nodesArr)},
            {"variants", std::move(varsArr)},
            {"configurations", std::move(cfgsArr)}
        };
    }

    std::string DesignGraph::summarize() const
    {
        std::ostringstream out;
        out << "Phase: " << toString(m_phase) << ", Requirements: " << m_requirements.size()
            << ", Nodes: " << m_nodes.size() << ", Variants: " << m_variants.size()
            << ", Configurations: " << m_configurations.size();
        if (auto const active = activeConfiguration())
        {
            out << ", Active: " << active->name << " (" << active->id << ")";
        }
        return out.str();
    }

    void DesignGraph::clear()
    {
        m_phase = DesignPhase::ClarifyRequirements;
        m_requirements.clear();
        m_nodes.clear();
        m_variants.clear();
        m_configurations.clear();
        m_nextReqId = 1;
        m_nextNodeId = 1;
        m_nextVariantId = 1;
        m_nextConfigId = 1;
    }
} // namespace gladius::design
