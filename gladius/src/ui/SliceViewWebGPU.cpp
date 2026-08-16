#include "SliceView.h"

namespace gladius::ui
{
    void SliceView::show()
    {
        m_visible = true;
    }

    void SliceView::hide()
    {
        m_visible = false;
    }

    bool SliceView::isVisible() const
    {
        return m_visible;
    }

    bool SliceView::render(gladius::ComputeCore &, GLView &)
    {
        return false;
    }

    bool SliceView::isHovered() const
    {
        return false;
    }

    void SliceView::zoomIn()
    {
        m_zoomTarget = std::min(100.0f, m_zoomTarget * 1.25f);
    }

    void SliceView::zoomOut()
    {
        m_zoomTarget = std::max(0.5f, m_zoomTarget / 1.25f);
    }

    void SliceView::resetView()
    {
        m_zoom = 4.0f;
        m_zoomTarget = 4.0f;
        m_scrolling = {0.0f, 250.0f};
    }

    void SliceView::centerView()
    {
        resetView();
    }
}
