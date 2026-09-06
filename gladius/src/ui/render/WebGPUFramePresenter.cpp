#include "WebGPUFramePresenter.h"

#include "webgpu/WebGPUComputeContext.h"

#include <limits>
#include <utility>

namespace gladius::ui::async_rendering
{
    WebGPUFramePresenter::WebGPUFramePresenter(
      std::shared_ptr<webgpu::WebGPUComputeContext> context)
        : m_context{std::move(context)}
    {
    }

    WebGPUFramePresenter::~WebGPUFramePresenter()
    {
        release();
    }

    bool WebGPUFramePresenter::present(compute::RenderFrame const & frame)
    {
        if (!frame.isValid() || !m_context || !m_context->isValid())
        {
            return false;
        }

        auto const rowCount = frame.endRow - frame.firstRow;
        auto const bytesPerRow = static_cast<std::size_t>(frame.width) * sizeof(std::uint32_t);
        if (bytesPerRow > std::numeric_limits<std::uint32_t>::max())
        {
            return false;
        }

        webgpu::WebGPUComputeContext::DeviceLock const deviceLock(*m_context);
        if (!m_texture || m_width != frame.width || m_height != frame.height)
        {
            allocateTexture(frame.width, frame.height);
        }
        if (!m_texture || !m_textureView)
        {
            return false;
        }

        wgpu::TexelCopyTextureInfo destination;
        destination.texture = m_texture;
        destination.origin = {.x = 0u, .y = frame.firstRow, .z = 0u};

        wgpu::TexelCopyBufferLayout layout;
        layout.bytesPerRow = static_cast<std::uint32_t>(bytesPerRow);
        layout.rowsPerImage = rowCount;

        wgpu::Extent3D writeSize{.width = frame.width, .height = rowCount, .depthOrArrayLayers = 1u};
        m_context->getQueue().WriteTexture(&destination,
                                           frame.pixels.data(),
                                           frame.pixels.size() * sizeof(std::uint32_t),
                                           &layout,
                                           &writeSize);

        m_freshness = frame.freshness;
        return true;
    }

    void WebGPUFramePresenter::release() noexcept
    {
        if (m_context)
        {
            webgpu::WebGPUComputeContext::DeviceLock const deviceLock(*m_context);
            m_textureView = nullptr;
            m_texture = nullptr;
        }
        m_width = 0u;
        m_height = 0u;
        m_freshness.reset();
    }

    std::uintptr_t WebGPUFramePresenter::getTextureId() const noexcept
    {
        return reinterpret_cast<std::uintptr_t>(m_textureView.Get());
    }

    std::uint32_t WebGPUFramePresenter::getWidth() const noexcept
    {
        return m_width;
    }

    std::uint32_t WebGPUFramePresenter::getHeight() const noexcept
    {
        return m_height;
    }

    std::optional<compute::RenderFreshnessStamp> WebGPUFramePresenter::getFreshness() const noexcept
    {
        return m_freshness;
    }

    void WebGPUFramePresenter::allocateTexture(std::uint32_t const width,
                                               std::uint32_t const height)
    {
        m_textureView = nullptr;
        m_texture = nullptr;

        wgpu::TextureDescriptor descriptor;
        descriptor.label = "Gladius WebGPU frame texture";
        descriptor.usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::TextureBinding;
        descriptor.dimension = wgpu::TextureDimension::e2D;
        descriptor.size = {.width = width, .height = height, .depthOrArrayLayers = 1u};
        descriptor.format = wgpu::TextureFormat::RGBA8Unorm;
        descriptor.mipLevelCount = 1u;
        descriptor.sampleCount = 1u;
        m_texture = m_context->getDevice().CreateTexture(&descriptor);
        if (m_texture)
        {
            m_textureView = m_texture.CreateView();
            if (!m_textureView)
            {
                m_texture = nullptr;
            }
        }

        if (m_texture && m_textureView)
        {
            m_width = width;
            m_height = height;
        }
        else
        {
            m_width = 0u;
            m_height = 0u;
        }
    }
}
