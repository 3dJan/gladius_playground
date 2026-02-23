/**
 * @file FunctionGraphDeserializer.cpp
 */

#include "FunctionGraphDeserializer.h"

#include "../nodes/Assembly.h"
#include "../nodes/DerivedNodes.h"
#include "../nodes/Model.h"
#include "../nodes/NodeFactory.h"
#include "../nodes/Parameter.h"
#include "../nodes/Port.h"
#include "../nodes/nodesfwd.h"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace gladius
{
    namespace mcp
    {
        using nlohmann::json;

        /// @see FunctionGraphDeserializer::applyNodeValues
        void FunctionGraphDeserializer::applyNodeValues(nodes::NodeBase & node, json const & values)
        {
            for (auto const & [paramName, paramValue] : values.items())
            {
                // Virtual "matrix" key for ConstantMatrix batch-set
                if (paramName == "matrix" && node.name() == "ConstantMatrix")
                {
                    if (paramValue.is_array() && paramValue.size() == 16)
                    {
                        for (int i = 0; i < 4; ++i)
                            for (int j = 0; j < 4; ++j)
                            {
                                auto * p = node.getParameter(fmt::format("m{}{}", i, j));
                                if (p)
                                    p->setValue(nodes::VariantType{
                                      static_cast<float>(paramValue[i * 4 + j].get<double>())});
                            }
                    }
                    else if (paramValue.is_array() && paramValue.size() == 4 &&
                             paramValue[0].is_array())
                    {
                        for (int i = 0; i < 4; ++i)
                            for (int j = 0; j < 4; ++j)
                            {
                                auto * p = node.getParameter(fmt::format("m{}{}", i, j));
                                if (p)
                                    p->setValue(nodes::VariantType{
                                      static_cast<float>(paramValue[i][j].get<double>())});
                            }
                    }
                    continue;
                }

                auto * param = node.getParameter(paramName);
                if (!param)
                    continue;

                auto typeIdx = param->getTypeIndex();
                if (typeIdx == nodes::ParameterTypeIndex::Float && paramValue.is_number())
                {
                    param->setValue(
                      nodes::VariantType{static_cast<float>(paramValue.get<double>())});
                }
                else if (typeIdx == nodes::ParameterTypeIndex::Int && paramValue.is_number())
                {
                    param->setValue(
                      nodes::VariantType{static_cast<int>(paramValue.get<long long>())});
                }
                else if (typeIdx == nodes::ParameterTypeIndex::String && paramValue.is_string())
                {
                    param->setValue(nodes::VariantType{paramValue.get<std::string>()});
                }
                else if (typeIdx == nodes::ParameterTypeIndex::Float3)
                {
                    nodes::float3 v{};
                    if (paramValue.is_array() && paramValue.size() == 3)
                    {
                        v.x = static_cast<float>(paramValue[0].get<double>());
                        v.y = static_cast<float>(paramValue[1].get<double>());
                        v.z = static_cast<float>(paramValue[2].get<double>());
                    }
                    else if (paramValue.is_object())
                    {
                        v.x = static_cast<float>(paramValue.value("x", 0.0));
                        v.y = static_cast<float>(paramValue.value("y", 0.0));
                        v.z = static_cast<float>(paramValue.value("z", 0.0));
                    }
                    param->setValue(nodes::VariantType{v});
                }
                else if (typeIdx == nodes::ParameterTypeIndex::ResourceId &&
                         paramValue.is_number_integer())
                {
                    param->setValue(nodes::VariantType{
                      static_cast<gladius::ResourceId>(paramValue.get<long long>())});
                }
            }
        }

        json FunctionGraphDeserializer::applyToModel(nodes::Model & model,
                                                     json const & graph,
                                                     bool replace,
                                                     nodes::Assembly * assembly)
        {
            // Validate input
            if (!graph.is_object())
            {
                return json{{"success", false}, {"error", "graph must be a JSON object"}};
            }
            if (!graph.contains("nodes") || !graph["nodes"].is_array())
            {
                return json{{"success", false}, {"error", "graph.nodes must be an array"}};
            }

            // Optionally clear existing graph
            if (replace)
            {
                model.clear();
                model.createBeginEndWithDefaultInAndOuts();
            }

            // Build mapping from client ids to actual NodeBase*
            std::unordered_map<uint32_t, nodes::NodeBase *> idMap;

            // Keep handles to Begin/End for special mapping
            auto * beginNode = model.getBeginNode();
            auto * endNode = model.getEndNode();

            // Track whether incoming graph provides meaningful layout positions
            bool anyPosition = false;
            bool allNearOrigin = true;
            float minX = std::numeric_limits<float>::max();
            float minY = std::numeric_limits<float>::max();
            float maxX = -std::numeric_limits<float>::max();
            float maxY = -std::numeric_limits<float>::max();

            // First pass: create nodes
            for (const auto & jn : graph["nodes"])
            {
                if (!jn.is_object())
                    continue;

                uint32_t clientId = jn.value("id", 0u);
                std::string type = jn.value("type", "");
                std::string displayName = jn.value("display_name", "");
                nodes::NodeBase * created = nullptr;

                // Map special types to existing begin/end
                if (type == "Input" || type == "Begin")
                {
                    created = beginNode;
                    if (!displayName.empty())
                        created->setDisplayName(displayName);
                }
                else if (type == "Output" || type == "End")
                {
                    created = endNode;
                    if (!displayName.empty())
                        created->setDisplayName(displayName);
                }
                else if (type == "FunctionCall")
                {
                    // FunctionCall needs a referenced function (resourceid in values)
                    if (!assembly)
                    {
                        return json{
                          {"success", false},
                          {"error",
                           "FunctionCall nodes require an assembly context. "
                           "Ensure the graph is applied via set_function_graph."}};
                    }

                    ResourceId refId = 0;
                    if (jn.contains("values") && jn["values"].contains("resourceid"))
                    {
                        refId = static_cast<ResourceId>(
                          jn["values"]["resourceid"].get<long long>());
                    }
                    if (refId == 0)
                    {
                        return json{
                          {"success", false},
                          {"error",
                           "FunctionCall node (id=" + std::to_string(clientId) +
                             ") requires values.resourceid"}};
                    }

                    auto refModel = assembly->findModel(refId);
                    if (!refModel)
                    {
                        return json{
                          {"success", false},
                          {"error",
                           "Referenced function (resourceid=" + std::to_string(refId) +
                             ") not found in assembly"}};
                    }

                    created = model.createFunctionCallNode(refId, *refModel);
                    if (!displayName.empty())
                        created->setDisplayName(displayName);
                }
                else
                {
                    auto newNode = nodes::NodeFactory::createNode(type);
                    if (!newNode)
                    {
                        return json{{"success", false},
                                    {"error", std::string("Unknown node type: ") + type}};
                    }
                    if (!displayName.empty())
                        newNode->setDisplayName(displayName);
                    created = model.insert(std::move(newNode));
                }

                if (jn.contains("position") && jn["position"].is_array() &&
                    jn["position"].size() == 2)
                {
                    auto pos = const_cast<nodes::NodeBase *>(created)->screenPos();
                    pos.x = static_cast<float>(jn["position"][0].get<double>());
                    pos.y = static_cast<float>(jn["position"][1].get<double>());

                    anyPosition = true;
                    allNearOrigin = allNearOrigin && (std::abs(pos.x) < 1e-3f) && (std::abs(pos.y) < 1e-3f);
                    minX = std::min(minX, pos.x);
                    minY = std::min(minY, pos.y);
                    maxX = std::max(maxX, pos.x);
                    maxY = std::max(maxY, pos.y);
                }

                if (clientId != 0 && created)
                {
                    idMap[clientId] = created;
                }

                // Apply initial parameter values from "values" object
                if (created && jn.contains("values") && jn["values"].is_object())
                {
                    FunctionGraphDeserializer::applyNodeValues(*created, jn["values"]);
                }
            }

            // Update graph/ports prior to linking
            model.updateGraphAndOrderIfNeeded();

            // Second pass: create links
            if (graph.contains("links") && graph["links"].is_array())
            {
                for (const auto & jl : graph["links"])
                {
                    if (!jl.is_object())
                        continue;
                    uint32_t fromNodeId = jl.value("from_node_id", 0u);
                    uint32_t toNodeId = jl.value("to_node_id", 0u);
                    std::string fromPort = jl.value("from_port", "");
                    std::string toParam = jl.value("to_parameter", "");

                    if (!fromNodeId || !toNodeId || fromPort.empty() || toParam.empty())
                        continue;

                    auto itFrom = idMap.find(fromNodeId);
                    auto itTo = idMap.find(toNodeId);
                    if (itFrom == idMap.end() || itTo == idMap.end())
                        continue;

                    nodes::NodeBase * srcNode = itFrom->second;
                    nodes::NodeBase * dstNode = itTo->second;
                    auto * port = srcNode->findOutputPort(fromPort);
                    auto * param = dstNode->getParameter(toParam);
                    if (!port || !param)
                        continue;

                    // Ensure port has parent id set and ids are valid
                    model.registerOutput(*port);
                    model.registerInput(*param);
                    model.addLink(port->getId(), param->getId());
                }
            }

            // Finalize
            model.updateGraphAndOrderIfNeeded();

            if (anyPosition)
            {
                float const spanX = maxX - minX;
                float const spanY = maxY - minY;
                bool const degenerateSpan = (std::abs(spanX) < 1e-3f) && (std::abs(spanY) < 1e-3f);
                if (!(allNearOrigin || degenerateSpan))
                {
                    model.markAsLayouted();
                }
            }

            json out;
            out["success"] = true;
            json jmap = json::object();
            for (const auto & [cid, node] : idMap)
            {
                jmap[std::to_string(cid)] = node->getId();
            }
            out["id_map"] = jmap;
            return out;
        }
    } // namespace mcp
} // namespace gladius
