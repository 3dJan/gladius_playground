/**
 * @file FunctionGraphDeserializer.h
 * @brief Import a minimal JSON function graph (nodes + links) into a nodes::Model
 */

#pragma once

#include <nlohmann/json.hpp>

namespace gladius
{
    namespace nodes
    {
        class Model;
        class Assembly;
    }

    namespace mcp
    {
        /**
         * @brief Applies a minimal graph JSON to a Model (optionally replacing existing graph).
         *
         * Input JSON schema (minimal):
         * - nodes: [ { id: uint, type: string, display_name?: string, position?: [x,y],
         *              values?: { paramName: value, ... } } ]
         * - links: [ { from_node_id, from_port, to_node_id, to_parameter } ]
         *
         * The optional "values" object sets parameter values on the node at creation
         * time. Supports Float, Int, String, Float3 ([x,y,z] or {x,y,z}), ResourceId.
         * For ConstantMatrix, a virtual "matrix" key accepts a flat 16-element array
         * or a 4x4 nested array that gets distributed to m00..m33.
         *
         * Special node types:
         * - "Input"/"Begin" maps to existing model begin node
         * - "Output"/"End" maps to existing model end node
         * - "FunctionCall" creates a function call node referencing another function.
         *   Requires values.resourceid (the ModelResourceID of the function to call)
         *   and an assembly pointer to resolve referenced models.
         *
         * @param model    Target model to modify
         * @param graph    Minimal JSON graph
         * @param replace  If true (default), clear existing graph first
         * @param assembly Optional assembly for resolving FunctionCall references
         * @return JSON with { success: bool, id_map: { clientId -> modelNodeId }, error?: string }
         */
        struct FunctionGraphDeserializer
        {
            static nlohmann::json applyToModel(nodes::Model & model,
                                               nlohmann::json const & graph,
                                               bool replace,
                                               nodes::Assembly * assembly = nullptr);
        };
    } // namespace mcp
} // namespace gladius
