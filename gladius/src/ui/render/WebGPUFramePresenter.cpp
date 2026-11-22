#include "WebGPUFramePresenter.h"

#include "ProgressivePreviewSeed.h"
#include "webgpu/WebGPUComputeContext.h"
#include "webgpu/WebGPUFrameBufferSet.h"

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
        beginFrame();
    }

    bool WebGPUFramePresenter::supportsGpuDirectPresentation() const noexcept
    {
        return m_context != nullptr && m_context->isValid();
    }

    bool WebGPUFramePresenter::present(compute::RenderFrame const & frame,
                                       FramePresentationCandidate const & candidate)
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

        bool const activeDimensionsChanged = m_width != frame.width || m_height != frame.height;
        bool const freshnessChanged = !m_freshness.has_value() || *m_freshness != frame.freshness;
        bool const isFullFrame = frame.firstRow == 0u && frame.endRow == frame.height;
        bool const startsProgressiveFrame = frame.firstRow == 0u && frame.endRow < frame.height;
        if (frame.firstRow > 0u && (activeDimensionsChanged || freshnessChanged))
        {
            std::cerr << "[WebGPUFramePresenter] Cannot continue an uninitialized progressive frame\n";
            return false;
        }

        bool const isGpuDirect = frame.gpuOutputHandle != nullptr;
        auto const * gpuOutput = isGpuDirect
                                   ? static_cast<webgpu::WebGPUGpuOutputFrame const *>(
                                       frame.gpuOutputHandle.get())
                                   : nullptr;
        if (isGpuDirect && (gpuOutput == nullptr || !gpuOutput->outputBuffer ||
                            gpuOutput->bytesPerRow < rowBytes ||
                            (gpuOutput->bytesPerRow % rowAlignment) != 0u ||
                            gpuOutput->rowCount < rowCount))
        {
            return false;
        }

        // Full-frame uploads overwrite every pixel in [0..width) x [0..height), so clearing
        // beforehand would waste an entire full-frame texture upload.
        bool const initializeFrame =
          !isFullFrame && (activeDimensionsChanged || freshnessChanged || startsProgressiveFrame);

        if (!isGpuDirect)
        {
            auto const uploadSize = alignedBytesPerRow * static_cast<std::size_t>(rowCount);
            m_uploadData.resize(uploadSize);
            for (std::uint32_t row = 0u; row < rowCount; ++row)
            {
                auto const * sourceRow =
                  reinterpret_cast<unsigned char const *>(frame.pixels.data()) +
                  static_cast<std::size_t>(row) * rowBytes;
                std::copy_n(sourceRow,
                            rowBytes,
                            m_uploadData.data() + static_cast<std::size_t>(row) * alignedBytesPerRow);
            }
        }

        if (initializeFrame)
        {
            if (static_cast<std::size_t>(frame.height) >
                std::numeric_limits<std::size_t>::max() / alignedBytesPerRow)
            {
                return false;
            }
            auto const clearSizeBytes = alignedBytesPerRow * static_cast<std::size_t>(frame.height);
            m_clearData.resize(clearSizeBytes / sizeof(std::uint32_t));
            std::fill(m_clearData.begin(), m_clearData.end(), 0xFF1A1A1Au);

            bool const hasCompatibleSeed =
              startsProgressiveFrame && m_previewSeedFrame && m_previewSeedCandidate &&
              !m_previewSeedFrame->pixels.empty() &&
              canSeedProgressiveFrame(*m_previewSeedFrame,
                                      *m_previewSeedCandidate,
                                      frame,
                                      candidate);
            if (hasCompatibleSeed)
            {
                (void) resampleCompleteFrameToBuffer(*m_previewSeedFrame,
                                                     frame.width,
                                                     frame.height,
                                                     m_clearData.data(),
                                                     alignedBytesPerRow / sizeof(std::uint32_t));
            }
        }

        webgpu::WebGPUComputeContext::DeviceLock const deviceLock(*m_context);
        if (!m_texture || frame.width > m_capacityWidth || frame.height > m_capacityHeight)
        {
            auto const capacityWidth = std::max(m_capacityWidth, frame.width);
            auto const capacityHeight = std::max(m_capacityHeight, frame.height);
            if (!allocateTexture(capacityWidth, capacityHeight))
            {
                return false;
            }
        }
        if (!m_texture || !m_textureView)
        {
            return false;
        }

        if (initializeFrame)
        {
            auto const clearSizeBytes = alignedBytesPerRow * static_cast<std::size_t>(frame.height);
            wgpu::TexelCopyTextureInfo clearDestination;
            clearDestination.texture = m_texture;
            clearDestination.origin = {.x = 0u, .y = 0u, .z = 0u};
            wgpu::TexelCopyBufferLayout clearLayout;
            clearLayout.bytesPerRow = static_cast<std::uint32_t>(alignedBytesPerRow);
            clearLayout.rowsPerImage = frame.height;
            wgpu::Extent3D clearExtent{.width = frame.width,
                                       .height = frame.height,
                                       .depthOrArrayLayers = 1u};
            m_context->getQueue().WriteTexture(&clearDestination,
                                               m_clearData.data(),
                                               clearSizeBytes,
                                               &clearLayout,
                                               &clearExtent);
        }

        if (isGpuDirect)
        {
            wgpu::TexelCopyBufferInfo source;
            source.buffer = gpuOutput->outputBuffer;
            source.layout.offset = 0u;
            source.layout.bytesPerRow = gpuOutput->bytesPerRow;
            source.layout.rowsPerImage = rowCount;

            wgpu::TexelCopyTextureInfo destination;
            destination.texture = m_texture;
            destination.origin = {.x = 0u, .y = frame.firstRow, .z = 0u};

            wgpu::Extent3D copySize{
              .width = frame.width, .height = rowCount, .depthOrArrayLayers = 1u};

            auto const encoder = m_context->getDevice().CreateCommandEncoder();
            encoder.CopyBufferToTexture(&source, &destination, &copySize);
            auto const commandBuffer = encoder.Finish();
            m_context->getQueue().Submit(1u, &commandBuffer);
            // Retain the GPU output buffer lease until the next present/release so the
            // frame buffer pool ping-pongs to its second slot while CopyBufferToTexture runs.
            m_lastGpuOutputHandle = frame.gpuOutputHandle;
        }
        else
        {
            m_lastGpuOutputHandle.reset();
            wgpu::TexelCopyTextureInfo destination;
            destination.texture = m_texture;
            destination.origin = {.x = 0u, .y = frame.firstRow, .z = 0u};

            wgpu::TexelCopyBufferLayout layout;
            layout.bytesPerRow = static_cast<std::uint32_t>(alignedBytesPerRow);
            layout.rowsPerImage = rowCount;

            wgpu::Extent3D writeSize{
              .width = frame.width, .height = rowCount, .depthOrArrayLayers = 1u};
            m_context->getQueue().WriteTexture(&destination,
                                               m_uploadData.data(),
                                               m_uploadData.size(),
                                               &layout,
                                               &writeSize);
        }

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
        bool const loggedDimensionsChanged = frame.width != m_lastLoggedWidth || frame.height != m_lastLoggedHeight;
        bool const viewChanged = frame.freshness.viewGeneration != m_lastLoggedViewGeneration;
        if (!frame.pixels.empty() && (m_presentCount < 5u || loggedDimensionsChanged || viewChanged))
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

        m_width = frame.width;
        m_height = frame.height;
        m_freshness = frame.freshness;
        if (isCompletePreviewSeedFrame(frame, candidate) && !frame.pixels.empty())
        {
            m_previewSeedFrame = frame;
            m_previewSeedCandidate = candidate;
        }
        else if (isPreviewFrameSource(candidate.source) ||
                 (m_previewSeedCandidate &&
                  !matchesProgressivePreviewSeedStamp(m_previewSeedCandidate->stamp,
                                                       candidate.stamp)))
        {
            m_previewSeedFrame.reset();
            m_previewSeedCandidate.reset();
        }
        ++m_presentCount;
        return true;
    }

    void WebGPUFramePresenter::beginFrame() noexcept
    {
        if (m_retiredResources.empty())
        {
            return;
        }
        if (m_context)
        {
            webgpu::WebGPUComputeContext::DeviceLock const deviceLock(*m_context);
            m_retiredResources.clear();
        }
        else
        {
            m_retiredResources.clear();
        }
    }

    void WebGPUFramePresenter::release() noexcept
    {
        // Defer releasing the WebGPU texture/view until the next frame because ImGui
        // may have already recorded this texture view into the current frame's draw list.
        if (m_texture || m_textureView)
        {
            m_retiredResources.push_back(
              RetiredTexture{std::move(m_texture), std::move(m_textureView)});
        }
        m_width = 0u;
        m_height = 0u;
        m_capacityWidth = 0u;
        m_capacityHeight = 0u;
        m_freshness.reset();
        m_previewSeedFrame.reset();
        m_previewSeedCandidate.reset();
        m_lastGpuOutputHandle.reset();
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

    std::array<float, 2> WebGPUFramePresenter::getTextureUvMax() const noexcept
    {
        if (m_capacityWidth == 0u || m_capacityHeight == 0u)
        {
            return {0.0f, 0.0f};
        }
        return {static_cast<float>(m_width) / static_cast<float>(m_capacityWidth),
                static_cast<float>(m_height) / static_cast<float>(m_capacityHeight)};
    }

    std::optional<compute::RenderFreshnessStamp> WebGPUFramePresenter::getFreshness() const noexcept
    {
        return m_freshness;
    }

    bool WebGPUFramePresenter::allocateTexture(std::uint32_t const width,
                                               std::uint32_t const height)
    {
        wgpu::TextureDescriptor descriptor;
        descriptor.label = "Gladius WebGPU frame texture";
        descriptor.usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::TextureBinding;
        descriptor.dimension = wgpu::TextureDimension::e2D;
        descriptor.size = {.width = width, .height = height, .depthOrArrayLayers = 1u};
        descriptor.format = wgpu::TextureFormat::RGBA8Unorm;
        descriptor.mipLevelCount = 1u;
        descriptor.sampleCount = 1u;
        auto texture = m_context->getDevice().CreateTexture(&descriptor);
        if (!texture)
        {
            return false;
        }
        auto textureView = texture.CreateView();
        if (!textureView)
        {
            return false;
        }

        if (m_texture || m_textureView)
        {
            m_retiredResources.push_back(
              RetiredTexture{std::move(m_texture), std::move(m_textureView)});
        }
        m_texture = std::move(texture);
        m_textureView = std::move(textureView);
        m_capacityWidth = width;
        m_capacityHeight = height;
        return true;
    }
}
