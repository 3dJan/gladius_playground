#include "webgpu/WebGPUMeshColorSampler.h"

#include "io/3mf/FaceColorSampler.h"
#include "webgpu/WebGPUSdfEvaluationRequest.h"
#include "webgpu/WebGPUSdfEvaluator.h"

#include <Eigen/Core>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gladius::webgpu
{
    namespace
    {
        std::vector<io::FaceColors> sampleColors(
          std::shared_ptr<WebGPUComputeContext> context,
          compute::RenderSceneSnapshot const & snapshot,
          io::IndexedTriangleMesh const & mesh,
          std::size_t const samplesPerFace,
          bool const convertToSrgb,
          io::MeshExportCancellationPredicate shouldCancel,
          WebGPUMeshColorSampler::ProgressCallback reportProgress)
        {
            if (auto const error = mesh.validationError())
            {
                throw std::invalid_argument("Cannot sample colors for an invalid mesh: " + *error);
            }
            if (samplesPerFace != 1u && samplesPerFace != 4u)
            {
                throw std::invalid_argument("Unsupported WebGPU face color sample count");
            }

            auto const faceCount = mesh.indices.size() / 3u;
            if (faceCount > std::numeric_limits<std::size_t>::max() / samplesPerFace)
            {
                throw std::length_error("WebGPU face-color sample count is too large");
            }
            auto const sampleCount = faceCount * samplesPerFace;
            std::vector<io::FaceColors> result(samplesPerFace);
            for (auto & colors : result)
            {
                colors.colors.resize(faceCount);
            }
            if (sampleCount == 0u)
            {
                return result;
            }

            if (reportProgress)
            {
                reportProgress(0.0);
            }
            auto request = makeSdfEvaluationRequest(snapshot, 0.0F, true);
            WebGPUSdfEvaluator evaluator{std::move(context)};
            bool const completed = evaluator.evaluateColorsGenerated(
              request,
              sampleCount,
              [&mesh, samplesPerFace](std::size_t const offset,
                                     std::size_t const count,
                                     std::vector<std::array<float, 3>> & positions)
              {
                  positions.reserve(count);
                  for (std::size_t sample = offset; sample < offset + count; ++sample)
                  {
                      auto const faceIndex = sample / samplesPerFace;
                      auto const sampleIndex = sample % samplesPerFace;
                      auto const triangleOffset = faceIndex * 3u;
                      Eigen::Vector3f const & first = mesh.positions[mesh.indices[triangleOffset]];
                      Eigen::Vector3f const & second = mesh.positions[mesh.indices[triangleOffset + 1u]];
                      Eigen::Vector3f const & third = mesh.positions[mesh.indices[triangleOffset + 2u]];
                      Eigen::Vector3f position = (first + second + third) / 3.0F;
                      if (samplesPerFace == 4u)
                      {
                          switch (sampleIndex)
                          {
                          case 1u:
                              position = (first + second) * 0.5F;
                              break;
                          case 2u:
                              position = (second + third) * 0.5F;
                              break;
                          case 3u:
                              position = (third + first) * 0.5F;
                              break;
                          default:
                              break;
                          }
                      }
                      positions.push_back({position.x(), position.y(), position.z()});
                  }
              },
              [&result, convertToSrgb](std::size_t const offset,
                                       std::vector<std::array<float, 3>> && batch)
              {
                  for (std::size_t index = 0u; index < batch.size(); ++index)
                  {
                      auto const pointIndex = offset + index;
                      Eigen::Vector3f color{batch[index][0], batch[index][1], batch[index][2]};
                      if (!color.allFinite())
                      {
                          throw std::runtime_error("WebGPU model returned a non-finite color");
                      }
                      if (convertToSrgb)
                      {
                          color = io::FaceColorSampler::linearToSrgb(color);
                      }
                      auto const samplesPerFace = result.size();
                      auto const faceIndex = pointIndex / samplesPerFace;
                      auto const sampleIndex = pointIndex % samplesPerFace;
                      result[sampleIndex][faceIndex] = io::Color8::fromVector3f(color);
                  }
              },
              std::move(shouldCancel));
            if (!completed)
            {
                throw io::MeshExportCancelled{};
            }
            if (reportProgress)
            {
                reportProgress(1.0);
            }
            return result;
        }
    } // namespace

    io::FaceColors WebGPUMeshColorSampler::sampleFaceColors(
      std::shared_ptr<WebGPUComputeContext> context,
      compute::RenderSceneSnapshot const & snapshot,
      io::IndexedTriangleMesh const & mesh,
      bool const convertToSrgb,
      io::MeshExportCancellationPredicate shouldCancel,
      ProgressCallback reportProgress)
    {
        auto samples = sampleColors(std::move(context),
                                    snapshot,
                                    mesh,
                                    1u,
                                    convertToSrgb,
                                    std::move(shouldCancel),
                                    std::move(reportProgress));
        return std::move(samples.front());
    }

    std::vector<io::FaceColors> WebGPUMeshColorSampler::sampleFaceColorsMultipoint(
      std::shared_ptr<WebGPUComputeContext> context,
      compute::RenderSceneSnapshot const & snapshot,
      io::IndexedTriangleMesh const & mesh,
      bool const convertToSrgb,
      io::MeshExportCancellationPredicate shouldCancel,
      ProgressCallback reportProgress)
    {
        return sampleColors(std::move(context),
                            snapshot,
                            mesh,
                            4u,
                            convertToSrgb,
                            std::move(shouldCancel),
                            std::move(reportProgress));
    }
} // namespace gladius::webgpu
