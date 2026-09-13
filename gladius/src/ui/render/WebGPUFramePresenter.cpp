#include "WebGPUFramePresenter.h"

#include "webgpu/WebGPUComputeContext.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <iostream>
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
        auto const width = static_cast<std::size_t>(frame.width);
        if (width > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))
        {
            return false;
        }
        auto const rowBytes = width * sizeof(std::uint32_t);
        constexpr std::size_t rowAlignment = 256u;
        if (rowBytes > std::numeric_limits<std::size_t>::max() - (rowAlignment - 1u))
        {
            return false;
        }
        auto const alignedBytesPerRow =
          (rowBytes + rowAlignment - 1u) & ~(rowAlignment - 1u);
        if (alignedBytesPerRow > std::numeric_limits<std::uint32_t>::max() ||
            static_cast<std::size_t>(rowCount) >
              std::numeric_limits<std::size_t>::max() / alignedBytesPerRow)
        {
            return false;
        }

        auto const uploadSize = alignedBytesPerRow * static_cast<std::size_t>(rowCount);
        m_uploadData.resize(uploadSize);
        for (std::uint32_t row = 0u; row < rowCount; ++row)
        {
            auto const * sourceRow = reinterpret_cast<unsigned char const *>(frame.pixels.data()) +
                                     static_cast<std::size_t>(row) * rowBytes;
            std::copy_n(sourceRow,
                        rowBytes,
                        m_uploadData.data() + static_cast<std::size_t>(row) * alignedBytesPerRow);
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
        layout.bytesPerRow = static_cast<std::uint32_t>(alignedBytesPerRow);
        layout.rowsPerImage = rowCount;

        wgpu::Extent3D writeSize{.width = frame.width, .height = rowCount, .depthOrArrayLayers = 1u};
        m_context->getQueue().WriteTexture(&destination,
                                           m_uploadData.data(),
                                           m_uploadData.size(),
                                           &layout,
                                           &writeSize);

        auto const errorMessage = m_context->getErrorMessage();
#ifdef __EMSCRIPTEN__
        if (errorMessage != m_lastReportedError)
        {
            if (!errorMessage.empty())
            {
                std::cerr << "[WebGPUFramePresenter] Dawn error after upload: " << errorMessage << '\n';
            }
            m_lastReportedError = errorMessage;
        }
        bool const dimensionsChanged = frame.width != m_lastLoggedWidth || frame.height != m_lastLoggedHeight;
        bool const viewChanged = frame.freshness.viewGeneration != m_lastLoggedViewGeneration;
        if (m_presentCount < 5u || dimensionsChanged || viewChanged)
        {
            std::uint64_t checksum = 0u;
            for (auto const pixel : frame.pixels)
            {
                checksum = (checksum * 16777619u) ^ pixel;
            }
            std::cerr << "[WebGPUFramePresenter] upload #" << m_presentCount
                      << " frame=" << frame.width << 'x' << frame.height
                      << " rows=[" << frame.firstRow << ',' << frame.endRow << ')'
                      << " bytes=" << frame.pixels.size() * sizeof(std::uint32_t)
                      << " view=" << frame.freshness.viewGeneration
                      << " hash=0x" << std::hex << checksum
                      << " first=0x" << std::hex << frame.pixels.front()
                      << " last=0x" << frame.pixels.back() << std::dec
                      << " texture=" << reinterpret_cast<std::uintptr_t>(m_textureView.Get()) << '\n';
            m_lastLoggedViewGeneration = frame.freshness.viewGeneration;
            m_lastLoggedWidth = frame.width;
            m_lastLoggedHeight = frame.height;
        }
#endif

        m_freshness = frame.freshness;
        ++m_presentCount;
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
        m_presentCount = 0u;
        m_lastLoggedViewGeneration = 0u;
        m_lastLoggedWidth = 0u;
        m_lastLoggedHeight = 0u;
        m_lastReportedError.clear();
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
