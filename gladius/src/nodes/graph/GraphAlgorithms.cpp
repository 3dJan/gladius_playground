#include "GraphAlgorithms.h"
#include "Profiling.h"
#include "graph/IDirectedGraph.h"

#include <algorithm>
#include <list>
#include <queue>
#include <sstream>
#include <stack>
#include <unordered_map>
#include <unordered_set>

namespace gladius::nodes::graph
{
    auto determineDirectDependencies(const IDirectedGraph & graph, Identifier id) -> DependencySet
    {
        ProfileFunction;
        DependencySet dependencies;
        if (id < 0 || id > static_cast<int>(graph.getSize()))
        {
            return DependencySet();
        }

        for (auto dep : graph.getVertices())
        {
            auto const value = graph.isDirectlyDependingOn(id, dep);
            if (value && dep != id)
            {
                dependencies.insert(dep);
            }
        }
        return dependencies;
    }

    auto determineAllDependencies(const IDirectedGraph & graph, Identifier id) -> DependencySet
    {
        ProfileFunction;
        if (!graph.isInRange(id))
        {
            return DependencySet();
        }
        auto dependencies = DependencySet{id};

        // Breath First Search
        std::vector<bool> visited(graph.getSize(), false);
        std::queue<Identifier> nodesToVisit;

        nodesToVisit.push(id);
        visited[id] = true;

        while (!nodesToVisit.empty())
        {
            auto nextNode = nodesToVisit.front();
            nodesToVisit.pop();
            dependencies.insert(nextNode);
            for (auto dep : graph.getVertices())
            {
                if (!visited[dep])
                {
                    if (graph.isDirectlyDependingOn(nextNode, dep))
                    {
                        nodesToVisit.push(dep);
                        visited[dep] = true;
                    }
                }
            }
        }

        dependencies.erase(id);
        return dependencies;
    }

    auto graphToString(const IDirectedGraph & graph) -> std::string
    {
        std::stringstream output;
        output << "\n";
        auto const delimiter = "\t";
        output << delimiter << delimiter;
        for (auto col = 0; col < static_cast<Identifier>(graph.getSize()); ++col)
        {
            output << col << delimiter;
        }
        output << "\n" << std::string(120, '_') << "\n";

        for (auto row = 0; row < static_cast<Identifier>(graph.getSize()); ++row)
        {
            output << row << delimiter << "|" << delimiter;
            for (auto col = 0; col < static_cast<Identifier>(graph.getSize()); ++col)
            {
                output << (graph.isDirectlyDependingOn(Identifier(col), Identifier(row)) ? "X"
                                                                                         : " ")
                       << delimiter;
            }
            output << "\n";
        }

        return output.str();
    }

    auto graphToGraphVizStr(const IDirectedGraph & graph) -> std::string
    {
        std::stringstream output;
        output << "digraph G {\n";
        for (auto vertex : graph.getVertices())
        {
            auto dependencies = determineDirectDependencies(graph, vertex);
            for (auto dep : dependencies)
            {
                output << "\t \"" << dep << "\" -> \"" << vertex << "\"\n";
            }
        }
        output << "}\n";
        return output.str();
    }

    auto IsDependingOnImpl(const IDirectedGraph & graph,
                           Identifier id,
                           Identifier dependencyInQuestion) -> bool
    {
        ProfileFunction;

        if (graph.getSize() == 0)
        {
            return false;
        }

        if (id < 0 || dependencyInQuestion < 0)
        {
            return false;
        }
        if (graph.isDirectlyDependingOn(id, dependencyInQuestion))
        {
            return true;
        }

        std::unordered_set<Identifier> visited;
        visited.reserve(graph.getVertices().size());
        std::queue<Identifier> nodesToVisit;

        nodesToVisit.push(id);
        visited.insert(id);

        while (!nodesToVisit.empty())
        {
            auto const nextNode = nodesToVisit.front();
            nodesToVisit.pop();

            if (graph.isDirectlyDependingOn(nextNode, dependencyInQuestion))
            {
                return true;
            }

            for (auto dep : graph.getVertices())
            {
                if (visited.find(dep) == visited.end())
                {
                    if (graph.isDirectlyDependingOn(nextNode, dep))
                    {
                        nodesToVisit.push(dep);
                        visited.insert(dep);
                    }
                }
            }
        }
        return false;
    }

    auto isDependingOn(const IDirectedGraph & graph, Identifier id, Identifier dependencyInQuestion)
      -> bool
    {
        ProfileFunction;
        if (id < 0 || dependencyInQuestion < 0)
        {
            return false;
        }

        if (id == dependencyInQuestion)
        {
            return false;
        }
        return IsDependingOnImpl(graph, id, dependencyInQuestion);
    }

    auto addDependencyIfConflictFree(IDirectedGraph & graph,
                                     Identifier id,
                                     Identifier idOfDependency) -> bool
    {
        ProfileFunction;
        if (id < 0 || idOfDependency < 0)
        {
            return false;
        }
        if (isDependingOn(graph, idOfDependency, id))
        {
            return false;
        }

        graph.addDependency(id, idOfDependency);
        return true;
    }

