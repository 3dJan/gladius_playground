#pragma once
#include "ResourceKey.h"
#include <vector>

namespace gladius::io
{
    using ImageData = std::vector<unsigned char>;

    enum class PixelFormat
    {
        GRAYSCALE_1BIT,
        RGBA_8BIT,
        RGB_8BIT,
        GRAYSCALE_8BIT,
        GRAYSCALE_ALPHA_8BIT,
        RGBA_16BIT,
        RGB_16BIT,
        GRAYSCALE_16BIT,
        GRAYSCALE_ALPHA_16BIT
    };

    class Image
    {
      public:
        Image(ImageData const & data)
            : m_data(data)
        {
        }
        Image(ImageData const & data, unsigned int width, unsigned int height)
            : m_data(data)
            , m_width(width)
            , m_height(height)
        {
        }

        ImageData const & getData() const
        {
            return m_data;
        }

        unsigned int getWidth() const
        {
            return m_width;
        }

        unsigned int getHeight() const
        {
            return m_height;
        }

        PixelFormat getFormat() const
        {
            return m_format;
        }

        void setFormat(PixelFormat format)
        {
            m_format = format;
        }

        size_t getBitDepth() const
        {
            return m_bitDepth;
        }

        void setBitDepth(size_t bitDepth)
        {
            m_bitDepth = bitDepth;
        }

        void swapXYData()
        {
            std::vector<unsigned char> swappedData;
            swappedData.reserve(m_data.size());

            if (m_width == 0 || m_height == 0)
            {
                throw std::runtime_error("Image width or height is zero");
            }

            if (m_data.size() % (m_width * m_height) != 0)
            {
                throw std::runtime_error("Image data size is not a multiple of width * height");
            }

            unsigned int const numChannels =
              static_cast<unsigned int>(m_data.size()) / (m_width * m_height);
            for (int y = m_height - 1; y >= 0; --y)
            {
                for (int x = m_width - 1; x >= 0; --x)
                {
                    unsigned int index = (y * m_width + x) * numChannels;
                    for (unsigned int i = 0; i < numChannels; ++i)
                    {
                        swappedData.push_back(m_data[index + i]);
                    }
                }
            }

            m_data = std::move(swappedData);
        }

        /// Flip image horizontally (mirror along X axis)
        void flipHorizontal()
        {
            if (m_width == 0 || m_height == 0 || m_data.empty())
            {
                return;
            }

            unsigned int const numChannels =
              static_cast<unsigned int>(m_data.size()) / (m_width * m_height);
            unsigned int const rowBytes = m_width * numChannels;

            for (unsigned int y = 0; y < m_height; ++y)
            {
                unsigned char * rowStart = m_data.data() + y * rowBytes;
                for (unsigned int x = 0; x < m_width / 2; ++x)
                {
                    unsigned int leftIdx = x * numChannels;
                    unsigned int rightIdx = (m_width - 1 - x) * numChannels;
                    for (unsigned int c = 0; c < numChannels; ++c)
                    {
                        std::swap(rowStart[leftIdx + c], rowStart[rightIdx + c]);
                    }
                }
            }
        }

        /// Flip image vertically (mirror along Y axis)
        void flipVertical()
        {
            if (m_width == 0 || m_height == 0 || m_data.empty())
            {
                return;
            }

            unsigned int const numChannels =
              static_cast<unsigned int>(m_data.size()) / (m_width * m_height);
            unsigned int const rowBytes = m_width * numChannels;

            for (unsigned int y = 0; y < m_height / 2; ++y)
            {
                unsigned char * topRow = m_data.data() + y * rowBytes;
                unsigned char * bottomRow = m_data.data() + (m_height - 1 - y) * rowBytes;
                for (unsigned int i = 0; i < rowBytes; ++i)
                {
                    std::swap(topRow[i], bottomRow[i]);
                }
            }
        }

        /// Rotate image 90° clockwise
        /// @note Swaps width and height
        void rotate90CW()
        {
            if (m_width == 0 || m_height == 0 || m_data.empty())
            {
                return;
            }

            unsigned int const numChannels =
              static_cast<unsigned int>(m_data.size()) / (m_width * m_height);

            std::vector<unsigned char> rotated;
            rotated.resize(m_data.size());

            // New dimensions after rotation
            unsigned int const newWidth = m_height;
            unsigned int const newHeight = m_width;

            for (unsigned int y = 0; y < m_height; ++y)
            {
                for (unsigned int x = 0; x < m_width; ++x)
                {
                    unsigned int srcIdx = (y * m_width + x) * numChannels;
                    // CW rotation: (x,y) -> (height-1-y, x) in new coords
                    unsigned int newX = m_height - 1 - y;
                    unsigned int newY = x;
                    unsigned int dstIdx = (newY * newWidth + newX) * numChannels;
                    for (unsigned int c = 0; c < numChannels; ++c)
                    {
                        rotated[dstIdx + c] = m_data[srcIdx + c];
                    }
                }
            }

            m_data = std::move(rotated);
            std::swap(m_width, m_height);
        }

