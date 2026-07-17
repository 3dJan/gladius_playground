#include "RenderSceneGeneration.h"

namespace gladius
{
    RenderSceneGeneration::RenderSceneGeneration(SharedComputeContext context,
                                                 RequiredCapabilities requiredCapabilities,
                                                 events::SharedLogger logger,
                                                 RenderSceneRevision revision)
        : m_computeContext(std::move(context))
        , m_resources(std::make_shared<ResourceContext>(m_computeContext))
        , m_primitives(std::make_shared<Primitives>(*m_computeContext))
        , m_revision(revision)
        , m_programs(m_computeContext, requiredCapabilities, std::move(logger), m_resources)
    {
        m_primitives->create();
    }

    SharedComputeContext RenderSceneGeneration::getComputeContext() const
    {
        return m_computeContext;
    }

    SharedResources RenderSceneGeneration::getResources() const
    {
        return m_resources;
    }

    SharedPrimitives RenderSceneGeneration::getPrimitives() const
    {
        return m_primitives;
    }

    RenderSceneRevision RenderSceneGeneration::getRevision() const noexcept
    {
        return m_revision;
    }

    ProgramManager & RenderSceneGeneration::getProgramManager()
    {
        return m_programs;
    }

    ProgramManager const & RenderSceneGeneration::getProgramManager() const
    {
        return m_programs;
    }
} // namespace gladius
