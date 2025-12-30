
#pragma once

#include "ComputeContext.h"

#include "ImageRGBA.h"

#include <iostream>

namespace gladius
{
    class GLImageBuffer final : public ImageRGBA
    {
      public:
        explicit GLImageBuffer(ComputeContext & context)
            : ImageRGBA(context)
        {
        }

        GLImageBuffer(ComputeContext & context, size_t width, size_t height)
            : ImageRGBA(context, width, height)
        {
        }

        ~GLImageBuffer() override;

        void allocateOnDevice() override;

        void bind();

        static void unbind();

        [[nodiscard]] GLuint GetTextureId() const;

        void transferPixelInReadPixelMode();

        void invalidateContent();

        /// Clears the texture to a solid color (default: dark gray)
        void clear(float r = 0.1f, float g = 0.1f, float b = 0.1f, float a = 1.0f);

      private:
        void setupForInterOp();
        void setupForReadPixel();

        void transferPixels();

        GLuint m_textureID = 0;
        bool m_dirty{true};
    };
}
