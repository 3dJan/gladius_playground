#include "WebGPUSdfEvaluationRequest.h"

#include "webgpu/WebGPUSdfShaderComposer.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace gladius::webgpu
{
    compute::SdfEvaluationRequest makeSdfEvaluationRequest(
      compute::RenderSceneSnapshot const & snapshot,
      float const isoValue)
    {
        if (!snapshot.isValid())
        {
            throw std::invalid_argument("Cannot create an SDF request from an invalid scene snapshot");
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
        request.shaderSource = WebGPUSdfShaderComposer::composeWithResourceSupport(
          snapshot.analyticEvaluatorWgsl, hasMeshes, hasBeams, hasImages);
        request.parameterValues = snapshot.parameterValues;

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
