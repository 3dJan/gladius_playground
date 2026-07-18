#include "RenderSession.h"

namespace gladius
{
    RenderSession::RenderSession(SharedRenderSceneGeneration generation, RenderSessionInputs inputs)
        : m_generation(std::move(generation))
        , m_inputs(std::move(inputs))
        , m_payloadSnapshot(RenderPayloadSnapshot::capture(m_generation->getResources(),
                                                           *m_generation->getPrimitives()))
    {
    }

    SharedComputeContext RenderSession::getComputeContext() const
    { return m_generation->getComputeContext(); }

    RenderSceneRevision RenderSession::getRevision() const noexcept
    { return m_generation->getRevision(); }

    RenderSessionInputs const & RenderSession::getInputs() const
    { return m_inputs; }

    RenderPayloadSnapshot const & RenderSession::getPayloadSnapshot() const
    { return m_payloadSnapshot; }

    SharedPrimitives RenderSession::getPrimitives() const
    { return m_generation->getPrimitives(); }

    std::optional<RenderProgram *> RenderSession::tryGetRenderProgram() const
    { return m_generation->getProgramManager().tryGetBestRenderProgram(); }

    bool RenderSession::isPayloadCurrent() const
    {
        return m_payloadSnapshot.isCurrent(
          m_generation->getComputeContext()->gpuAccessCoordinator());
    }
} // namespace gladius
