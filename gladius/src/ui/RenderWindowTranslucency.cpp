/// @file RenderWindowTranslucency.cpp
/// @brief Bridges the translucency widgets to whichever render backend owns the view.

#include "RenderWindow.h"

#include "imgui.h"

#if defined(GLADIUS_ENABLE_OPENCL)
#include "compute/ComputeCore.h"
#endif

namespace gladius::ui
{
    compute::TranslucencyConfig RenderWindow::readActiveTranslucency() const
    {
#if defined(GLADIUS_ENABLE_OPENCL)
        if (m_core != nullptr)
        {
            return compute::readTranslucency(m_core->getResourceContext()->getRenderingSettings());
        }
#endif
        return compute::readTranslucency(m_neutralRenderSettings);
    }

    bool RenderWindow::applyActiveTranslucency(compute::TranslucencyConfig const & config)
    {
#if defined(GLADIUS_ENABLE_OPENCL)
        if (m_core != nullptr)
        {
            return compute::applyTranslucency(m_core->getResourceContext()->getRenderingSettings(),
                                              config);
        }
#endif
        return compute::applyTranslucency(m_neutralRenderSettings, config);
    }

    bool RenderWindow::renderTranslucencyMenuSection()
    {
        // Both backends integrate the channel, but only once one of them actually owns the
        // view - before that there is no settings object to push the state into.
        bool hasBackend = m_neutralBackendActive;
#if defined(GLADIUS_ENABLE_OPENCL)
        hasBackend = hasBackend || (m_core != nullptr);
#endif
        if (!hasBackend)
        {
            return false;
        }

        ImGui::Separator();
        auto edited = readActiveTranslucency();
        return renderTranslucencyControls(edited, 150.f * m_uiScale) &&
               applyActiveTranslucency(edited);
    }
} // namespace gladius::ui