        /// Rotate image 90° counter-clockwise
        /// @note Swaps width and height
        void rotate90CCW()
        {
            if (m_width == 0 || m_height == 0 || m_data.empty())
            {
                return;
            }

            unsigned int const numChannels =
              static_cast<unsigned int>(m_data.size()) / (m_width * m_height);

            std::vector<unsigned char> rotated;
            rotated.resize(m_data.size());

            // New dimensions after rotation
            unsigned int const newWidth = m_height;
            unsigned int const newHeight = m_width;

            for (unsigned int y = 0; y < m_height; ++y)
            {
                for (unsigned int x = 0; x < m_width; ++x)
                {
                    unsigned int srcIdx = (y * m_width + x) * numChannels;
                    // CCW rotation: (x,y) -> (y, width-1-x) in new coords
                    unsigned int newX = y;
                    unsigned int newY = m_width - 1 - x;
                    unsigned int dstIdx = (newY * newWidth + newX) * numChannels;
                    for (unsigned int c = 0; c < numChannels; ++c)
                    {
                        rotated[dstIdx + c] = m_data[srcIdx + c];
                    }
                }
            }

            m_data = std::move(rotated);
            std::swap(m_width, m_height);
        }

        /// Pad image to new dimensions
        /// @param newWidth Target width (must be >= current width)
        /// @param newHeight Target height (must be >= current height)
        /// @param padValue Pixel value for padding (default: 0)
        void padTo(unsigned int newWidth, unsigned int newHeight, unsigned char padValue = 0)
        {
            if (newWidth < m_width || newHeight < m_height)
            {
                throw std::runtime_error("New dimensions must be >= current dimensions");
            }

            if (newWidth == m_width && newHeight == m_height)
            {
                return;  // No padding needed
            }

            unsigned int const numChannels =
              static_cast<unsigned int>(m_data.size()) / (m_width * m_height);

            std::vector<unsigned char> padded(newWidth * newHeight * numChannels, padValue);

            // Copy original data row by row
            for (unsigned int y = 0; y < m_height; ++y)
            {
                unsigned char const * srcRow = m_data.data() + y * m_width * numChannels;
                unsigned char * dstRow = padded.data() + y * newWidth * numChannels;
                std::copy(srcRow, srcRow + m_width * numChannels, dstRow);
            }

            m_data = std::move(padded);
            m_width = newWidth;
            m_height = newHeight;
        }

      private:
        ImageData m_data;
        unsigned int m_width{};
        unsigned int m_height{};
        PixelFormat m_format{};
        size_t m_bitDepth{8};
    };

    class ImageStack
    {
      public:
        ImageStack() = default;

        explicit ImageStack(ResourceId resourceId)
            : m_resourceId(resourceId)
        {
        }

        void setResourceId(ResourceId resourceId)
        {
            m_resourceId = resourceId;
        }

        ResourceId getResourceId() const
        {
            return m_resourceId;
        }

        auto begin() const
        {
            return m_stack.begin();
        }

        auto end() const
        {
            return m_stack.end();
        }

        auto size() const
        {
            return m_stack.size();
        }

        auto empty() const
        {
            return m_stack.empty();
        }

        auto front() const
        {
            return m_stack.front();
        }

        void push_back(Image const & image)
        {
            m_stack.push_back(image);
        }

        auto emplace_back(Image && image)
        {
            return m_stack.emplace_back(std::move(image));
        }

        void reserve(size_t size)
        {
            m_stack.reserve(size);
        }

        /// Get layer by index
        /// @param index 0-based layer index
        /// @throws std::out_of_range if index >= size()
        Image const & at(size_t index) const
        {
            return m_stack.at(index);
        }

        /// Get layer by index (mutable)
        /// @param index 0-based layer index
        /// @throws std::out_of_range if index >= size()
        Image & at(size_t index)
        {
            return m_stack.at(index);
        }

        /// Flip all layers horizontally (mirror along X axis)
        void flipHorizontal()
        {
            for (auto & image : m_stack)
            {
                image.flipHorizontal();
            }
        }

        /// Flip all layers vertically (mirror along Y axis)
        void flipVertical()
        {
            for (auto & image : m_stack)
            {
                image.flipVertical();
            }
        }

        /// Rotate all layers 90° clockwise
        /// @note Updates width/height if images are rectangular
        void rotate90CW()
        {
            for (auto & image : m_stack)
            {
                image.rotate90CW();
            }
        }

        /// Rotate all layers 90° counter-clockwise
        /// @note Updates width/height if images are rectangular
        void rotate90CCW()
        {
            for (auto & image : m_stack)
            {
                image.rotate90CCW();
            }
        }

      private:
        std::vector<Image> m_stack;
        ResourceId m_resourceId{};
    };
}