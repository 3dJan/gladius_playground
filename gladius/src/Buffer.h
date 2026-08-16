#pragma once

#if defined(GLADIUS_ENABLE_OPENCL)
#include "ComputeContext.h"
#else
#include <cstddef>
#include <iostream>
#include <vector>

namespace gladius
{
    class ComputeContext;
}
#endif

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <typeinfo>
#include <vector>

namespace gladius
{
#if defined(GLADIUS_ENABLE_OPENCL)
    template <typename T>
    class Buffer
    {
      public:
        explicit Buffer(ComputeContext & context)
            : m_ComputeContext(context)
            , m_gpuResource(m_ComputeContext.gpuAccessCoordinator().registerResource(
                GpuResourceKind::Buffer, typeid(T).name()))
        {
        }

        ~Buffer()
        {
            try
            {
                // Release tracked bytes only after known GPU users have completed.
                if (m_buffer && m_size > 0)
                {
                    m_ComputeContext.waitForGpuResourceIdle(m_gpuResource);
                    m_ComputeContext.onBufferReleased(sizeof(T) * m_size);
                }
            }
            catch (...)
            {
                // Destructors must not throw.
            }
        }
        Buffer(Buffer const & other)
            : m_ComputeContext(other.m_ComputeContext)
            , m_gpuResource(m_ComputeContext.gpuAccessCoordinator().registerResource(
                GpuResourceKind::Buffer, typeid(T).name()))
            , m_data(other.m_data)
        {
            create();
        }

        // copying would require copying the buffer, which is not allowed
        Buffer(Buffer && other) = delete;

        Buffer & operator=(Buffer && other) = delete;
        Buffer & operator=(const Buffer & other) = delete;

        void read()
        {
            if (!m_buffer)
            {
                throw std::runtime_error("Failed to read, device buffer could not be created");
            }
            m_ComputeContext.waitForGpuResourceIdle(m_gpuResource);
            m_data.resize(m_size);
            CL_ERROR(m_ComputeContext.GetQueue().enqueueReadBuffer(
              *m_buffer.get(), CL_TRUE, 0, sizeof(T) * m_size, &m_data[0]));
            CL_ERROR(m_ComputeContext.GetQueue().finish());
        }

        void create()
        {
            if (m_data.empty())
            {
                m_data.push_back({});
            }
            // If there was a previous allocation, release its accounting first
            if (m_buffer && m_size > 0)
            {
                retireCurrentGpuGeneration();
                m_ComputeContext.onBufferReleased(sizeof(T) * m_size);
            }

            const size_t bytes = sizeof(T) * m_data.size();
            m_buffer = m_ComputeContext.createBufferChecked(
              CL_MEM_READ_WRITE, bytes, nullptr, typeid(T).name());
            m_size = m_data.size();
        }

        void clear()
        {
            m_data.clear();
            if (m_buffer && m_size > 0)
            {
                retireCurrentGpuGeneration();
                m_ComputeContext.onBufferReleased(sizeof(T) * m_size);
            }
            m_size = 0;
            m_buffer.reset();
        }

        void write()
        {
            if (m_data.empty())
            {
                return;
            }
            if (!m_buffer || m_size != m_data.size())
            {
                create(); // recreate buffer with the needed size
            }

            if (!m_buffer)
            {
                throw std::runtime_error("Failed to write, device buffer could not be created");
            }

            retireCurrentGpuGeneration();
            CL_ERROR(m_ComputeContext.GetQueue().enqueueWriteBuffer(
              *m_buffer.get(), CL_TRUE, 0, sizeof(T) * m_data.size(), &m_data[0]));

            CL_ERROR(m_ComputeContext.GetQueue().finish());
        }

        [[nodiscard]] std::vector<T> getDataCopy()
        {
            return m_data;
        }

        [[nodiscard]] std::vector<T> & getData()
        {
            return m_data;
        }

        [[nodiscard]] auto getSize() const -> size_t
        {
            return m_data.size();
        }

        void print() const
        {
            int elementCount = 0;
            int const lineBreak = static_cast<int>(std::sqrt(static_cast<double>(m_data.size())));

            for (const auto & res : m_data)
            {
                std::cout << res << " ";
                ++elementCount;
                if (elementCount == lineBreak)
                {
                    elementCount = 0;
                    std::cout << std::endl;
                }
            }
            std::cout << std::endl;
        }

        [[nodiscard]] auto getBuffer() const -> const cl::Buffer &
        {
            return *m_buffer.get();
        }

        [[nodiscard]] GpuResourceHandle gpuResourceHandle() const noexcept
        {
            return m_gpuResource;
        }

      private:
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
                throw std::runtime_error("Failed to retire GPU buffer generation safely");
            }

            m_ComputeContext.waitForGpuEvents(retirement.waitEvents);
            m_ComputeContext.gpuAccessCoordinator().collectCompletedRetirements();
            m_gpuResource = retirement.newGeneration;
        }

        ComputeContext & m_ComputeContext;
        GpuResourceHandle m_gpuResource{};
        std::vector<T> m_data;
        size_t m_size = 0;
        std::unique_ptr<cl::Buffer> m_buffer;
    };
#else
    /// Backend-neutral host buffer used when the OpenCL backend is not built.
    /// It preserves the data-container API needed by mesh and CPU-side code without
    /// pulling OpenCL wrapper types into pure WebGPU translation units.
    template <typename T>
    class Buffer
    {
      public:
        explicit Buffer(ComputeContext &)
        {
        }

        ~Buffer() = default;
        Buffer(Buffer const &) = default;
        Buffer(Buffer &&) = delete;
        Buffer & operator=(Buffer const &) = delete;
        Buffer & operator=(Buffer &&) = delete;

        void read()
        {
        }

        void create()
        {
            if (m_data.empty())
            {
                m_data.push_back({});
            }
        }

        void clear()
        {
            m_data.clear();
        }

        void write()
        {
        }

        [[nodiscard]] std::vector<T> getDataCopy() const
        {
            return m_data;
        }

        [[nodiscard]] std::vector<T> & getData()
        {
            return m_data;
        }

        [[nodiscard]] std::vector<T> const & getData() const
        {
            return m_data;
        }

        [[nodiscard]] size_t getSize() const
        {
            return m_data.size();
        }

        void print() const
        {
            int elementCount = 0;
            int const lineBreak = static_cast<int>(
              std::sqrt(static_cast<double>(m_data.size())));

            for (auto const & value : m_data)
            {
                std::cout << value << " ";
                ++elementCount;
                if (lineBreak > 0 && elementCount == lineBreak)
                {
                    elementCount = 0;
                    std::cout << std::endl;
                }
            }
            std::cout << std::endl;
        }

      private:
        std::vector<T> m_data;
    };
#endif
}