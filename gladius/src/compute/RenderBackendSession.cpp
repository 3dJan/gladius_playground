#include "compute/RenderBackendSession.h"
#include "compute/ParameterLayout.h"
#include "nodes/Assembly.h"
#include "nodes/Parameter.h"

#include <stdexcept>
#include <utility>

namespace gladius::compute
{
    RenderBackendSession::RenderBackendSession(std::unique_ptr<IComputeRenderer> renderer,
                                               std::shared_ptr<IBoundsService> boundsService)
        : m_renderer{std::move(renderer)}
        , m_boundsService{std::move(boundsService)}
    {
        if (!m_renderer)
        {
            throw std::invalid_argument("Render backend session requires a renderer");
        }
    }

    ComputeBackendKind RenderBackendSession::getBackendKind() const noexcept
    {
        return m_renderer->getBackendKind();
    }

    RendererCapability RenderBackendSession::getCapabilities() const noexcept
    {
        auto capabilities = m_renderer->getCapabilities();
        if (m_boundsService)
        {
            capabilities = capabilities | m_boundsService->getCapabilities();
        }
        return capabilities;
    }

    bool RenderBackendSession::isAvailable() const noexcept
    {
        return m_renderer->isAvailable();
    }

    bool RenderBackendSession::hasMaterializedScene() const noexcept
    {
        return m_scene != nullptr;
    }

    std::uint64_t RenderBackendSession::getSceneGeneration() const noexcept
    {
        return m_scene ? m_scene->getSceneGeneration() : 0u;
    }

    std::shared_ptr<const RenderSceneSnapshot> RenderBackendSession::getSceneSnapshot() const noexcept
    {
        return m_snapshot;
    }

    std::shared_ptr<const RenderParameterSnapshot>
    RenderBackendSession::getParameterSnapshot() const noexcept
    {
        return m_parameterSnapshot;
    }

    IBoundsService * RenderBackendSession::getBoundsService() noexcept
    {
        return m_boundsService.get();
    }

    std::string const & RenderBackendSession::getErrorMessage() const noexcept
    {
        return m_errorMessage;
    }

    bool RenderBackendSession::replaceScene(RenderSceneSnapshot snapshot,
                                            std::uint64_t const parameterGeneration) noexcept
    {
        try
        {
            if (!m_renderer->isAvailable())
            {
                m_errorMessage = "Selected render backend is unavailable";
                return false;
            }
            if (!snapshot.isValid())
            {
                m_errorMessage = "Render scene snapshot is invalid";
                return false;
            }
            if (!hasCapability(m_renderer->getCapabilities(), snapshot.requiredCapabilities))
            {
                m_errorMessage = "Selected render backend does not support the scene capabilities";
                return false;
            }

            auto sharedSnapshot = std::make_shared<const RenderSceneSnapshot>(std::move(snapshot));
            auto scene = m_renderer->materializeScene(sharedSnapshot);
            if (!scene || scene->getBackendKind() != m_renderer->getBackendKind())
            {
                m_errorMessage = "Render backend returned an invalid materialized scene";
                return false;
            }

            auto parameterSnapshot = std::make_shared<const RenderParameterSnapshot>(
              RenderParameterSnapshot{.sceneGeneration = sharedSnapshot->sceneGeneration,
                                      .parameterGeneration = parameterGeneration,
                                      .values = sharedSnapshot->parameterValues});
            if (!parameterSnapshot->isValid())
            {
                m_errorMessage = "Render scene contains an invalid parameter snapshot";
                return false;
            }
            (void) scene->updateParameterSnapshot(parameterSnapshot);

            if (m_boundsService)
            {
                m_boundsService->setSceneSnapshot(sharedSnapshot);
                m_boundsService->setParameterSnapshot(parameterSnapshot);
            }
            m_snapshot = std::move(sharedSnapshot);
            m_parameterSnapshot = std::move(parameterSnapshot);
            m_scene = std::move(scene);
            m_errorMessage.clear();
            return true;
        }
        catch (std::exception const & error)
        {
            m_errorMessage = error.what();
            return false;
        }
        catch (...)
        {
            m_errorMessage = "Render scene materialization failed";
            return false;
        }
    }

    bool RenderBackendSession::updateParameterValues(nodes::Assembly const & assembly,
                                                     std::uint64_t const parameterGeneration) noexcept
    {
        try
        {
            if (!m_scene || !m_snapshot || !m_parameterSnapshot ||
                !m_snapshot->parameterLayout)
            {
                m_errorMessage = "Materialized scene has no runtime parameter layout";
                return false;
            }

            auto values = m_parameterSnapshot->values;
            auto const & layout = *m_snapshot->parameterLayout;
            for (auto const & [modelId, model] : assembly.getFunctions())
            {
                if (!model)
                {
                    continue;
                }

                for (auto const & [parameterId, parameter] : model->getConstParameterRegistry())
                {
                    if (parameter == nullptr || parameter->getId() != parameterId)
                    {
                        continue;
                    }

                    auto const origin = nodes::ParameterOrigin{modelId, parameterId};
                    if (layout.getDestinations(origin).empty())
                    {
                        continue;
                    }

                    auto const * variantParameter =
                      dynamic_cast<nodes::VariantParameter const *>(parameter);
                    if (variantParameter == nullptr ||
                        !layout.apply(origin,
                                      variantParameter->getValue(),
                                      m_snapshot->sceneGeneration,
                                      values))
                    {
                        m_errorMessage = "Unable to pack runtime parameter " +
                                         std::to_string(parameterId);
                        return false;
                    }
                }
            }

            auto parameterSnapshot = std::make_shared<const RenderParameterSnapshot>(
              RenderParameterSnapshot{.sceneGeneration = m_snapshot->sceneGeneration,
                                      .parameterGeneration = parameterGeneration,
                                      .values = std::move(values)});
            if (!parameterSnapshot->isValid() ||
                !m_scene->updateParameterSnapshot(parameterSnapshot))
            {
                m_errorMessage = "Render backend rejected the runtime parameter snapshot";
                return false;
            }

            if (m_boundsService)
            {
                m_boundsService->setParameterSnapshot(parameterSnapshot);
            }
            m_parameterSnapshot = std::move(parameterSnapshot);
            m_errorMessage.clear();
            return true;
        }
        catch (std::exception const & error)
        {
            m_errorMessage = error.what();
            return false;
        }
        catch (...)
        {
            m_errorMessage = "Runtime parameter update failed";
            return false;
        }
    }

    std::unique_ptr<IRenderSubmission> RenderBackendSession::submitFrame(RenderRequest request)
    {
        if (!m_scene)
        {
            throw std::runtime_error("Selected render backend has no materialized scene");
        }
        if (!m_renderer->isAvailable())
        {
            throw std::runtime_error("Selected render backend is unavailable");
        }

        return m_renderer->submitFrame(*m_scene, std::move(request));
    }
}