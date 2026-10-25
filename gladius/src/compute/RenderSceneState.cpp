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
    { m_primitives->create(); }

    SharedComputeContext RenderSceneState::getComputeContext() const
    { return m_computeContext; }

    SharedResources & RenderSceneState::getResourceContext()
    { return m_resources; }

    SharedResources RenderSceneState::getResourceContext() const
    { return m_resources; }

    SharedPrimitives & RenderSceneState::getPrimitives()
    { return m_primitives; }

    SharedPrimitives RenderSceneState::getPrimitives() const
    { return m_primitives; }

    ProgramManager & RenderSceneState::getProgramManager()
    { return m_programs; }

    ProgramManager const & RenderSceneState::getProgramManager() const
    { return m_programs; }
} // namespace gladius