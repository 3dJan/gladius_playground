#include "ImageStackView.h"
#include "io/3mf/ImageStack.h"

#include <IconsFontAwesome6.h>
#include <glad/glad.h>
#include <algorithm>
#include <fmt/format.h>

namespace gladius::ui
{
    ImageStackView::ImageStackView() = default;

    ImageStackView::~ImageStackView()
    {
        cleanupTexture();
    }

    void ImageStackView::setImageStack(io::ImageStack const * stack)
    {
        if (m_imageStack != stack)
        {
            m_imageStack = stack;
            m_state.currentLayerIndex = 0;
            m_state.textureDirty = true;
            
            if (stack != nullptr)
            {
                m_state.imageStackId = stack->getResourceId();
            }
            else
            {
                m_state.imageStackId = 0;
                cleanupTexture();
            }
        }
    }

    int ImageStackView::getCurrentLayerIndex() const
    {
        return m_state.currentLayerIndex;
    }

    void ImageStackView::setCurrentLayerIndex(int index)
    {
        if (m_imageStack == nullptr || m_imageStack->empty())
        {
            m_state.currentLayerIndex = 0;
            return;
        }

        int const maxIndex = static_cast<int>(m_imageStack->size()) - 1;
        int const clampedIndex = std::clamp(index, 0, maxIndex);
        
        if (m_state.currentLayerIndex != clampedIndex)
        {
            m_state.currentLayerIndex = clampedIndex;
            m_state.textureDirty = true;
        }
    }

    void ImageStackView::setTransformCallback(TransformCallback callback)
    {
        m_transformCallback = std::move(callback);
    }

    void ImageStackView::invalidateTexture()
    {
        m_state.textureDirty = true;
    }

