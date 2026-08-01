#include "compute/AnalyticRenderSceneSnapshotFactory.h"

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

    RenderSceneSnapshot AnalyticRenderSceneSnapshotFactory::create(nodes::Assembly const & assembly,
                                                                    std::uint64_t const sceneGeneration)
    {
        nodes::GraphFlattener flattener(assembly);
        auto flattenedAssembly = flattener.flatten();
        auto const & flattenedModel = flattenedAssembly.assemblyModel();
        if (!flattenedModel)
        {
            throw std::runtime_error("Analytic render scene could not obtain a flattened assembly model");
        }

        return create(*flattenedModel, sceneGeneration);
    }
}
