#include "FunctionDeduplicator.h"
#include "DerivedNodes.h"
#include "FunctionalEquality.h"
#include <algorithm>
#include <unordered_map>

namespace gladius::nodes
{
    std::vector<DuplicateGroup> FunctionDeduplicator::findDuplicateGroups(
        Assembly const & assembly)
    {
        auto const & functions = assembly.getFunctions();
        auto const assemblyModelId = assembly.getAssemblyModelId();

        // Build hash → [resourceIds] map for quick lookup
        std::unordered_map<size_t, std::vector<ResourceId>> hashGroups;

        for (auto const & [id, model] : functions)
        {
            // Skip assembly model
            if (id == assemblyModelId)
            {
                continue;
            }

            if (model)
            {
                auto const hash = FunctionalEquality::computeHash(*model);
                hashGroups[hash].push_back(id);
            }
        }

        // Now verify actual equality for groups with same hash (handle collisions)
        std::vector<DuplicateGroup> result;

        for (auto const & [hash, ids] : hashGroups)
        {
            if (ids.size() < 2)
            {
                continue;
            }

            // For each ID with this hash, find truly equal functions
            std::vector<bool> processed(ids.size(), false);

            for (size_t i = 0; i < ids.size(); ++i)
            {
                if (processed[i])
                {
                    continue;
                }

                DuplicateGroup group;
                group.members.push_back(ids[i]);
                processed[i] = true;

                auto const modelI = assembly.getFunctions().at(ids[i]);

                for (size_t j = i + 1; j < ids.size(); ++j)
                {
                    if (processed[j])
                    {
                        continue;
                    }

                    auto const modelJ = assembly.getFunctions().at(ids[j]);

                    if (FunctionalEquality::areEqual(*modelI, *modelJ))
                    {
                        group.members.push_back(ids[j]);
                        processed[j] = true;
                    }
                }

                // Only add group if there are actual duplicates (2+ members)
                if (group.members.size() >= 2)
                {
                    result.push_back(std::move(group));
                }
            }
        }

        return result;
    }

    DeduplicationResult FunctionDeduplicator::deduplicate(Assembly & assembly)
    {
        DeduplicationResult result;

        // Find all duplicate groups
        result.groups = findDuplicateGroups(assembly);

        // Process each group
        for (auto & group : result.groups)
        {
            // Select canonical (the one to keep)
            group.canonical = selectCanonical(group, assembly);

            // Update references and remove duplicates
            for (auto const memberId : group.members)
            {
                if (memberId == group.canonical)
                {
                    continue; // Don't remove the canonical
                }

                // Update all references to point to canonical
                result.updatedReferences +=
                    updateInternalReferences(assembly, memberId, group.canonical);

                // Remove the duplicate
                assembly.deleteModel(memberId);
                ++result.removedCount;
            }
        }

        return result;
    }

    ResourceId FunctionDeduplicator::selectCanonical(
        DuplicateGroup const & group,
        Assembly const & assembly)
    {
        if (group.members.empty())
        {
            return ResourceId{0};
        }

        // Select canonical based on:
        // 1. Higher internal reference count (more callers = keep it)
        // 2. Lower ResourceId as tie-breaker
        ResourceId best = group.members[0];
        size_t bestCount = countInternalReferences(best, assembly);

        for (auto const & id : group.members)
        {
            auto const count = countInternalReferences(id, assembly);
            // Priority 1: Higher reference count wins
            if (count > bestCount)
            {
                best = id;
                bestCount = count;
            }
            // Priority 2: Lower ResourceId as tie-breaker
            else if (count == bestCount && id < best)
            {
                best = id;
            }
        }
        return best;
    }

    size_t FunctionDeduplicator::countInternalReferences(
        ResourceId functionId,
        Assembly const & assembly)
    {
        size_t count = 0;

        for (auto const & [modelId, model] : assembly.getFunctions())
        {
            if (!model)
            {
                continue;
            }

            for (auto const & [nodeId, node] : *model)
            {
                if (auto const * callNode = dynamic_cast<FunctionCall const *>(node.get()))
                {
                    if (callNode->getFunctionId() == functionId)
                    {
                        ++count;
                    }
                }
                else if (auto const * gradNode =
                             dynamic_cast<FunctionGradient const *>(node.get()))
                {
                    if (gradNode->getFunctionId() == functionId)
                    {
                        ++count;
                    }
                }
                else if (auto const * normNode =
                             dynamic_cast<NormalizeDistanceField const *>(node.get()))
                {
                    if (normNode->getFunctionId() == functionId)
                    {
                        ++count;
                    }
                }
            }
        }

        return count;
    }

    size_t FunctionDeduplicator::updateInternalReferences(
        Assembly & assembly,
        ResourceId oldId,
        ResourceId newId)
    {
        size_t updatedCount = 0;

        for (auto & [modelId, model] : assembly.getFunctions())
        {
            if (!model)
            {
                continue;
            }

            for (auto & [nodeId, node] : *model)
            {
                if (auto * callNode = dynamic_cast<FunctionCall *>(node.get()))
                {
                    if (callNode->getFunctionId() == oldId)
                    {
                        callNode->setFunctionId(newId);
                        ++updatedCount;
                    }
                }
                else if (auto * gradNode = dynamic_cast<FunctionGradient *>(node.get()))
                {
                    if (gradNode->getFunctionId() == oldId)
                    {
                        gradNode->setFunctionId(newId);
                        ++updatedCount;
                    }
                }
                else if (auto * normNode = dynamic_cast<NormalizeDistanceField *>(node.get()))
                {
                    if (normNode->getFunctionId() == oldId)
                    {
                        normNode->setFunctionId(newId);
                        ++updatedCount;
                    }
                }
            }
        }

        return updatedCount;
    }
} // namespace gladius::nodes