    auto topologicalSort(const IDirectedGraph & graph) -> VertexList
    {
        ProfileFunction;
        // tsort based on DFS
        enum class NodeType
        {
            CHILD,
            PARENT
        };

        auto const & vertices = graph.getVertices();
        std::size_t const vertexCount = vertices.size();

        // Stack to keep track of the nodes to visit
        std::stack<std::pair<NodeType, Identifier>> nodesToVisit;

        // Use unordered_sets for O(1) membership tests instead of O(N) linear scan
        std::unordered_set<Identifier> visited;
        std::unordered_set<Identifier> inResult;
        visited.reserve(vertexCount);
        inResult.reserve(vertexCount);

        // List of vertices in topological order
        VertexList topologicalOrder;
        topologicalOrder.reserve(vertexCount);

        // Sort vertices by ID so that Begin nodes (lowest IDs, created first in a model)
        // are always processed before disconnected constant nodes (higher IDs).
        // This preserves the code-generator's requirement that Begin is visited first.
        VertexList sortedVertices(vertices.begin(), vertices.end());
        std::sort(sortedVertices.begin(), sortedVertices.end());

        // Loop through all the vertices of the graph
        for (auto id : sortedVertices)
        {
            // If the current vertex is not visited, add it as a child to the stack
            if (visited.find(id) == visited.end())
            {
                nodesToVisit.push({NodeType::CHILD, id});
            }

            // While there are still nodes in the stack to visit
            while (!nodesToVisit.empty())
            {
                // Get the type and id of the top node in the stack
                auto [nodeType, nodeId] = nodesToVisit.top();
                nodesToVisit.pop();

                // Mark the current node as visited
                visited.insert(nodeId);

                // If the node type is parent and it is not already in the topological order,
                // add it to the topological order
                if (nodeType == NodeType::PARENT)
                {
                    if (inResult.find(nodeId) == inResult.end())
                    {
                        topologicalOrder.push_back(nodeId);
                        inResult.insert(nodeId);
                    }
                }
                else // CHILD
                {
                    // Add the node to the stack as a parent
                    nodesToVisit.push({NodeType::PARENT, nodeId});

                    // Check every adjacent vertex — iterate only actual vertices, not ID gaps
                    for (auto dep : vertices)
                    {
                        if (visited.find(dep) == visited.end())
                        {
                            if (graph.isDirectlyDependingOn(nodeId, dep))
                            {
                                nodesToVisit.push({NodeType::CHILD, dep});
                            }
                        }
                    }
                }
            }
        }

        // Return the list of vertices in topological order
        return topologicalOrder;
    }

    auto determineDepth(const IDirectedGraph & graph, Identifier start) -> DepthMap
    {
        ProfileFunction;
        DepthMap result;
        result.reserve(graph.getSize());

        std::list<BfsItem> nodesToVisit;
        std::vector<bool> visited(graph.getSize() + 1u, false);
        visited[start] = true;

        auto const constexpr depth = 0;
        auto currentNode = BfsItem{start, depth};
        nodesToVisit.push_back(currentNode);

        while (!nodesToVisit.empty())
        {
            currentNode = nodesToVisit.front();
            result[currentNode.identifier] =
              std::max(currentNode.depth, result[currentNode.identifier]);
            nodesToVisit.pop_front();

            // Traverse backward through dependencies (predecessors)
            // This ensures we capture all nodes that contribute to the output,
            // including constant nodes and disconnected inputs
            auto const predecessorList = determineDirectDependencies(graph, currentNode.identifier);
            for (auto predecessorId : predecessorList)
            {
                nodesToVisit.push_back({predecessorId, currentNode.depth + 1});
                visited[predecessorId] = true;
            }
        }
        return result;
    }

    auto inDegreeZeroVertices(const IDirectedGraph & graph) -> VertexList
    {
        VertexList InDegreeZeroVertices;
        for (auto id : graph.getVertices())
        {
            if (!graph.hasPredecessors(id))
            {
                InDegreeZeroVertices.push_back(id);
            }
        }
        return InDegreeZeroVertices;
    }

    auto determineSuccessor(const IDirectedGraph & graph, Identifier predecessor) -> VertexList
    {
        VertexList successor;
        for (auto id : graph.getVertices())
        {
            if (graph.isDirectlyDependingOn(id, predecessor))
            {
                successor.push_back(id);
            }
        }
        return successor;
    }

    auto isCyclic(const IDirectedGraph & graph) -> bool
    {
        // Single-pass DFS with 3-color marking (white/gray/black).
        // A back-edge to a gray (in-stack) vertex proves a cycle.
        // O(V+E) with actual vertex iteration — far better than the
        // previous per-vertex BFS approach.
        enum class Color : uint8_t
        {
            White,
            Gray,
            Black
        };

        auto const & vertices = graph.getVertices();
        std::unordered_map<Identifier, Color> color;
        color.reserve(vertices.size());
        for (auto v : vertices)
        {
            color[v] = Color::White;
        }

        // Iterative DFS: pair is (vertex, already-expanded)
        std::stack<std::pair<Identifier, bool>> dfsStack;

        for (auto startVertex : vertices)
        {
            if (color.at(startVertex) != Color::White)
            {
                continue;
            }

            dfsStack.push({startVertex, false});

            while (!dfsStack.empty())
            {
                auto & [v, expanded] = dfsStack.top();

                if (!expanded)
                {
                    expanded = true;
                    color[v] = Color::Gray;

                    for (auto dep : vertices)
                    {
                        if (!graph.isDirectlyDependingOn(v, dep))
                        {
                            continue;
                        }
                        if (color.at(dep) == Color::Gray)
                        {
                            return true; // back-edge → cycle
                        }
                        if (color.at(dep) == Color::White)
                        {
                            dfsStack.push({dep, false});
                        }
                    }
                }
                else
                {
                    color[v] = Color::Black;
                    dfsStack.pop();
                }
            }
        }
        return false;
    }
} // namespace gladius::nodes::graph