    void ImageStackView::renderTransformButtons()
    {
        // T059: Transform buttons (Flip H, Flip V, Rotate CW, Rotate CCW)
        bool const hasCallback = m_transformCallback != nullptr;

        ImGui::BeginDisabled(!hasCallback);

        if (ImGui::Button(reinterpret_cast<char const *>(ICON_FA_ARROWS_LEFT_RIGHT)))
        {
            if (m_transformCallback)
            {
                m_transformCallback(ImageStackTransform::FlipHorizontal);
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Flip Horizontal");
        }

        ImGui::SameLine();
        if (ImGui::Button(reinterpret_cast<char const *>(ICON_FA_ARROWS_UP_DOWN)))
        {
            if (m_transformCallback)
            {
                m_transformCallback(ImageStackTransform::FlipVertical);
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Flip Vertical");
        }

        ImGui::SameLine();
        if (ImGui::Button(reinterpret_cast<char const *>(ICON_FA_ROTATE_RIGHT)))
        {
            if (m_transformCallback)
            {
                m_transformCallback(ImageStackTransform::Rotate90CW);
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Rotate 90\xC2\xB0 Clockwise");
        }

        ImGui::SameLine();
        if (ImGui::Button(reinterpret_cast<char const *>(ICON_FA_ROTATE_LEFT)))
        {
            if (m_transformCallback)
            {
                m_transformCallback(ImageStackTransform::Rotate90CCW);
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Rotate 90\xC2\xB0 Counter-Clockwise");
        }

        ImGui::EndDisabled();
    }

    bool ImageStackView::render()
    {
        bool changed = false;
        m_hovered = false;

        if (m_imageStack == nullptr || m_imageStack->empty())
        {
            ImGui::TextDisabled("No ImageStack selected");
            return false;
        }

        // Upload texture if dirty
        if (m_state.textureDirty)
        {
            uploadLayerTexture();
            m_state.textureDirty = false;
        }

        int const layerCount = static_cast<int>(m_imageStack->size());
        auto const & currentImage = m_imageStack->at(static_cast<size_t>(m_state.currentLayerIndex));

        // T059: Transform buttons
        renderTransformButtons();

        ImGui::Separator();

        // Layer navigation slider (1-based display for user-friendliness)
        int displayIndex = m_state.currentLayerIndex + 1;
        std::string const label = fmt::format("Layer {} of {}", displayIndex, layerCount);
        
        ImGui::PushItemWidth(-1);
        if (ImGui::SliderInt("##LayerSlider", &displayIndex, 1, layerCount, label.c_str()))
        {
            setCurrentLayerIndex(displayIndex - 1);
            changed = true;
        }
        ImGui::PopItemWidth();

        // Calculate image display size with aspect ratio preservation
        ImVec2 const availableSize = ImGui::GetContentRegionAvail();
        float const imageWidth = static_cast<float>(currentImage.getWidth());
        float const imageHeight = static_cast<float>(currentImage.getHeight());
        
        if (imageWidth > 0.f && imageHeight > 0.f)
        {
            float const aspectRatio = imageWidth / imageHeight;
            float displayWidth = availableSize.x;
            float displayHeight = displayWidth / aspectRatio;

            // If height exceeds available space, scale by height instead
            if (displayHeight > availableSize.y)
            {
                displayHeight = availableSize.y;
                displayWidth = displayHeight * aspectRatio;
            }

            // Center the image horizontally
            float const offsetX = (availableSize.x - displayWidth) * 0.5f;
            if (offsetX > 0.f)
            {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
            }

            // Display the texture
            if (m_layerTexture != 0)
            {
                ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(m_layerTexture)),
                             ImVec2(displayWidth, displayHeight));
                
                m_hovered = ImGui::IsItemHovered();

                // Mouse wheel scrolling through layers
                if (m_hovered)
                {
                    float const wheel = ImGui::GetIO().MouseWheel;
                    if (wheel != 0.f)
                    {
                        int const delta = (wheel > 0.f) ? -1 : 1;
                        setCurrentLayerIndex(m_state.currentLayerIndex + delta);
                        changed = true;
                    }
                }
            }
        }

        // Display layer info
        ImGui::Text("Dimensions: %ux%u", currentImage.getWidth(), currentImage.getHeight());

        return changed;
    }

    bool ImageStackView::isHovered() const
    {
        return m_hovered;
    }

    void ImageStackView::uploadLayerTexture()
    {
        if (m_imageStack == nullptr || m_imageStack->empty())
        {
            return;
        }

        auto const & image = m_imageStack->at(static_cast<size_t>(m_state.currentLayerIndex));
        auto const & data = image.getData();
        
        if (data.empty())
        {
            return;
        }

        // Create texture if needed
        if (m_layerTexture == 0)
        {
            glGenTextures(1, &m_layerTexture);
        }

        glBindTexture(GL_TEXTURE_2D, m_layerTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Determine format based on pixel format
        GLenum format = GL_RGBA;
        GLenum internalFormat = GL_RGBA8;
        bool needsGrayscaleSwizzle = false;
        
        switch (image.getFormat())
        {
        case io::PixelFormat::GRAYSCALE_8BIT:
        case io::PixelFormat::GRAYSCALE_1BIT:
            format = GL_RED;
            internalFormat = GL_R8;
            needsGrayscaleSwizzle = true;
            break;
        case io::PixelFormat::GRAYSCALE_ALPHA_8BIT:
            format = GL_RG;
            internalFormat = GL_RG8;
            needsGrayscaleSwizzle = true;
            break;
        case io::PixelFormat::RGB_8BIT:
            format = GL_RGB;
            internalFormat = GL_RGB8;
            break;
        case io::PixelFormat::RGBA_8BIT:
        default:
            format = GL_RGBA;
            internalFormat = GL_RGBA8;
            break;
        case io::PixelFormat::GRAYSCALE_16BIT:
            format = GL_RED;
            internalFormat = GL_R16;
            needsGrayscaleSwizzle = true;
            break;
        case io::PixelFormat::GRAYSCALE_ALPHA_16BIT:
            format = GL_RG;
            internalFormat = GL_RG16;
            needsGrayscaleSwizzle = true;
            break;
        case io::PixelFormat::RGB_16BIT:
            format = GL_RGB;
            internalFormat = GL_RGB16;
            break;
        case io::PixelFormat::RGBA_16BIT:
            format = GL_RGBA;
            internalFormat = GL_RGBA16;
            break;
        }

        // For grayscale textures, set swizzle to replicate R channel to RGB
        if (needsGrayscaleSwizzle)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
        }
        else
        {
            // Reset to default swizzle for non-grayscale
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
        }

        GLenum const dataType = (image.getBitDepth() == 16) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;

        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     static_cast<GLint>(internalFormat),
                     static_cast<GLsizei>(image.getWidth()),
                     static_cast<GLsizei>(image.getHeight()),
                     0,
                     format,
                     dataType,
                     data.data());

        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void ImageStackView::cleanupTexture()
    {
        if (m_layerTexture != 0)
        {
            glDeleteTextures(1, &m_layerTexture);
            m_layerTexture = 0;
        }
    }
}
