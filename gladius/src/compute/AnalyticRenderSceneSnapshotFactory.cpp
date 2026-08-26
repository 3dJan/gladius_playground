#include "compute/AnalyticRenderSceneSnapshotFactory.h"

#include "BeamLatticeResource.h"
#include "BeamPayloadSerializer.h"
#include "ImagePayloadSerializer.h"
#include "ImageStackResource.h"
#include "MeshPayloadSerializer.h"
#include "MeshResourceBase.h"
#include "ResourceManager.h"
#include "SpatialMeshResource.h"
#include "nodes/Assembly.h"
#include "nodes/GraphFlattener.h"
#include "nodes/Model.h"
#include "nodes/Parameter.h"
#include "nodes/ToWgslVisitor.h"

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gladius::compute
{
    namespace
    {
        void assignParameterLookupIndices(nodes::Model & model)
        {
            int nextIndex = 0;
            for (auto const & [parameterId, parameter] : model.getParameterRegistry())
            {
                if (parameter == nullptr || parameter->getId() != parameterId || !parameter->isModifiable() ||
                    parameter->getConstSource().has_value())
                {
                    continue;
                }

                parameter->setLookUpIndex(nextIndex);
                nextIndex += parameter->getSize();
            }
        }

        void writeParameterValues(nodes::IParameter const & parameter,
                                  std::vector<float> & parameterValues,
                                  std::vector<bool> & assignedValues)
        {
            if (!parameter.isModifiable() || parameter.getConstSource().has_value())
            {
                return;
            }

            auto const * variantParameter = dynamic_cast<nodes::VariantParameter const *>(&parameter);
            if (variantParameter == nullptr)
            {
                throw std::runtime_error("Analytic render scene contains an unsupported parameter type");
            }

            auto const lookupIndex = const_cast<nodes::IParameter &>(parameter).getLookUpIndex();
            if (lookupIndex < 0)
            {
                throw std::runtime_error("Analytic render scene contains an invalid parameter lookup index");
            }

            auto const firstIndex = static_cast<std::size_t>(lookupIndex);
            auto const writeValue = [&](std::size_t const index, float const value)
            {
                if (index >= parameterValues.size())
                {
                    throw std::runtime_error("Analytic render scene parameter lookup exceeds evaluator storage");
                }
                if (assignedValues[index])
                {
                    throw std::runtime_error("Analytic render scene parameter lookup index " +
                                             std::to_string(index) + " is assigned more than once by parameter " +
                                             std::to_string(parameter.getId()));
                }

                parameterValues[index] = value;
                assignedValues[index] = true;
            };

            auto const & value = variantParameter->getValue();
            if (auto const * scalar = std::get_if<float>(&value))
            {
                writeValue(firstIndex, *scalar);
                return;
            }
            if (auto const * vector = std::get_if<nodes::float3>(&value))
            {
                writeValue(firstIndex, vector->x);
                writeValue(firstIndex + 1u, vector->y);
                writeValue(firstIndex + 2u, vector->z);
                return;
            }
            if (auto const * matrix = std::get_if<nodes::Matrix4x4>(&value))
            {
                for (std::size_t row = 0u; row < 4u; ++row)
                {
                    for (std::size_t column = 0u; column < 4u; ++column)
                    {
                        writeValue(firstIndex + row * 4u + column, (*matrix)[row][column]);
                    }
                }
                return;
            }

            throw std::runtime_error("Analytic render scene supports only float, float3, and Matrix4x4 modifiable parameters");
        }
    }

    RenderSceneSnapshot AnalyticRenderSceneSnapshotFactory::create(nodes::Model & model,
                                                                    std::uint64_t const sceneGeneration)
    {
        assignParameterLookupIndices(model);

        nodes::ToWgslVisitor visitor;
        model.visitNodes(visitor);

        std::ostringstream evaluator;
        visitor.write(evaluator);

        std::vector<float> parameterValues(visitor.getRequiredParameterCount(), 0.0f);
        std::vector<bool> assignedValues(parameterValues.size(), false);
        for (auto const & [parameterId, parameter] : model.getConstParameterRegistry())
        {
            if (parameter != nullptr && parameter->getId() == parameterId && visitor.usesParameter(parameterId))
            {
                writeParameterValues(*parameter, parameterValues, assignedValues);
            }
        }

        return {.sceneGeneration = sceneGeneration,
                .requiredCapabilities = RendererCapability::AnalyticRendering,
                .analyticEvaluatorWgsl = evaluator.str(),
                .parameterValues = std::move(parameterValues)};
    }

    RenderSceneSnapshot AnalyticRenderSceneSnapshotFactory::create(
      nodes::Model & model, std::uint64_t const sceneGeneration, ResourceManager const & resourceManager)
    {
        auto snapshot = create(model, sceneGeneration);

        // Collect mesh resource ids referenced by the lowered model. The visitor emits
        // gladiusSignedDistanceToMesh(pos, id) calls; the ids must match payload entries.
        std::set<ResourceId> referencedMeshIds;
        std::set<ResourceId> referencedBeamLatticeIds;
        std::set<ResourceId> referencedImageIds;
        for (auto const & node : model)
        {
            auto * meshNode = dynamic_cast<nodes::SignedDistanceToMesh *>(node.second.get());
            auto * beamNode = dynamic_cast<nodes::SignedDistanceToBeamLattice *>(node.second.get());
            auto * imageNode = dynamic_cast<nodes::ImageSampler *>(node.second.get());
            if (meshNode == nullptr && beamNode == nullptr && imageNode == nullptr)
            {
                continue;
            }
            try
            {
                // Re-resolve the connected resource node the same way the visitor does.
                auto const & fieldName = meshNode != nullptr
                                          ? nodes::FieldNames::Mesh
                                          : beamNode != nullptr ? nodes::FieldNames::BeamLattice
                                                                : nodes::FieldNames::ResourceId;
                auto & resourceParameter = node.second->parameter().at(fieldName);
                auto const source = resourceParameter.getSource();
                if (!source.has_value() || source->port == nullptr ||
                    source->port->getParent() == nullptr)
                {
                    continue;
                }
                auto * resourceNode = dynamic_cast<nodes::Resource *>(source->port->getParent());
                if (resourceNode == nullptr)
                {
                    continue;
                }
                if (meshNode != nullptr)
                {
                    referencedMeshIds.insert(resourceNode->getResourceId());
                }
                else if (beamNode != nullptr)
                {
                    referencedBeamLatticeIds.insert(resourceNode->getResourceId());
                }
                else
                {
                    referencedImageIds.insert(resourceNode->getResourceId());
                }
            }
            catch (std::exception const &)
            {
                throw std::runtime_error(
                                    "Analytic render scene contains a resource node without a valid resource");
            }
        }

        if (referencedMeshIds.empty() && referencedBeamLatticeIds.empty() &&
            referencedImageIds.empty())
        {
            return snapshot;
        }

        for (auto const resourceId : referencedMeshIds)
        {
            // getResourcePtr is non-const in the ResourceManager API; the snapshot
            // factory only reads the resource, so use a const_cast at this boundary.
            auto & mutableManager = const_cast<ResourceManager &>(resourceManager);
            auto * resource = mutableManager.getResourcePtr(ResourceKey{resourceId, ResourceType::Mesh});
            if (resource == nullptr)
            {
                throw std::runtime_error("Analytic render scene references a missing mesh resource");
            }
            auto * spatialMesh = dynamic_cast<SpatialMeshResource *>(resource);
            if (spatialMesh == nullptr)
            {
                throw std::runtime_error(
                  "WebGPU render scenes support SpatialMesh resources only (no VDB meshes)");
            }

            MeshResourcePayload payload;
            payload.data = io::serializeSpatialMeshPayload(spatialMesh->getData());
            auto const index = static_cast<std::size_t>(resourceId);
            if (index >= snapshot.meshResources.size())
            {
                snapshot.meshResources.resize(index + 1u);
            }
            snapshot.meshResources[index] = std::move(payload);
        }

            if (!referencedMeshIds.empty())
            {
                snapshot.requiredCapabilities =
                  snapshot.requiredCapabilities | RendererCapability::MeshSdf;
            }

        for (auto const resourceId : referencedBeamLatticeIds)
        {
            auto & mutableManager = const_cast<ResourceManager &>(resourceManager);
            auto * resource =
              mutableManager.getResourcePtr(ResourceKey{resourceId, ResourceType::BeamLattice});
            if (resource == nullptr)
            {
                throw std::runtime_error(
                  "Analytic render scene references a missing beam lattice resource");
            }
            auto * beamLattice = dynamic_cast<BeamLatticeResource *>(resource);
            if (beamLattice == nullptr)
            {
                throw std::runtime_error("Referenced beam lattice resource has an unexpected type");
            }

            MeshResourcePayload payload;
            payload.data =
              io::serializeBeamLatticePayload(beamLattice->getBeams(), beamLattice->getBalls());
            auto const index = static_cast<std::size_t>(resourceId);
            if (index >= snapshot.beamLatticeResources.size())
            {
                snapshot.beamLatticeResources.resize(index + 1u);
            }
            snapshot.beamLatticeResources[index] = std::move(payload);
        }

        if (!referencedBeamLatticeIds.empty())
        {
            snapshot.requiredCapabilities =
              snapshot.requiredCapabilities | RendererCapability::BeamLattice;
        }

        for (auto const resourceId : referencedImageIds)
        {
            auto & mutableManager = const_cast<ResourceManager &>(resourceManager);
            auto * resource = mutableManager.getResourcePtr(
              ResourceKey{resourceId, ResourceType::ImageStack});
            if (resource == nullptr)
            {
                throw std::runtime_error(
                  "Analytic render scene references a missing image stack resource");
            }
            auto * imageStack = dynamic_cast<ImageStackResource *>(resource);
            if (imageStack == nullptr || imageStack->getImageStack() == nullptr)
            {
                throw std::runtime_error("Referenced image stack resource has an unexpected type");
            }

            MeshResourcePayload payload;
            payload.data = io::serializeImageStackPayload(*imageStack->getImageStack());
            auto const index = static_cast<std::size_t>(resourceId);
            if (index >= snapshot.imageResources.size())
            {
                snapshot.imageResources.resize(index + 1u);
            }
            snapshot.imageResources[index] = std::move(payload);
        }

        if (!referencedImageIds.empty())
        {
            snapshot.requiredCapabilities =
              snapshot.requiredCapabilities | RendererCapability::ImageSampling;
        }
        return snapshot;
    }

    RenderSceneSnapshot AnalyticRenderSceneSnapshotFactory::create(nodes::Assembly const & assembly,
                                                                    std::uint64_t const sceneGeneration)
    {
        return create(assembly, sceneGeneration, nullptr);
    }

    RenderSceneSnapshot AnalyticRenderSceneSnapshotFactory::create(nodes::Assembly const & assembly,
                                                                    std::uint64_t const sceneGeneration,
                                                                    ResourceManager const * resourceManager)
    {
        nodes::GraphFlattener flattener(assembly);
        auto flattenedAssembly = flattener.flatten();
        auto const & flattenedModel = flattenedAssembly.assemblyModel();
        if (!flattenedModel)
        {
            throw std::runtime_error("Analytic render scene could not obtain a flattened assembly model");
        }

        if (resourceManager != nullptr)
        {
            return create(*flattenedModel, sceneGeneration, *resourceManager);
        }
        return create(*flattenedModel, sceneGeneration);
    }
}