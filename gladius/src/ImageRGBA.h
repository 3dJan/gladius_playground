#pragma once

#if defined(GLADIUS_ENABLE_OPENCL)
#include "ComputeContext.h"
#endif

#include "ComputeTypes.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <typeinfo>
#include <vector>

namespace gladius
{
    class ComputeContext;

#if defined(GLADIUS_ENABLE_OPENCL)
    template <typename ImageDataPoint>
    class ImageImpl
    {
      public:
        explicit ImageImpl(ComputeContext & context)
            : m_width(512)
            , m_height(512)
            , m_size(m_width * m_height * m_depth)
            , m_ComputeContext(context)
            , m_gpuResource(m_ComputeContext.gpuAccessCoordinator().registerResource(
                resourceKindForDepth(m_depth), typeid(ImageDataPoint).name()))
        {
        }

        ImageImpl(ComputeContext & ComputeContext, size_t width, size_t height)
            : m_width(width)
            , m_height(height)
            , m_size(m_width * m_height * m_depth)
            , m_ComputeContext(ComputeContext)
            , m_gpuResource(m_ComputeContext.gpuAccessCoordinator().registerResource(
                resourceKindForDepth(m_depth), typeid(ImageDataPoint).name()))
        {
        }

        ImageImpl(ComputeContext & ComputeContext, size_t width, size_t height, size_t depth)
            : m_width(width)
            , m_height(height)
            , m_depth(depth)
            , m_size(m_width * m_height * m_depth)
            , m_ComputeContext(ComputeContext)
            , m_gpuResource(m_ComputeContext.gpuAccessCoordinator().registerResource(
                resourceKindForDepth(m_depth), typeid(ImageDataPoint).name()))
        {
        }

        // Note that only the content of the device buffer is copied but not download to the host
        ImageImpl(ImageImpl<ImageDataPoint> & src)
            : m_width(src.getWidth())
            , m_height(src.getHeight())
            , m_depth(src.getDepth())
            , m_size(m_width * m_height * m_depth)
            , m_ComputeContext(src.m_ComputeContext)
            , m_gpuResource(m_ComputeContext.gpuAccessCoordinator().registerResource(
                resourceKindForDepth(m_depth), typeid(ImageDataPoint).name()))
        {
            ImageImpl<ImageDataPoint>::allocateOnDevice();

            m_ComputeContext.waitForGpuResourceIdle(src.gpuResourceHandle());
            CL_ERROR(m_ComputeContext.GetQueue().enqueueCopyImage(
              src.getBuffer(), getBuffer(), {0, 0, 0}, {0, 0, 0}, {m_width, m_height, m_depth}));
            CL_ERROR(m_ComputeContext.GetQueue().finish());
        }

        virtual ~ImageImpl()
        {
            try
            {
                if (m_buffer && m_allocatedBytes > 0)
                {
                    m_ComputeContext.waitForGpuResourceIdle(m_gpuResource);
                    m_ComputeContext.onBufferReleased(m_allocatedBytes);
                    m_allocatedBytes = 0;
                }
            }
            catch (...)
            {
                // Destructors must not throw.
            }
        }

        void setWidth(size_t width)
        {

            m_width = std::max(static_cast<size_t>(1), width);
        }

        void setHeight(size_t height)
        {
            m_height = std::max(static_cast<size_t>(1), height);
        }

        void read()
        {
            m_ComputeContext.waitForGpuResourceIdle(m_gpuResource);
            CL_ERROR(m_ComputeContext.GetQueue().finish());
            CL_ERROR(m_ComputeContext.GetQueue().enqueueReadImage(
              *m_buffer, CL_TRUE, {0, 0, 0}, {m_width, m_height, m_depth}, 0, 0, m_data.data()));
            CL_ERROR(m_ComputeContext.GetQueue().finish());
        }

        void write()
        {
            retireCurrentGpuGeneration();
            CL_ERROR(m_ComputeContext.GetQueue().finish());
            CL_ERROR(m_ComputeContext.GetQueue().enqueueWriteImage(
              *m_buffer, CL_TRUE, {0, 0, 0}, {m_width, m_height, m_depth}, 0, 0, m_data.data()));
            CL_ERROR(m_ComputeContext.GetQueue().finish());
        }

        void fill(ImageDataPoint value)
        {
            std::fill(std::begin(m_data), std::end(m_data), value);
        }

