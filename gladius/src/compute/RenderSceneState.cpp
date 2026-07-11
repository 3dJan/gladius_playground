#include "RenderSceneState.h"

#include "Primitives.h"

namespace gladius
{
    RenderSceneState::RenderSceneState(SharedComputeContext context,
                                       RequiredCapabilities requiredCapabilities,
                                       events::SharedLogger logger)
        : m_computeContext(std::move(context))
        , m_resources(std::make_shared<ResourceContext>(m_computeContext))
        , m_primitives(std::make_shared<Primitives>(*m_computeContext))
        , m_programs(m_computeContext, requiredCapabilities, std::move(logger), m_resources)
    {
        m_primitives->create();
    }

    SharedComputeContext RenderSceneState::getComputeContext() const
    {
        return m_computeContext;
    }

    void RenderSceneState::setComputeContext(SharedComputeContext context)
    {
        m_programs.requestShutdownAll();
        m_programs.waitForAllCompilations();
        if (m_computeContext)
        {
            m_computeContext->waitForAllTrackedGpuWork();
        }

        auto resources = std::make_shared<ResourceContext>(context);
        auto primitives = std::make_shared<Primitives>(*context);
        primitives->create();

        // ProgramManager owns the programs that reference the old resources. They have been
        // drained above before this replacement, so destroying them cannot race old GPU work.
        m_programs.setComputeContext(context, resources);
        m_computeContext = std::move(context);
        m_resources = std::move(resources);
        m_primitives = std::move(primitives);
    }

    SharedResources & RenderSceneState::getResourceContext()
    {
        return m_resources;
    }

    SharedResources RenderSceneState::getResourceContext() const
    {
        return m_resources;
    }

    SharedPrimitives & RenderSceneState::getPrimitives()
    {
        return m_primitives;
    }

    SharedPrimitives RenderSceneState::getPrimitives() const
    {
        return m_primitives;
    }

    ProgramManager & RenderSceneState::getProgramManager()
    {
        return m_programs;
    }

    ProgramManager const & RenderSceneState::getProgramManager() const
    {
        return m_programs;
    }
} // namespace gladius