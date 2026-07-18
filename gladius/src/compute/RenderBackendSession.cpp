#include "compute/RenderBackendSession.h"

#include <stdexcept>
#include <utility>

namespace gladius::compute
{
    RenderBackendSession::RenderBackendSession(std::unique_ptr<IComputeRenderer> renderer)
        : m_renderer{std::move(renderer)}
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
        return m_renderer->getCapabilities();
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

    std::string const & RenderBackendSession::getErrorMessage() const noexcept
    {
        return m_errorMessage;
    }

    bool RenderBackendSession::replaceScene(RenderSceneSnapshot snapshot) noexcept
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

            auto scene = m_renderer->materializeScene(std::move(snapshot));
            if (!scene || scene->getBackendKind() != m_renderer->getBackendKind())
            {
                m_errorMessage = "Render backend returned an invalid materialized scene";
                return false;
            }

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