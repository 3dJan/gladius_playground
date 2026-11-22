#include "GraphOptimizer.h"

#include "DerivedNodes.h"
#include "Model.h"
#include "Parameter.h"
#include "Port.h"
#include "Visitor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <queue>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gladius::nodes
{
    namespace
    {
        struct InputKey
        {
            bool isWired{false};
            PortId sourcePortId{0};
            std::uint8_t valueKind{0u}; // 1=float, 2=float3, 3=int, 4=string
            std::array<std::uint32_t, 3> bits{0u, 0u, 0u};
            std::string strValue;

            bool operator==(InputKey const &) const = default;
            auto operator<=>(InputKey const &) const = default;
        };

        struct NodeKey
        {
            std::type_index type{typeid(void)};
            std::vector<InputKey> inputs;

            bool operator==(NodeKey const & other) const
            {
                return type == other.type && inputs == other.inputs;
            }
        };

        struct NodeKeyHash
        {
            std::size_t operator()(NodeKey const & key) const noexcept
            {
                std::size_t h = key.type.hash_code();
                auto const mix = [&h](std::size_t const v) noexcept
                {
                    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6u) + (h >> 2u);
                };
                for (auto const & in : key.inputs)
                {
                    mix(static_cast<std::size_t>(in.isWired));
                    mix(static_cast<std::size_t>(in.sourcePortId));
                    mix(static_cast<std::size_t>(in.valueKind));
                    mix(in.bits[0]);
                    mix(in.bits[1]);
                    mix(in.bits[2]);
                    if (!in.strValue.empty())
                    {
                        mix(std::hash<std::string>{}(in.strValue));
                    }
                }
                return h;
            }
        };

        [[nodiscard]] std::uint32_t floatBits(float const v) noexcept
        {
            std::uint32_t bits = 0u;
            // Canonicalize -0.0f to +0.0f for hashing only where exact equality holds; keep distinct
            // sign bit for strict bit-for-bit equivalence.
            std::memcpy(&bits, &v, sizeof(bits));
            return bits;
        }

        [[nodiscard]] bool isPureCseCandidate(NodeBase const & node) noexcept
        {
            auto const ti = std::type_index(typeid(node));
            return ti == typeid(Addition) || ti == typeid(Multiplication) ||
                   ti == typeid(Subtraction) || ti == typeid(Division) ||
                   ti == typeid(Min) || ti == typeid(Max) || ti == typeid(Abs) ||
                   ti == typeid(Sqrt) || ti == typeid(Pow) || ti == typeid(Mix) ||
                   ti == typeid(Clamp) || ti == typeid(Sine) || ti == typeid(Cosine) ||
                   ti == typeid(Tangent) || ti == typeid(ArcSin) || ti == typeid(ArcCos) ||
                   ti == typeid(ArcTan) || ti == typeid(ArcTan2) || ti == typeid(Fmod) ||
                   ti == typeid(Mod) || ti == typeid(Exp) || ti == typeid(Log) ||
                   ti == typeid(Log2) || ti == typeid(Log10) || ti == typeid(Select) ||
                   ti == typeid(SinH) || ti == typeid(CosH) || ti == typeid(TanH) ||
                   ti == typeid(Round) || ti == typeid(Ceil) || ti == typeid(Floor) ||
                   ti == typeid(Sign) || ti == typeid(Fract) || ti == typeid(Length) ||
                   ti == typeid(DotProduct) || ti == typeid(CrossProduct) ||
                   ti == typeid(ComposeVector) || ti == typeid(DecomposeVector) ||
                   ti == typeid(VectorFromScalar) || ti == typeid(ConstantScalar) ||
                   ti == typeid(ConstantVector);
        }

        [[nodiscard]] bool isCommutativeBinaryOp(NodeBase const & node) noexcept
        {
            auto const ti = std::type_index(typeid(node));
            return ti == typeid(Addition) || ti == typeid(Multiplication) ||
                   ti == typeid(Min) || ti == typeid(Max) || ti == typeid(DotProduct);
        }

        [[nodiscard]] std::optional<InputKey> buildInputKey(VariantParameter const & param)
        {
            if (auto const & src = param.getConstSource(); src.has_value())
            {
                return InputKey{.isWired = true, .sourcePortId = src->portId};
            }
            // Never merge distinct modifiable parameters.
            if (param.isModifiable())
            {
                return std::nullopt;
            }
            // Parameters with a resolved origin are addressable runtime parameters; merging
            // them would collapse their value buffer destinations.
            if (param.getOrigin().has_value())
            {
                return std::nullopt;
            }
            auto const & val = param.getValue();
            if (auto const * f = std::get_if<float>(&val))
            {
                return InputKey{.isWired = false, .valueKind = 1u, .bits = {floatBits(*f), 0u, 0u}};
            }
            if (auto const * v3 = std::get_if<float3>(&val))
            {
                return InputKey{.isWired = false,
                                .valueKind = 2u,
                                .bits = {floatBits(v3->x), floatBits(v3->y), floatBits(v3->z)}};
            }
            if (auto const * i = std::get_if<int>(&val))
            {
                return InputKey{
                  .isWired = false, .valueKind = 3u, .bits = {static_cast<std::uint32_t>(*i), 0u, 0u}};
            }
            if (auto const * s = std::get_if<std::string>(&val))
            {
                return InputKey{.isWired = false, .valueKind = 4u, .strValue = *s};
            }
            return std::nullopt;
        }

        /// Returns the exact non-modifiable float constant represented by `param`
        /// (either directly unwired or fed by a non-modifiable ConstantScalar node).
        [[nodiscard]] std::optional<float> getExactImmutableFloat(Model const & model,
                                                                  VariantParameter const & param)
        {
            if (auto const & src = param.getConstSource(); src.has_value())
            {
                auto const srcNodeOpt = model.getNode(src->nodeId);
                if (!srcNodeOpt.has_value() || *srcNodeOpt == nullptr)
                {
                    return std::nullopt;
                }
                auto const * constScalar = dynamic_cast<ConstantScalar const *>(*srcNodeOpt);
                if (constScalar == nullptr)
                {
                    return std::nullopt;
                }
                auto const & params = constScalar->constParameter();
                auto const it = params.find(FieldNames::Value);
                if (it == params.end() || it->second.isModifiable() ||
                    it->second.getConstSource().has_value())
                {
                    return std::nullopt;
                }
                auto const val = it->second.getValue();
                if (auto const * f = std::get_if<float>(&val))
                {
                    return *f;
                }
                return std::nullopt;
            }
            if (param.isModifiable())
            {
                return std::nullopt;
            }
            auto const val = param.getValue();
            if (auto const * f = std::get_if<float>(&val))
            {
                return *f;
            }
            return std::nullopt;
        }

        /// Rewires all downstream parameters consuming `oldPortId` to consume `newPortId`.
        void rewirePortConsumers(Model & model, PortId const oldPortId, PortId const newPortId)
        {
            if (oldPortId == newPortId)
            {
                return;
            }
            std::vector<ParameterId> consumers;
            for (auto & [nodeId, nodePtr] : model)
            {
                if (!nodePtr)
                {
                    continue;
                }
                for (auto & [paramName, param] : nodePtr->parameter())
                {
                    auto const & src = param.getConstSource();
                    if (src.has_value() && src->portId == oldPortId)
                    {
                        consumers.push_back(param.getId());
                    }
                }
            }
            for (auto const consumerParamId : consumers)
            {
                (void) model.addLink(newPortId, consumerParamId, true);
            }
        }

        /// Collects node IDs in dependency order using Model::visitNodes.
        class OrderCollectorVisitor final : public Visitor
        {
          public:
            void visit(NodeBase & node) override { order.push_back(node.getId()); }
            std::vector<NodeId> order;
        };

        [[nodiscard]] std::vector<NodeId> collectNodeOrder(Model & model)
        {
            OrderCollectorVisitor collector;
            model.visitNodes(collector);
            return collector.order;
        }
    }

    std::size_t GraphOptimizer::applyPeepholeRewrites(Model & model)
    {
        std::size_t rewrites = 0u;
        auto const order = collectNodeOrder(model);

        for (auto const nodeId : order)
        {
            auto const nodeOpt = model.getNode(nodeId);
            if (!nodeOpt.has_value() || *nodeOpt == nullptr)
            {
                continue;
            }
            NodeBase * const node = *nodeOpt;
            if (auto * powNode = dynamic_cast<Pow *>(node); powNode != nullptr)
            {
                auto const outIt = powNode->getOutputs().find(FieldNames::Value);
                auto const & params = powNode->constParameter();
                auto const itBase = params.find(FieldNames::Base);
                auto const itExp = params.find(FieldNames::Exponent);
                if (outIt != powNode->getOutputs().end() && itBase != params.end() &&
                    itExp != params.end())
                {
                    auto const valExp = getExactImmutableFloat(model, itExp->second);
                    if (valExp.has_value() && *valExp == 2.0f &&
                        itBase->second.getConstSource().has_value())
                    {
                        auto const srcPortId = itBase->second.getConstSource()->portId;
                        auto * mulNode = model.create<Multiplication>();
                        if (mulNode != nullptr)
                        {
                            auto const paramAId = mulNode->parameter().at(FieldNames::A).getId();
                            auto const paramBId = mulNode->parameter().at(FieldNames::B).getId();
                            (void) model.addLink(srcPortId, paramAId, true);
                            (void) model.addLink(srcPortId, paramBId, true);
                            auto const mulResultId =
                              mulNode->getOutputs().at(FieldNames::Result).getId();
                            rewirePortConsumers(model, outIt->second.getId(), mulResultId);
                            ++rewrites;
                        }
                    }
                }
                continue;
            }

            auto const outIt = node->getOutputs().find(FieldNames::Result);
            if (outIt == node->getOutputs().end())
            {
                continue;
            }
            PortId const resultPortId = outIt->second.getId();

            auto const & params = node->constParameter();
            auto const itA = params.find(FieldNames::A);
            auto const itB = params.find(FieldNames::B);
            if (itA == params.end() || itB == params.end())
            {
                continue;
            }

            if (dynamic_cast<Addition *>(node) != nullptr)
            {
                auto const valA = getExactImmutableFloat(model, itA->second);
                auto const valB = getExactImmutableFloat(model, itB->second);
                if (valB.has_value() && *valB == 0.0f && itA->second.getConstSource().has_value())
                {
                    rewirePortConsumers(model, resultPortId, itA->second.getConstSource()->portId);
                    ++rewrites;
                }
                else if (valA.has_value() && *valA == 0.0f &&
                         itB->second.getConstSource().has_value())
                {
                    rewirePortConsumers(model, resultPortId, itB->second.getConstSource()->portId);
                    ++rewrites;
                }
            }
            else if (dynamic_cast<Subtraction *>(node) != nullptr)
            {
                auto const valB = getExactImmutableFloat(model, itB->second);
                if (valB.has_value() && *valB == 0.0f && itA->second.getConstSource().has_value())
                {
                    rewirePortConsumers(model, resultPortId, itA->second.getConstSource()->portId);
                    ++rewrites;
                }
            }
            else if (dynamic_cast<Multiplication *>(node) != nullptr)
            {
                auto const valA = getExactImmutableFloat(model, itA->second);
                auto const valB = getExactImmutableFloat(model, itB->second);
                if (valB.has_value() && *valB == 1.0f && itA->second.getConstSource().has_value())
                {
                    rewirePortConsumers(model, resultPortId, itA->second.getConstSource()->portId);
                    ++rewrites;
                }
                else if (valA.has_value() && *valA == 1.0f &&
                         itB->second.getConstSource().has_value())
                {
                    rewirePortConsumers(model, resultPortId, itB->second.getConstSource()->portId);
                    ++rewrites;
                }
            }
            else if (dynamic_cast<Division *>(node) != nullptr)
            {
                auto const valB = getExactImmutableFloat(model, itB->second);
                if (valB.has_value() && *valB == 1.0f && itA->second.getConstSource().has_value())
                {
                    rewirePortConsumers(model, resultPortId, itA->second.getConstSource()->portId);
                    ++rewrites;
                }
            }
        }

        return rewrites;
    }

    std::size_t GraphOptimizer::eliminateCommonSubexpressions(Model & model)
    {
        std::size_t deduplicated = 0u;
        std::unordered_map<NodeKey, NodeId, NodeKeyHash> canonicalByKey;
        auto const order = collectNodeOrder(model);

        for (auto const nodeId : order)
        {
            auto const nodeOpt = model.getNode(nodeId);
            if (!nodeOpt.has_value() || *nodeOpt == nullptr)
            {
                continue;
            }
            NodeBase * const node = *nodeOpt;
            if (!isPureCseCandidate(*node))
            {
                continue;
            }

            NodeKey key;
            key.type = std::type_index(typeid(*node));
            bool buildOk = true;

            auto const & params = node->constParameter();
            key.inputs.reserve(params.size());
            // std::map orders parameters deterministically by name.
            for (auto const & [paramName, param] : params)
            {
                (void) paramName;
                auto inputKey = buildInputKey(param);
                if (!inputKey.has_value())
                {
                    buildOk = false;
                    break;
                }
                key.inputs.push_back(std::move(*inputKey));
            }
            if (!buildOk)
            {
                continue;
            }

            if (isCommutativeBinaryOp(*node) && key.inputs.size() == 2u &&
                key.inputs[1] < key.inputs[0])
            {
                std::swap(key.inputs[0], key.inputs[1]);
            }

            auto const [it, inserted] = canonicalByKey.emplace(std::move(key), nodeId);
            if (!inserted && it->second != nodeId)
            {
                auto const canonicalOpt = model.getNode(it->second);
                if (!canonicalOpt.has_value() || *canonicalOpt == nullptr)
                {
                    continue;
                }
                NodeBase * const canonical = *canonicalOpt;
                for (auto const & [portName, dupPort] : node->getOutputs())
                {
                    auto const canonPortIt = canonical->getOutputs().find(portName);
                    if (canonPortIt != canonical->getOutputs().end())
                    {
                        rewirePortConsumers(model, dupPort.getId(), canonPortIt->second.getId());
                    }
                }
                ++deduplicated;
            }
        }

        return deduplicated;
    }

    std::size_t GraphOptimizer::eliminateDeadCode(Model & model)
    {
        auto * const endNode = model.getEndNode();
        auto * const beginNode = model.getBeginNode();
        if (endNode == nullptr)
        {
            return 0u;
        }

        std::unordered_set<NodeId> reachable;
        std::queue<NodeId> frontier;
        reachable.insert(endNode->getId());
        frontier.push(endNode->getId());
        if (beginNode != nullptr)
        {
            reachable.insert(beginNode->getId());
        }

        while (!frontier.empty())
        {
            auto const currentId = frontier.front();
            frontier.pop();
            auto const nodeOpt = model.getNode(currentId);
            if (!nodeOpt.has_value() || *nodeOpt == nullptr)
            {
                continue;
            }
            for (auto const & [paramName, param] : (*nodeOpt)->constParameter())
            {
                (void) paramName;
                if (auto const & src = param.getConstSource(); src.has_value())
                {
                    if (reachable.insert(src->nodeId).second)
                    {
                        frontier.push(src->nodeId);
                    }
                }
            }
        }

        std::vector<NodeId> deadNodes;
        for (auto const & [nodeId, nodePtr] : model)
        {
            if (nodePtr != nullptr && !reachable.contains(nodeId))
            {
                deadNodes.push_back(nodeId);
            }
        }

        for (auto const deadId : deadNodes)
        {
            model.remove(deadId);
        }

        return deadNodes.size();
    }

    GraphOptimizationStats GraphOptimizer::optimize(Model & model)
    {
        GraphOptimizationStats stats;
        stats.initialNodeCount = model.getSize();
        stats.peepholeRewrites = applyPeepholeRewrites(model);
        stats.deduplicatedNodes = eliminateCommonSubexpressions(model);
        stats.prunedDeadNodes = eliminateDeadCode(model);
        model.updateGraphAndOrderIfNeeded();
        stats.finalNodeCount = model.getSize();
        return stats;
    }
}
