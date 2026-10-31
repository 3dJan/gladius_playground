#include "WebGPUSdfEvaluationRequest.h"

#include "webgpu/WebGPUSdfShaderComposer.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace gladius::webgpu
{
    namespace
    {
        compute::SdfEvaluationRequest makeSdfEvaluationRequest(
          compute::RenderSceneSnapshot const & snapshot,
          std::vector<float> const & parameterValues,
          float const isoValue,
          bool const returnColor)
        {
            if (!snapshot.isValid())
            {
                throw std::invalid_argument(
                  "Cannot create an SDF request from an invalid scene snapshot");
            }
            if (!std::isfinite(isoValue))
            {
                throw std::invalid_argument("SDF request iso value must be finite");
            }

            auto const hasMeshes = !snapshot.meshResources.empty();
            auto const hasBeams = !snapshot.beamLatticeResources.empty();
            auto const hasImages = !snapshot.imageResources.empty();

            compute::SdfEvaluationRequest request;
            request.isoValue = isoValue;
            request.returnColor = returnColor;
            if (returnColor)
            {
                request.shaderSource = WebGPUSdfShaderComposer::composeColorWithResourceSupport(
                  snapshot.analyticEvaluatorWgsl, hasMeshes, hasBeams, hasImages);
            }
            else
            {
                request.shaderSource = WebGPUSdfShaderComposer::composeWithResourceSupport(
                  snapshot.analyticEvaluatorWgsl, hasMeshes, hasBeams, hasImages);
            }
            request.parameterValues = parameterValues;

            request.meshPayloadTable.reserve(snapshot.meshResources.size());
            for (auto const & resource : snapshot.meshResources)
            {
                request.meshPayloadTable.push_back(resource.data);
            }
            request.beamPayloadTable.reserve(snapshot.beamLatticeResources.size());
            for (auto const & resource : snapshot.beamLatticeResources)
            {
                request.beamPayloadTable.push_back(resource.data);
            }
            request.imagePayloadTable.reserve(snapshot.imageResources.size());
            for (auto const & resource : snapshot.imageResources)
            {
                request.imagePayloadTable.push_back(resource.data);
            }

            return request;
        }
        }

    compute::SdfEvaluationRequest makeSdfEvaluationRequest(
      compute::RenderSceneSnapshot const & snapshot,
      float const isoValue,
      bool const returnColor)
    {
        return makeSdfEvaluationRequest(snapshot, snapshot.parameterValues, isoValue, returnColor);
    }

    compute::SdfEvaluationRequest makeSdfEvaluationRequest(
      compute::RenderSceneSnapshot const & snapshot,
      compute::RenderParameterSnapshot const & parameters,
      float const isoValue,
      bool const returnColor)
    {
        if (!parameters.isValid() || parameters.sceneGeneration != snapshot.sceneGeneration)
        {
            throw std::invalid_argument("SDF parameter snapshot does not match the scene generation");
        }
        return makeSdfEvaluationRequest(snapshot, parameters.values, isoValue, returnColor);
    }
}
