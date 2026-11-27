#include "DesignGraph.h"

#include "../agent/Conversation.h"
#include "InterfaceCompliance.h"

#include <algorithm>
#include <functional>
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

    bool DesignGraph::updateRequirement(std::string const & id,
                                        std::string description,
                                        std::string category,
                                        bool mandatory,
                                        bool trackUserChange)
    {
        auto it = std::find_if(m_requirements.begin(), m_requirements.end(), [&](Requirement const & r) {
            return r.id == id;
        });
        if (it == m_requirements.end())
        {
            return false;
        }
        std::string const prevDesc = it->description;
        std::string const prevCat = it->category;
        bool const prevMand = it->mandatory;

        it->description = std::move(description);
        it->category = std::move(category);
        it->mandatory = mandatory;

        if (trackUserChange
            && (prevDesc != it->description || prevCat != it->category || prevMand != it->mandatory))
        {
            m_pendingRequirementChanges.push_back(
              "Updated requirement " + id + ": [" + it->category
              + (it->mandatory ? ", mandatory" : ", optional") + "] \"" + it->description
              + "\" (was: \"" + prevDesc + "\")");
        }
        return true;
    }

    bool DesignGraph::removeRequirement(std::string const & id, bool trackUserChange)
    {
        auto it = std::find_if(m_requirements.begin(), m_requirements.end(), [&](Requirement const & r) {
            return r.id == id;
        });
        if (it == m_requirements.end())
        {
            return false;
        }
        if (trackUserChange)
        {
            m_pendingRequirementChanges.push_back(
              "Removed requirement " + id + ": \"" + it->description + "\"");
        }
        m_requirements.erase(it);
        return true;
    }

    void DesignGraph::recordUserAddedRequirement(std::string const & id)
    {
        auto it = std::find_if(m_requirements.begin(), m_requirements.end(), [&](Requirement const & r) {
            return r.id == id;
        });
        if (it != m_requirements.end())
        {
            m_pendingRequirementChanges.push_back(
              "Added requirement " + id + ": [" + it->category
              + (it->mandatory ? ", mandatory" : ", optional") + "] \"" + it->description + "\"");
        }
    }

    std::vector<std::string> DesignGraph::consumePendingRequirementChanges()
    {
        std::vector<std::string> out;
        out.swap(m_pendingRequirementChanges);
        return out;
    }

    bool DesignGraph::reopenForFollowUpRequest(std::string const & userRequest)
    {
        if (m_phase != DesignPhase::Complete)
        {
            return false;
        }
        m_phase = DesignPhase::Refine;
        std::string const request = userRequest.empty() ? std::string("see attached images") : userRequest;
        m_pendingRequirementChanges.push_back(
          "New user request for the already completed design: \"" + request
          + "\". Extend the existing requirements and iterate on the design again; do not consider the design finished.");
        return true;
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

    bool DesignGraph::setVariantResource(std::string const & variantId,
                                         uint32_t modelResourceId,
                                         std::string functionName)
    {
        auto it = m_variants.find(variantId);
        if (it == m_variants.end())
        {
            return false;
        }
        it->second.modelResourceId = modelResourceId;
        if (!functionName.empty())
        {
            it->second.functionName = std::move(functionName);
        }
        return true;
    }

    bool DesignGraph::substituteVariant(std::string const & configId,
                                          std::string const & nodeId,
                                          std::string const & newVariantId,
                                          std::string & errorOut)
    {
        auto cfgIt = m_configurations.find(configId);
        if (cfgIt == m_configurations.end())
        {
            errorOut = "Unknown configurationId: " + configId;
            return false;
        }
        auto nodeIt = m_nodes.find(nodeId);
        if (nodeIt == m_nodes.end())
        {
            errorOut = "Unknown nodeId: " + nodeId;
            return false;
        }
        if (nodeIt->second.status == DesignStatus::Frozen)
        {
            errorOut = "Node '" + nodeIt->second.name + "' (" + nodeId + ") is frozen";
            return false;
        }
        auto varIt = m_variants.find(newVariantId);
        if (varIt == m_variants.end())
        {
            errorOut = "Unknown variantId: " + newVariantId;
            return false;
        }
        if (varIt->second.nodeId != nodeId)
        {
            errorOut = "Variant '" + newVariantId + "' belongs to node '" + varIt->second.nodeId + "', not '" + nodeId + "'";
            return false;
        }
        if (varIt->second.status == DesignStatus::Rejected)
        {
            errorOut = "Variant '" + newVariantId + "' is rejected";
            return false;
        }

        // Verify interface compatibility with other selected variants in the configuration.
        for (auto const & [otherNodeId, otherVarId] : cfgIt->second.selectedVariants)
        {
            if (otherNodeId == nodeId)
            {
                continue;
            }
            auto otherVarIt = m_variants.find(otherVarId);
            if (otherVarIt == m_variants.end())
            {
                continue;
            }
            for (auto const & problem : checkInterfacesAgainstConfiguration(varIt->second.interfaces,
                                                                           otherVarIt->second.interfaces))
            {
                errorOut = "Interface mismatch on " + problem.describe() + ": variant '" + newVariantId + "' vs '" +
                           otherVarId + "'";
                return false;
            }
        }

        // A variant may not stick out of the envelope its parent reserved for this node.
        if (auto const * parent = findParentNode(nodeId))
        {
            auto const parentSelection = cfgIt->second.selectedVariants.find(parent->id);
            if (parentSelection != cfgIt->second.selectedVariants.end())
            {
                auto const parentVarIt = m_variants.find(parentSelection->second);
                if (parentVarIt != m_variants.end())
                {
                    auto const containment = checkEnvelopeContainmentBetween(parentVarIt->second.interfaces,
                                                                            varIt->second.interfaces);
                    if (!containment.compatible)
                    {
                        errorOut = "Interface mismatch on " + containment.describe() + ": variant '" + newVariantId +
                                   "' does not fit the envelope reserved by node '" + parent->id + "'";
                        return false;
                    }
                }
            }
        }

        cfgIt->second.selectedVariants[nodeId] = newVariantId;
        return true;
    }

    DesignNode const * DesignGraph::findNode(std::string const & nodeId) const
    {
        auto it = m_nodes.find(nodeId);
        return it != m_nodes.end() ? &it->second : nullptr;
    }

    DesignNode const * DesignGraph::findParentNode(std::string const & nodeId) const
    {
        auto it = std::find_if(m_nodes.begin(), m_nodes.end(),
            [&nodeId](std::pair<std::string, DesignNode> const & node)
            {
                return std::find(node.second.children.begin(), node.second.children.end(), nodeId) !=
                       node.second.children.end();
            });
        return it != m_nodes.end() ? &it->second : nullptr;
    }

    DesignVariant const * DesignGraph::findVariant(std::string const & variantId) const
    {
        auto it = m_variants.find(variantId);
        return it != m_variants.end() ? &it->second : nullptr;
    }

    Configuration const * DesignGraph::findConfiguration(std::string const & configId) const
    {
        auto it = m_configurations.find(configId);
        return it != m_configurations.end() ? &it->second : nullptr;
    }

    std::string DesignGraph::addReferenceImage(std::string prompt,
                                               std::string caption,
                                               std::vector<std::byte> pngBytes,
                                               std::string mimeType,
                                               std::string linkedNodeId,
                                               std::string linkedVariantId)
    {
        std::string const id = "ref_" + std::to_string(m_nextRefImageId++);
        ReferenceImage img;
        img.id = id;
        img.prompt = std::move(prompt);
        img.caption = std::move(caption);
        img.linkedNodeId = std::move(linkedNodeId);
        img.linkedVariantId = std::move(linkedVariantId);
        img.mimeType = mimeType.empty() ? "image/png" : std::move(mimeType);
        img.pngBytes = std::move(pngBytes);
        m_referenceImages.push_back(std::move(img));
        return id;
    }

    bool DesignGraph::removeReferenceImage(std::string const & id)
    {
        auto const it = std::find_if(m_referenceImages.begin(),
                                     m_referenceImages.end(),
                                     [&id](ReferenceImage const & r) { return r.id == id; });
        if (it == m_referenceImages.end())
        {
            return false;
        }
        m_referenceImages.erase(it);
        return true;
    }

    ReferenceImage const * DesignGraph::findReferenceImage(std::string const & id) const
    {
        auto const it = std::find_if(m_referenceImages.begin(),
                                     m_referenceImages.end(),
                                     [&id](ReferenceImage const & r) { return r.id == id; });
        return it != m_referenceImages.end() ? &(*it) : nullptr;
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

    bool DesignGraph::setVariantFeedback(std::string const & variantId, std::string feedback)
    {
        auto it = m_variants.find(variantId);
        if (it == m_variants.end())
        {
            return false;
        }
        it->second.userFeedback = std::move(feedback);
        return true;
    }

    bool DesignGraph::setConfigurationFeedback(std::string const & configId, std::string feedback)
    {
        auto it = m_configurations.find(configId);
        if (it == m_configurations.end())
        {
            return false;
        }
        it->second.userFeedback = std::move(feedback);
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

    std::string DesignGraph::describeSubtree(std::string const & nodeId) const
    {
        auto const node = findNode(nodeId);
        if (!node)
        {
            return "";
        }

        std::ostringstream out;
        // Depth-first with an indent per level: a scoped agent sees its own branch, nothing else.
        std::function<void(DesignNode const &, int)> walk = [&](DesignNode const & n, int depth)
        {
            out << std::string(static_cast<std::size_t>(depth) * 2u, ' ') << "- " << n.id << " "
                << n.name << " (" << toString(n.type) << ", " << toString(n.status);
            if (!n.addressedRequirements.empty())
            {
                out << ", reqs " << n.addressedRequirements.size();
            }
            out << ")\n";
            for (auto const & variantId : n.variantIds)
            {
                if (auto const * variant = findVariant(variantId))
                {
                    out << std::string(static_cast<std::size_t>(depth + 1) * 2u, ' ') << "variant "
                        << variant->id << " " << variant->name << " [" << toString(variant->fidelity)
                        << ", " << toString(variant->status) << "]";
                    if (!variant->functionName.empty())
                    {
                        out << " function=" << variant->functionName;
                    }
                    out << "\n";
                }
            }
            for (auto const & childId : n.children)
            {
                if (auto const * child = findNode(childId))
                {
                    walk(*child, depth + 1);
                }
            }
        };
        walk(*node, 0);
        return out.str();
    }

    std::string DesignGraph::describeInheritedContract(std::string const & nodeId) const
    {
        auto const node = findNode(nodeId);
        if (!node)
        {
            return "";
        }

        std::ostringstream out;
        for (auto const & variantId : node->variantIds)
        {
            auto const * variant = findVariant(variantId);
            if (!variant || variant->interfaces.empty())
            {
                continue;
            }
            out << "Variant " << variant->id << " (" << variant->name << "):\n";
            for (auto const & iface : variant->interfaces)
            {
                out << "  - " << iface.name << " [" << iface.kind << "] = " << iface.specification.dump()
                    << "\n";
            }
        }
        return out.str();
    }

    nlohmann::json DesignGraph::toJson() const
    {
        auto evalToJson = [](Evaluation const & e) {
            return nlohmann::json{
              {"deterministicPassed", e.deterministicPassed},
              {"deterministicWarnings", e.deterministicWarnings},
              {"heuristicNotes", e.heuristicNotes},
              {"subjectiveNotes", e.subjectiveNotes},
              {"pruningScore", e.pruningScore}};
        };

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
                {"type", toString(n.type)},
                {"name", n.name},
                {"status", toString(n.status)},
                {"frozen", n.status == DesignStatus::Frozen},
                {"addressedRequirements", n.addressedRequirements},
                {"variantIds", n.variantIds},
                {"children", n.children}
            });
        }

        nlohmann::json varsArr = nlohmann::json::array();
        for (auto const & [id, v] : m_variants)
        {
            nlohmann::json item = {
                {"id", v.id},
                {"nodeId", v.nodeId},
                {"name", v.name},
                {"rationale", v.rationale},
                {"fidelity", toString(v.fidelity)},
                {"status", toString(v.status)},
                {"rejected", v.status == DesignStatus::Rejected},
                {"parameters", v.parameters},
                {"evaluation", evalToJson(v.evaluation)},
                {"modelResourceId", v.modelResourceId},
                {"functionName", v.functionName},
                {"userFeedback", v.userFeedback}
            };
            if (!v.interfaces.empty())
            {
                nlohmann::json ifaces = nlohmann::json::array();
                for (auto const & iface : v.interfaces)
                {
                    ifaces.push_back({{"id", iface.id},
                                      {"name", iface.name},
                                      {"kind", iface.kind},
                                      {"specification", iface.specification}});
                }
                item["interfaces"] = std::move(ifaces);
            }
            if (!v.previewPng.empty())
            {
                item["previewPngBase64"] = agent::encodeBase64(v.previewPng);
            }
            varsArr.push_back(std::move(item));
        }

        nlohmann::json cfgsArr = nlohmann::json::array();
        for (auto const & [id, c] : m_configurations)
        {
            nlohmann::json item = {
                {"id", c.id},
                {"name", c.name},
                {"active", c.active},
                {"selectedVariants", c.selectedVariants},
                {"evaluation", evalToJson(c.evaluation)},
                {"userFeedback", c.userFeedback}
            };
            if (!c.previewPng.empty())
            {
                item["previewPngBase64"] = agent::encodeBase64(c.previewPng);
            }
            cfgsArr.push_back(std::move(item));
        }

        nlohmann::json refsArr = nlohmann::json::array();
        for (auto const & r : m_referenceImages)
        {
            nlohmann::json item = {
              {"id", r.id},
              {"prompt", r.prompt},
              {"caption", r.caption},
              {"linkedNodeId", r.linkedNodeId},
              {"linkedVariantId", r.linkedVariantId},
              {"mimeType", r.mimeType}};
            if (!r.pngBytes.empty())
            {
                item["pngBase64"] = agent::encodeBase64(r.pngBytes);
            }
            refsArr.push_back(std::move(item));
        }

        return {
            {"phase", toString(m_phase)},
            {"goal", m_goal},
            {"requirements", std::move(reqs)},
            {"nodes", std::move(nodesArr)},
            {"variants", std::move(varsArr)},
            {"configurations", std::move(cfgsArr)},
            {"referenceImages", std::move(refsArr)},
            {"pendingRequirementChanges", m_pendingRequirementChanges},
            {"nextReqId", m_nextReqId},
            {"nextNodeId", m_nextNodeId},
            {"nextVariantId", m_nextVariantId},
            {"nextConfigId", m_nextConfigId},
            {"nextRefImageId", m_nextRefImageId}
        };
    }

    bool DesignGraph::fromJson(nlohmann::json const & json)
    {
        if (!json.is_object())
        {
            return false;
        }
        clear();

        auto evalFromJson = [](nlohmann::json const & ej) {
            Evaluation e;
            if (!ej.is_object())
            {
                return e;
            }
            e.deterministicPassed = ej.value("deterministicPassed", std::vector<std::string>{});
            e.deterministicWarnings = ej.value("deterministicWarnings", std::vector<std::string>{});
            e.heuristicNotes = ej.value("heuristicNotes", std::vector<std::string>{});
            e.subjectiveNotes = ej.value("subjectiveNotes", std::vector<std::string>{});
            e.pruningScore = ej.value("pruningScore", 0.0);
            return e;
        };

        m_phase = designPhaseFromString(json.value("phase", "ClarifyRequirements"));
        m_goal = json.value("goal", std::string{});

        if (json.contains("requirements") && json["requirements"].is_array())
        {
            for (auto const & r : json["requirements"])
            {
                m_requirements.push_back(Requirement{
                  r.value("id", ""),
                  r.value("description", ""),
                  r.value("category", "functional"),
                  r.value("mandatory", true)});
            }
        }

        if (json.contains("nodes") && json["nodes"].is_array())
        {
            for (auto const & n : json["nodes"])
            {
                DesignNode node;
                node.id = n.value("id", "");
                node.type = designNodeTypeFromString(n.value("type", "function"));
                node.name = n.value("name", "");
                if (n.contains("status"))
                {
                    node.status = designStatusFromString(n.value("status", "proposed"));
                }
                else if (n.value("frozen", false))
                {
                    node.status = DesignStatus::Frozen;
                }
                node.addressedRequirements = n.value("addressedRequirements", std::vector<std::string>{});
                node.variantIds = n.value("variantIds", std::vector<std::string>{});
                node.children = n.value("children", std::vector<std::string>{});
                if (!node.id.empty())
                {
                    m_nodes.emplace(node.id, std::move(node));
                }
            }
        }

        if (json.contains("variants") && json["variants"].is_array())
        {
            for (auto const & v : json["variants"])
            {
                DesignVariant var;
                var.id = v.value("id", "");
                var.nodeId = v.value("nodeId", "");
                var.name = v.value("name", "");
                var.rationale = v.value("rationale", "");
                var.fidelity = fidelityLevelFromString(v.value("fidelity", "symbolic"));
                if (v.contains("status"))
                {
                    var.status = designStatusFromString(v.value("status", "proposed"));
                }
                else if (v.value("rejected", false))
                {
                    var.status = DesignStatus::Rejected;
                }
                var.parameters = v.value("parameters", nlohmann::json::object());
                if (v.contains("evaluation"))
                {
                    var.evaluation = evalFromJson(v["evaluation"]);
                }
                var.modelResourceId = v.value("modelResourceId", 0u);
                var.functionName = v.value("functionName", "");
                if (v.contains("interfaces") && v["interfaces"].is_array())
                {
                    for (auto const & ij : v["interfaces"])
                    {
                        var.interfaces.push_back(Interface{ij.value("id", ""),
                                                           ij.value("name", ""),
                                                           ij.value("kind", ""),
                                                           ij.value("specification", nlohmann::json::object())});
                    }
                }
                var.userFeedback = v.value("userFeedback", "");
                if (v.contains("previewPngBase64"))
                {
                    var.previewPng = agent::decodeBase64(v.value("previewPngBase64", ""));
                }
                if (!var.id.empty())
                {
                    m_variants.emplace(var.id, std::move(var));
                }
            }
        }

        if (json.contains("configurations") && json["configurations"].is_array())
        {
            for (auto const & c : json["configurations"])
            {
                Configuration cfg;
                cfg.id = c.value("id", "");
                cfg.name = c.value("name", "");
                cfg.active = c.value("active", false);
                cfg.selectedVariants =
                  c.value("selectedVariants", std::unordered_map<std::string, std::string>{});
                if (c.contains("evaluation"))
                {
                    cfg.evaluation = evalFromJson(c["evaluation"]);
                }
                cfg.userFeedback = c.value("userFeedback", "");
                if (c.contains("previewPngBase64"))
                {
                    cfg.previewPng = agent::decodeBase64(c.value("previewPngBase64", ""));
                }
                if (!cfg.id.empty())
                {
                    m_configurations.emplace(cfg.id, std::move(cfg));
                }
            }
        }

        if (json.contains("referenceImages") && json["referenceImages"].is_array())
        {
            for (auto const & r : json["referenceImages"])
            {
                ReferenceImage img;
                img.id = r.value("id", "");
                img.prompt = r.value("prompt", "");
                img.caption = r.value("caption", "");
                img.linkedNodeId = r.value("linkedNodeId", "");
                img.linkedVariantId = r.value("linkedVariantId", "");
                img.mimeType = r.value("mimeType", "image/png");
                if (r.contains("pngBase64"))
                {
                    img.pngBytes = agent::decodeBase64(r.value("pngBase64", ""));
                }
                if (!img.id.empty())
                {
                    m_referenceImages.push_back(std::move(img));
                }
            }
        }

        m_pendingRequirementChanges =
          json.value("pendingRequirementChanges", std::vector<std::string>{});
        m_nextReqId = json.value("nextReqId", m_requirements.size() + 1);
        m_nextNodeId = json.value("nextNodeId", m_nodes.size() + 1);
        m_nextVariantId = json.value("nextVariantId", m_variants.size() + 1);
        m_nextConfigId = json.value("nextConfigId", m_configurations.size() + 1);
        m_nextRefImageId = json.value("nextRefImageId", m_referenceImages.size() + 1);
        return true;
    }

    std::string DesignGraph::summarize() const
    {
        std::ostringstream out;
        if (!m_goal.empty())
        {
            out << "Goal: " << m_goal << ", ";
        }
        out << "Phase: " << toString(m_phase) << ", Requirements: " << m_requirements.size()
            << ", Nodes: " << m_nodes.size() << ", Variants: " << m_variants.size()
            << ", Configurations: " << m_configurations.size();
        if (auto const active = activeConfiguration())
        {
            out << ", Active: " << active->name << " (" << active->id << ")";
        }
        for (auto const & [cid, cfg] : m_configurations)
        {
            if (!cfg.userFeedback.empty())
            {
                out << "\n  Config " << cid << " (" << cfg.name << ") feedback: " << cfg.userFeedback;
            }
        }
        for (auto const & [vid, var] : m_variants)
        {
            if (!var.userFeedback.empty())
            {
                out << "\n  Variant " << vid << " (" << var.name << ") feedback: " << var.userFeedback;
            }
        }
        return out.str();
    }

    void DesignGraph::clear()
    {
        m_phase = DesignPhase::ClarifyRequirements;
        m_goal.clear();
        m_requirements.clear();
        m_nodes.clear();
        m_variants.clear();
        m_configurations.clear();
        m_referenceImages.clear();
        m_pendingRequirementChanges.clear();
        m_nextReqId = 1;
        m_nextNodeId = 1;
        m_nextVariantId = 1;
        m_nextConfigId = 1;
        m_nextRefImageId = 1;
    }
} // namespace gladius::design