        virtual void allocateOnDevice()
        {
            const cl::ImageFormat format = determineImageFormat();

            m_data.resize(m_width * m_height * m_depth);

            // If re-allocating, release previous accounting tracked
            if (m_buffer && m_allocatedBytes > 0)
            {
                retireCurrentGpuGeneration();
                m_ComputeContext.onBufferReleased(m_allocatedBytes);
                m_allocatedBytes = 0;
            }

            if (m_depth == 1)
            {
                m_buffer = m_ComputeContext.createImage2DChecked(format,
                                                                 m_width,
                                                                 m_height,
                                                                 CL_MEM_READ_WRITE,
                                                                 0,
                                                                 nullptr,
                                                                 typeid(ImageDataPoint).name());
                m_allocatedBytes =
                  ComputeContext::estimateImageSizeBytes(format, m_width, m_height, 1);
            }
            else
            {
                m_buffer = m_ComputeContext.createImage3DChecked(format,
                                                                 m_width,
                                                                 m_height,
                                                                 m_depth,
                                                                 CL_MEM_READ_WRITE,
                                                                 0,
                                                                 0,
                                                                 nullptr,
                                                                 typeid(ImageDataPoint).name());
                m_allocatedBytes =
                  ComputeContext::estimateImageSizeBytes(format, m_width, m_height, m_depth);
            }
            write();
        }

        std::vector<ImageDataPoint> & getData()
        {
            return m_data;
        }

        [[nodiscard]] const std::vector<ImageDataPoint> & getData() const
        {
            return m_data;
        }

        void print()
        {
            int i = 0;

            for (auto res : m_data)
            {
                std::cout << res.x << " ";
                ++i;

                if (i == static_cast<int>(sqrt((double) m_data.size())))
                {
                    i = 0;
                    std::cout << std::endl;
                }
            }

            std::cout << std::endl;
        }

        [[nodiscard]] size_t index(size_t x, size_t y) const
        {
            const auto ix = std::clamp(x, static_cast<size_t>(3), m_width - 2);
            const auto iy = std::clamp(y, static_cast<size_t>(3), m_height - 2);
            return iy * m_width + ix;
        }

        [[nodiscard]] size_t index(size_t x, size_t y, size_t z) const
        {
            const auto ix = std::clamp(x, static_cast<size_t>(0), m_width);
            const auto iy = std::clamp(y, static_cast<size_t>(0), m_height);
            const auto iz = std::clamp(z, static_cast<size_t>(0), m_depth);

            return std::clamp(
              iz * m_width * m_height + iy * m_width + ix, static_cast<size_t>(0), m_size - 1);
        }

        cl::Image & getBuffer()
        {
            return *m_buffer;
        }

        cl::Image * getBufferPtr()
        {
            return m_buffer.get();
        }

        [[nodiscard]] GpuResourceHandle gpuResourceHandle() const noexcept
        {
            return m_gpuResource;
        }

        [[nodiscard]] size_t getWidth() const
        {
            return m_width;
        }

        [[nodiscard]] size_t getHeight() const
        {
            return m_height;
        }

        [[nodiscard]] size_t getDepth() const
        {
            return m_depth;
        }

        [[nodiscard]] ImageDataPoint getValue(size_t x, size_t y) const
        {
            auto id = index(x, y);
            return m_data[id];
        }

        [[nodiscard]] ImageDataPoint getValue(size_t x, size_t y, size_t z) const
        {
            auto id = index(x, y, z);
            return m_data[id];
        }

        void setValue(size_t x, size_t y, ImageDataPoint value)
        {
            auto id = index(x, y);
            m_data[id] = value;
        }

        void setValue(size_t x, size_t y, size_t z, ImageDataPoint value)
        {
            auto id = index(x, y, z);
            m_data[id] = value;
        }

      protected:
        std::vector<ImageDataPoint> m_data;

        size_t m_width;
        size_t m_height;
        size_t m_depth = 1;

        size_t m_size = 0;
        ComputeContext & m_ComputeContext;
        std::unique_ptr<cl::Image> m_buffer;

        GpuResourceHandle m_gpuResource{};

        // Track bytes accounted in ComputeContext for this device image
        size_t m_allocatedBytes{0};

