#include "webgpu/WebGPUModelSliceRequestFactory.h"

#include "compute/AnalyticRenderSceneSnapshotFactory.h"
#include "nodes/Assembly.h"
#include "nodes/Model.h"
#include "webgpu/WebGPUFrameShaderComposer.h"
#include "webgpu/WebGPUSliceShaderComposer.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace gladius::webgpu
{
    namespace
    {
        [[nodiscard]] compute::RenderSceneSnapshot createAnalyticScene(nodes::Model & model)
        {
            return compute::AnalyticRenderSceneSnapshotFactory::create(model, 1u);
        }
    }

    compute::SliceRequest WebGPUModelSliceRequestFactory::create(nodes::Model & model,
                                                                  std::uint32_t const width,
                                                                  std::uint32_t const height,
                                                                  float const sliceZ,
                                                                  float const scale)
    {
        auto scene = createAnalyticScene(model);

        return compute::SliceRequest{.width = width,
                                     .height = height,
                                     .sliceZ = sliceZ,
                                     .scale = scale,
                                     .shaderSource = WebGPUSliceShaderComposer::compose(scene.analyticEvaluatorWgsl),
                                     .parameterValues = std::move(scene.parameterValues)};
    }

    compute::SliceRequest WebGPUModelSliceRequestFactory::create(nodes::Assembly const & assembly,
                                                                  std::uint32_t const width,
                                                                  std::uint32_t const height,
                                                                  float const sliceZ,
                                                                  float const scale)
    {
        auto scene = compute::AnalyticRenderSceneSnapshotFactory::create(assembly, 1u);
        return compute::SliceRequest{.width = width,
                                     .height = height,
                                     .sliceZ = sliceZ,
                                     .scale = scale,
                                     .shaderSource = WebGPUSliceShaderComposer::compose(scene.analyticEvaluatorWgsl),
                                     .parameterValues = std::move(scene.parameterValues)};
    }

    compute::FrameRequest WebGPUModelSliceRequestFactory::createFrame(nodes::Model & model,
                                                                       compute::FrameRequest frameRequest)
    {
        auto scene = createAnalyticScene(model);

        frameRequest.shaderSource = WebGPUFrameShaderComposer::compose(scene.analyticEvaluatorWgsl);
        frameRequest.parameterValues = std::move(scene.parameterValues);
        return frameRequest;
    }

    compute::FrameRequest WebGPUModelSliceRequestFactory::createFrame(nodes::Assembly const & assembly,
                                                                       compute::FrameRequest frameRequest)
    {
        auto scene = compute::AnalyticRenderSceneSnapshotFactory::create(assembly, 1u);
        frameRequest.shaderSource = WebGPUFrameShaderComposer::compose(scene.analyticEvaluatorWgsl);
        frameRequest.parameterValues = std::move(scene.parameterValues);
        return frameRequest;
    }

    compute::RenderSceneSnapshot WebGPUModelSliceRequestFactory::createScene(nodes::Model & model,
                                                                               std::uint64_t const sceneGeneration)
    {
        return compute::AnalyticRenderSceneSnapshotFactory::create(model, sceneGeneration);
    }

    compute::RenderSceneSnapshot WebGPUModelSliceRequestFactory::createScene(nodes::Assembly const & assembly,
                                                                               std::uint64_t const sceneGeneration)
    {
        return compute::AnalyticRenderSceneSnapshotFactory::create(assembly, sceneGeneration);
    }
}
