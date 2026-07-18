#include "webgpu/WebGPUModelSliceRequestFactory.h"

#include "nodes/Model.h"
#include "nodes/Assembly.h"
#include "nodes/GraphFlattener.h"
#include "nodes/Parameter.h"
#include "nodes/ToWgslVisitor.h"
#include "webgpu/WebGPUSliceShaderComposer.h"

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gladius::webgpu
{
    namespace
    {
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
                throw std::runtime_error("WebGPU slice request contains an unsupported parameter type");
            }

            auto const lookupIndex = const_cast<nodes::IParameter &>(parameter).getLookUpIndex();
            if (lookupIndex < 0)
            {
                throw std::runtime_error("WebGPU slice request contains an invalid parameter lookup index");
            }

            auto const firstIndex = static_cast<std::size_t>(lookupIndex);
            auto const writeValue = [&](std::size_t const index, float const value)
            {
                if (index >= parameterValues.size())
                {
                    throw std::runtime_error("WebGPU parameter lookup index exceeds generated evaluator storage");
                }
                if (assignedValues[index])
                {
                    throw std::runtime_error("WebGPU parameter lookup indices must be unique");
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

            throw std::runtime_error("WebGPU slice request supports only float, float3, and Matrix4x4 modifiable parameters");
        }
    }

    compute::SliceRequest WebGPUModelSliceRequestFactory::create(nodes::Model & model,
                                                                  std::uint32_t const width,
                                                                  std::uint32_t const height,
                                                                  float const sliceZ,
                                                                  float const scale)
    {
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

        return compute::SliceRequest{.width = width,
                                     .height = height,
                                     .sliceZ = sliceZ,
                                     .scale = scale,
                                     .shaderSource = WebGPUSliceShaderComposer::compose(evaluator.str()),
                                     .parameterValues = std::move(parameterValues)};
    }

    compute::SliceRequest WebGPUModelSliceRequestFactory::create(nodes::Assembly const & assembly,
                                                                  std::uint32_t const width,
                                                                  std::uint32_t const height,
                                                                  float const sliceZ,
                                                                  float const scale)
    {
        nodes::GraphFlattener flattener(assembly);
        auto flattenedAssembly = flattener.flatten();
        auto const & flattenedModel = flattenedAssembly.assemblyModel();
        if (!flattenedModel)
        {
            throw std::runtime_error("WebGPU slice request could not obtain a flattened assembly model");
        }

        return create(*flattenedModel, width, height, sliceZ, scale);
    }
}
