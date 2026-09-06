#include "ui/render/OpenGLFramePresenter.h"

#include <glad/glad.h>

namespace gladius::ui::async_rendering
{
    OpenGLFramePresenter::~OpenGLFramePresenter()
    {
        release();
    }

    bool OpenGLFramePresenter::canPresent(compute::RenderFrame const & frame) noexcept
    {
        return frame.isValid();
    }

    bool OpenGLFramePresenter::present(compute::RenderFrame const & frame)
    {
        if (!canPresent(frame))
        {
            return false;
        }

        if (m_textureId == 0u)
        {
            glGenTextures(1, &m_textureId);
        }

        glBindTexture(GL_TEXTURE_2D, m_textureId);
        if (m_width != frame.width || m_height != frame.height)
        {
            allocateTexture(frame.width, frame.height);
        }

        GLint previousUnpackAlignment{};
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpackAlignment);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        static_cast<GLint>(frame.firstRow),
                        static_cast<GLsizei>(frame.width),
                        static_cast<GLsizei>(frame.endRow - frame.firstRow),
                        GL_RGBA,
                        GL_UNSIGNED_BYTE,
                        frame.pixels.data());
                glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpackAlignment);
        glBindTexture(GL_TEXTURE_2D, 0);

        m_freshness = frame.freshness;
        return true;
    }

    void OpenGLFramePresenter::release() noexcept
    {
        if (m_textureId != 0u)
        {
            glDeleteTextures(1, &m_textureId);
            m_textureId = 0u;
        }
        m_width = 0u;
        m_height = 0u;
        m_freshness.reset();
    }

    std::uintptr_t OpenGLFramePresenter::getTextureId() const noexcept
    {
        return m_textureId;
    }

    std::uint32_t OpenGLFramePresenter::getWidth() const noexcept
    {
        return m_width;
    }

    std::uint32_t OpenGLFramePresenter::getHeight() const noexcept
    {
        return m_height;
    }

    std::optional<compute::RenderFreshnessStamp> OpenGLFramePresenter::getFreshness() const noexcept
    {
        return m_freshness;
    }

    void OpenGLFramePresenter::allocateTexture(std::uint32_t const width, std::uint32_t const height)
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA8,
                     static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height),
                     0,
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     nullptr);
        m_width = width;
        m_height = height;
    }
}