        void retireCurrentGpuGeneration()
        {
            m_ComputeContext.waitForGpuResourceIdle(m_gpuResource);

            auto retirement = m_ComputeContext.gpuAccessCoordinator().retireCurrentGeneration(
              m_gpuResource.resourceId);
            if (retirement.status == GpuAccessStatus::PendingAccessWithoutEvent)
            {
                m_ComputeContext.waitForGpuResourceIdle(m_gpuResource);
                retirement = m_ComputeContext.gpuAccessCoordinator().retireCurrentGeneration(
                  m_gpuResource.resourceId);
            }

            if (!retirement.granted())
            {
                throw std::runtime_error("Failed to retire GPU image generation safely");
            }

            m_ComputeContext.waitForGpuEvents(retirement.waitEvents);
            m_ComputeContext.gpuAccessCoordinator().collectCompletedRetirements();
            m_gpuResource = retirement.newGeneration;
        }

        [[nodiscard]] static GpuResourceKind resourceKindForDepth(size_t const depth) noexcept
        {
            return depth > 1u ? GpuResourceKind::Image3D : GpuResourceKind::Image2D;
        }

        static cl::ImageFormat determineImageFormat()
        {
            if constexpr (std::is_same_v<ImageDataPoint, cl_int>)
            {
                return cl::ImageFormat{CL_R, CL_SIGNED_INT32};
            }
            else if constexpr (std::is_same_v<ImageDataPoint, cl_int2>)
            {
                return cl::ImageFormat{CL_RG, CL_SIGNED_INT32};
            }
            else if constexpr (std::is_same_v<ImageDataPoint, cl_float2>)
            {
                return cl::ImageFormat{CL_RG, CL_FLOAT};
            }
            else if constexpr (std::is_same_v<ImageDataPoint, cl_float4>)
            {
                return cl::ImageFormat{CL_RGBA, CL_FLOAT};
            }
            else if constexpr (std::is_same_v<ImageDataPoint, cl_uchar>)
            {
                return cl::ImageFormat{CL_R, CL_UNSIGNED_INT8};
            }
            else if constexpr (std::is_same_v<ImageDataPoint, cl_float>)
            {
                return cl::ImageFormat{CL_R, CL_FLOAT};
            }
            else if constexpr (std::is_same_v<ImageDataPoint, cl_char4>)
            {
                return cl::ImageFormat{CL_RGBA, CL_UNSIGNED_INT8};
            }
            else if constexpr (std::is_same_v<ImageDataPoint, cl_char>)
            {
                return cl::ImageFormat{CL_R, CL_UNSIGNED_INT8};
            }
            else
            {
                throw std::domain_error("Image Format is not supported!");
            }
        }
    };

    using ImageRGBA = ImageImpl<cl_float4>;

    // TODO: Replace cl_floatN by structs if applicable
    // x = euclidean distance, y == 0:  does not need to be evaluate in the next layer, y ==
    // FLT_MAX:
    using DistanceMap = ImageImpl<cl_float2>;
    using DepthBuffer = ImageImpl<cl_float>;

    using PowerMap = ImageImpl<cl_float2>;
    // x and y are the position, z is set to FLT_MAX if the vertex is not contained in a contour
    using Vertices = ImageImpl<cl_float4>;

    using Normals = ImageImpl<cl_float2>;   // aka gradient
    using Adjacencies = ImageImpl<cl_int2>; // coordinate indices
    using JfAMap = ImageImpl<cl_float2>;    // normalized coordinates

    using Skeleton = ImageImpl<cl_int>;

    // `sdf_generator.cl` writes preview color to xyz and signed distance to w.
    // Keep full-float color here because the low-res preview resamples this 3D
    // field directly; any host-side distance consumer must read `.s[3]` only.
    using PreComputedSdf = ImageImpl<cl_float4>;

    /// Buffer storing traveled distances from low-res preview for HQ initialization
    /// Format: Single-channel float (CL_R, CL_FLOAT)
    /// Resolution: Same as low-res preview (e.g., width/4 × height/4)
    using DistanceInitBuffer = ImageImpl<cl_float>;

    using MarchingSquaresStates = ImageImpl<cl_char>;
#else
    /// Backend-neutral host image used by CPU-side contour and resource code when
    /// the OpenCL backend is not part of the binary.
    template <typename ImageDataPoint>
    class ImageImpl
    {
      public:
        explicit ImageImpl(ComputeContext &)
            : m_width(512)
            , m_height(512)
            , m_size(m_width * m_height * m_depth)
        {
        }

        ImageImpl(ComputeContext &, size_t width, size_t height)
            : m_width(width)
            , m_height(height)
            , m_size(m_width * m_height * m_depth)
        {
        }

        ImageImpl(ComputeContext &, size_t width, size_t height, size_t depth)
            : m_width(width)
            , m_height(height)
            , m_depth(depth)
            , m_size(m_width * m_height * m_depth)
        {
        }

        ImageImpl(ImageImpl const & source)
            : m_width(source.m_width)
            , m_height(source.m_height)
            , m_depth(source.m_depth)
            , m_size(source.m_size)
            , m_data(source.m_data)
        {
        }

        virtual ~ImageImpl() = default;

        void setWidth(size_t width)
        {
            m_width = std::max(static_cast<size_t>(1), width);
            m_size = m_width * m_height * m_depth;
        }

        void setHeight(size_t height)
        {
            m_height = std::max(static_cast<size_t>(1), height);
            m_size = m_width * m_height * m_depth;
        }

        void read()
        {
        }

        void write()
        {
        }

        void fill(ImageDataPoint value)
        {
            std::fill(std::begin(m_data), std::end(m_data), value);
        }

        virtual void allocateOnDevice()
        {
            m_size = m_width * m_height * m_depth;
            m_data.resize(m_size);
        }

        std::vector<ImageDataPoint> & getData()
        {
            return m_data;
        }

        [[nodiscard]] std::vector<ImageDataPoint> const & getData() const
        {
            return m_data;
        }

        void print()
        {
            int lineIndex = 0;
            for (auto const & value : m_data)
            {
                std::cout << value.x << " ";
                ++lineIndex;
                if (lineIndex == static_cast<int>(std::sqrt(static_cast<double>(m_data.size()))))
                {
                    lineIndex = 0;
                    std::cout << std::endl;
                }
            }
            std::cout << std::endl;
        }

        [[nodiscard]] size_t index(size_t x, size_t y) const
        {
            auto const ix = std::clamp(x, static_cast<size_t>(3), m_width - 2);
            auto const iy = std::clamp(y, static_cast<size_t>(3), m_height - 2);
            return iy * m_width + ix;
        }

        [[nodiscard]] size_t index(size_t x, size_t y, size_t z) const
        {
            auto const ix = std::clamp(x, static_cast<size_t>(0), m_width);
            auto const iy = std::clamp(y, static_cast<size_t>(0), m_height);
            auto const iz = std::clamp(z, static_cast<size_t>(0), m_depth);
            return std::clamp(iz * m_width * m_height + iy * m_width + ix,
                              static_cast<size_t>(0),
                              m_size - 1);
        }

        [[nodiscard]] size_t getWidth() const
        {
            return m_width;
        }

        [[nodiscard]] size_t getHeight() const
        {
            return m_height;
        }

        [[nodiscard]] size_t getDepth() const
        {
            return m_depth;
        }

        [[nodiscard]] ImageDataPoint getValue(size_t x, size_t y) const
        {
            return m_data.at(index(x, y));
        }

        [[nodiscard]] ImageDataPoint getValue(size_t x, size_t y, size_t z) const
        {
            return m_data.at(index(x, y, z));
        }

        void setValue(size_t x, size_t y, ImageDataPoint value)
        {
            m_data.at(index(x, y)) = value;
        }

        void setValue(size_t x, size_t y, size_t z, ImageDataPoint value)
        {
            m_data.at(index(x, y, z)) = value;
        }

      protected:
        std::vector<ImageDataPoint> m_data;
        size_t m_width;
        size_t m_height;
        size_t m_depth = 1;
        size_t m_size = 0;
    };

    using ImageRGBA = ImageImpl<cl_float4>;
    using DistanceMap = ImageImpl<cl_float2>;
    using DepthBuffer = ImageImpl<cl_float>;
    using PowerMap = ImageImpl<cl_float2>;
    using Vertices = ImageImpl<cl_float4>;
    using Normals = ImageImpl<cl_float2>;
    using Adjacencies = ImageImpl<cl_int2>;
    using JfAMap = ImageImpl<cl_float2>;
    using Skeleton = ImageImpl<cl_int>;
    using PreComputedSdf = ImageImpl<cl_float4>;
    using DistanceInitBuffer = ImageImpl<cl_float>;
    using MarchingSquaresStates = ImageImpl<cl_char>;
#endif
}
