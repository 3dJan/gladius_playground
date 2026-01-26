/// @file FunctionFromImage3DView.cpp
/// @brief Implementation of FunctionFromImage3DView widget

#include "FunctionFromImage3DView.h"

#include "Document.h"
#include "ImageStackResource.h"
#include "ModelEditor.h"
#include "ResourceManager.h"
#include "nodes/DerivedNodes.h"
#include "nodes/Model.h"

#include "imgui.h"

#include <chrono>
#include <fmt/format.h>
#include <glad/glad.h>

namespace gladius::ui
{
    namespace
    {
        char const * filterNames[] = {"Nearest", "Linear"};
        char const * tileStyleNames[] = {"Repeat", "Mirror", "Clamp", "None"};
    }

    FunctionFromImage3DView::FunctionFromImage3DView() = default;

    FunctionFromImage3DView::~FunctionFromImage3DView() = default;

    void FunctionFromImage3DView::setFunction(nodes::Model * model, nodes::Assembly * assembly)
    {
        m_function = model;
        m_assembly = assembly;
    }

    void FunctionFromImage3DView::setModelEditor(ModelEditor * editor)
    {
        m_modelEditor = editor;
    }

    nodes::ImageSampler * FunctionFromImage3DView::findImageSampler()
    {
        if (!m_function)
        {
            return nullptr;
        }

        for (auto & [id, node] : *m_function)
        {
            if (auto * sampler = dynamic_cast<nodes::ImageSampler *>(node.get()))
            {
                return sampler;
            }
        }
        return nullptr;
    }

    SamplingFilter FunctionFromImage3DView::getFilter() const
    {
        if (!m_function)
        {
            return SF_LINEAR;
        }

        // Const-cast for iteration - safe since we're just reading
        for (auto & [id, node] : *const_cast<nodes::Model *>(m_function))
        {
            if (auto * sampler = dynamic_cast<nodes::ImageSampler *>(node.get()))
            {
                return sampler->getFilter();
            }
        }
        return SF_LINEAR;
    }

    void FunctionFromImage3DView::setFilter(SamplingFilter filter)
    {
        auto * sampler = findImageSampler();
        if (!sampler)
        {
            return;
        }
        sampler->parameter().at(nodes::FieldNames::Filter).setValue(static_cast<int>(filter));
    }

    TextureTileStyle FunctionFromImage3DView::getTileStyleU() const
    {
        if (!m_function)
        {
            return TTS_REPEAT;
        }

        for (auto & [id, node] : *const_cast<nodes::Model *>(m_function))
        {
            if (auto * sampler = dynamic_cast<nodes::ImageSampler *>(node.get()))
            {
                return sampler->getTileStyleU();
            }
        }
        return TTS_REPEAT;
    }

    TextureTileStyle FunctionFromImage3DView::getTileStyleV() const
    {
        if (!m_function)
        {
            return TTS_REPEAT;
        }

        for (auto & [id, node] : *const_cast<nodes::Model *>(m_function))
        {
            if (auto * sampler = dynamic_cast<nodes::ImageSampler *>(node.get()))
            {
                return sampler->getTileStyleV();
            }
        }
        return TTS_REPEAT;
    }

    TextureTileStyle FunctionFromImage3DView::getTileStyleW() const
    {
        if (!m_function)
        {
            return TTS_REPEAT;
        }

        for (auto & [id, node] : *const_cast<nodes::Model *>(m_function))
        {
            if (auto * sampler = dynamic_cast<nodes::ImageSampler *>(node.get()))
            {
                return sampler->getTileStyleW();
            }
        }
        return TTS_REPEAT;
    }

    void FunctionFromImage3DView::setTileStyle(int axis, TextureTileStyle style)
    {
        auto * sampler = findImageSampler();
        if (!sampler)
        {
            return;
        }

        char const * field = nodes::FieldNames::TileStyleU;
        switch (axis)
        {
        case 0:
            field = nodes::FieldNames::TileStyleU;
            break;
        case 1:
            field = nodes::FieldNames::TileStyleV;
            break;
        case 2:
            field = nodes::FieldNames::TileStyleW;
            break;
        default:
            return;
        }
        sampler->parameter().at(field).setValue(static_cast<int>(style));
    }

    float FunctionFromImage3DView::getOffset() const
    {
        // Offset is typically stored in a Constant node connected to ImageSampler
        // Return stored value from render loop
        return 0.0f;
    }

    void FunctionFromImage3DView::setOffset(float offset)
    {
        // TODO: Find the Offset constant node and update it
        (void)offset;
    }

    float FunctionFromImage3DView::getScale() const
    {
        // Scale is typically stored in a Constant node
        return 1.0f;
    }

    void FunctionFromImage3DView::setScale(float scale)
    {
        // TODO: Find the Scale constant node and update it
        (void)scale;
    }

    ResourceId FunctionFromImage3DView::getImageStackId() const
    {
        if (!m_function)
        {
            return 0;
        }

        for (auto & [id, node] : *const_cast<nodes::Model *>(m_function))
        {
            if (auto * sampler = dynamic_cast<nodes::ImageSampler *>(node.get()))
            {
                try
                {
                    return sampler->getImageResourceId();
                }
                catch (...)
                {
                    return 0;
                }
            }
        }
        return 0;
    }

    void FunctionFromImage3DView::setImageStackId(ResourceId id)
    {
        // Setting the ImageStack requires finding and updating the Resource node
        // connected to the ImageSampler's ResourceId input
        (void)id; // TODO: Implement in US3
    }

    void FunctionFromImage3DView::invalidatePreview()
    {
        m_state.previewDirty = true;
    }

    void FunctionFromImage3DView::renderPreview()
    {
        // T043: Display 2D slice preview
        ImGui::Text("Preview:");

        // T044: Slice position slider with extended range for tile demo
        if (ImGui::SliderFloat("Slice Position",
                               &m_state.previewPosition,
                               m_state.previewRangeMin,
                               m_state.previewRangeMax,
                               "%.2f"))
        {
            invalidatePreview();
        }

        // Axis selector
        char const * axisNames[] = {"X (YZ plane)", "Y (XZ plane)", "Z (XY plane)"};
        if (ImGui::Combo("Slice Axis", &m_state.previewAxis, axisNames, 3))
        {
            invalidatePreview();
        }

        // T046: Throttle preview updates
        auto now = std::chrono::steady_clock::now();
        double currentTimeMs =
            std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();

        if (m_state.previewDirty &&
            (currentTimeMs - m_state.lastPreviewUpdateTime >=
             FunctionFromImage3DViewState::previewUpdateIntervalMs))
        {
            updatePreviewTexture();
            m_state.lastPreviewUpdateTime = currentTimeMs;
            m_state.previewDirty = false;
        }

        // Display the preview texture
        if (m_state.previewTextureId != 0)
        {
            ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(m_state.previewTextureId)),
                         ImVec2(static_cast<float>(m_state.previewWidth),
                                static_cast<float>(m_state.previewHeight)));
        }
        else
        {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No preview available");
        }
    }

    void FunctionFromImage3DView::updatePreviewTexture()
    {
        // T042: Sample the ImageStack at the current slice position
        if (!m_modelEditor)
        {
            return;
        }

        auto doc = m_modelEditor->getDocument();
        if (!doc || !doc->getCore())
        {
            return;
        }

        // Get the current ImageStack
        ResourceId const stackId = getImageStackId();
        if (stackId == 0)
        {
            return;
        }

        auto & resourceManager = doc->getGeneratorContext().resourceManager;
        auto const & resources = resourceManager.getResourceMap();

        io::ImageStack const * imageStack = nullptr;
        for (auto const & [key, res] : resources)
        {
            if (key.getResourceId().has_value() && key.getResourceId().value() == stackId)
            {
                auto const * stackRes = dynamic_cast<ImageStackResource const *>(res.get());
                if (stackRes)
                {
                    imageStack = stackRes->getImageStack();
                    break;
                }
            }
        }

        if (!imageStack || imageStack->empty())
        {
            return;
        }

        // Calculate which layer to show based on slice position
        size_t const numLayers = imageStack->size();
        float const normalizedPos = (m_state.previewPosition - m_state.previewRangeMin) /
                                    (m_state.previewRangeMax - m_state.previewRangeMin);

        // Handle tiling for out-of-bounds positions
        float wrappedPos = normalizedPos;
        if (wrappedPos < 0.0f || wrappedPos >= 1.0f)
        {
            // Apply wrap based on current tile style
            TextureTileStyle const tileW = getTileStyleW();
            if (tileW == TTS_REPEAT)
            {
                wrappedPos = wrappedPos - std::floor(wrappedPos);
            }
            else if (tileW == TTS_MIRROR)
            {
                int const period = static_cast<int>(std::floor(wrappedPos));
                wrappedPos = wrappedPos - std::floor(wrappedPos);
                if (period % 2 != 0)
                {
                    wrappedPos = 1.0f - wrappedPos;
                }
            }
            else if (tileW == TTS_CLAMP)
            {
                wrappedPos = std::clamp(wrappedPos, 0.0f, 0.999f);
            }
            else
            {
                // TTS_NONE - show nothing outside bounds
                if (m_state.previewTextureId != 0)
                {
                    glDeleteTextures(1, &m_state.previewTextureId);
                    m_state.previewTextureId = 0;
                }
                return;
            }
        }

        size_t const layerIndex =
            std::min(static_cast<size_t>(wrappedPos * numLayers), numLayers - 1);

        auto const & layer = imageStack->at(layerIndex);
        unsigned int const width = layer.getWidth();
        unsigned int const height = layer.getHeight();
        auto const & pixels = layer.getData();

        if (pixels.empty() || width == 0 || height == 0)
        {
            return;
        }

        // Create or update OpenGL texture
        if (m_state.previewTextureId == 0)
        {
            glGenTextures(1, &m_state.previewTextureId);
        }

        glBindTexture(GL_TEXTURE_2D, m_state.previewTextureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        getFilter() == SF_NEAREST ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        getFilter() == SF_NEAREST ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Determine format based on pixel format
        GLenum format = GL_RED;
        GLenum internalFormat = GL_R8;
        switch (layer.getFormat())
        {
        case io::PixelFormat::GRAYSCALE_8BIT:
        case io::PixelFormat::GRAYSCALE_1BIT:
            format = GL_RED;
            internalFormat = GL_R8;
            break;
        case io::PixelFormat::RGB_8BIT:
        case io::PixelFormat::RGB_16BIT:
            format = GL_RGB;
            internalFormat = GL_RGB8;
            break;
        case io::PixelFormat::RGBA_8BIT:
        case io::PixelFormat::RGBA_16BIT:
            format = GL_RGBA;
            internalFormat = GL_RGBA8;
            break;
        case io::PixelFormat::GRAYSCALE_ALPHA_8BIT:
        case io::PixelFormat::GRAYSCALE_ALPHA_16BIT:
            format = GL_RG;
            internalFormat = GL_RG8;
            break;
        case io::PixelFormat::GRAYSCALE_16BIT:
            format = GL_RED;
            internalFormat = GL_R16;
            break;
        }

        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     static_cast<GLint>(internalFormat),
                     static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height),
                     0,
                     format,
                     GL_UNSIGNED_BYTE,
                     pixels.data());

        m_state.previewWidth = static_cast<int>(width);
        m_state.previewHeight = static_cast<int>(height);

        glBindTexture(GL_TEXTURE_2D, 0);
    }

    bool FunctionFromImage3DView::render()
    {
        bool changed = false;

        auto * sampler = findImageSampler();
        if (!sampler)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                               "No ImageSampler found in this function");
            return false;
        }

        // ImageStack selection dropdown (T036-T040)
        ImGui::Text("Image Stack:");
        if (m_modelEditor)
        {
            auto doc = m_modelEditor->getDocument();
            if (doc && doc->getCore())
            {
                auto & resourceManager = doc->getGeneratorContext().resourceManager;
                auto const & resources = resourceManager.getResourceMap();

                // Collect available ImageStacks
                std::vector<std::pair<ResourceKey, ImageStackResource const *>> imageStacks;
                for (auto const & [key, res] : resources)
                {
                    auto const * stack = dynamic_cast<ImageStackResource const *>(res.get());
                    if (stack)
                    {
                        imageStacks.emplace_back(key, stack);
                    }
                }

                if (imageStacks.empty())
                {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No ImageStacks available");
                }
                else
                {
                    // Get current selection
                    ResourceId const currentStackId = getImageStackId();

                    // Build combo preview string
                    std::string previewStr = "Select ImageStack...";
                    int currentIndex = -1;
                    for (size_t i = 0; i < imageStacks.size(); ++i)
                    {
                        auto const & [key, stack] = imageStacks[i];
                        if (key.getResourceId().has_value() &&
                            key.getResourceId().value() == currentStackId)
                        {
                            previewStr = key.getDisplayName();
                            currentIndex = static_cast<int>(i);
                            break;
                        }
                    }

                    if (ImGui::BeginCombo("##ImageStack", previewStr.c_str()))
                    {
                        for (size_t i = 0; i < imageStacks.size(); ++i)
                        {
                            auto const & [key, stack] = imageStacks[i];
                            auto const displayName = fmt::format("{} ({}x{}x{})",
                                                                 key.getDisplayName(),
                                                                 stack->getWidth(),
                                                                 stack->getHeight(),
                                                                 stack->getNumSheets());

                            bool const isSelected = (static_cast<int>(i) == currentIndex);
                            if (ImGui::Selectable(displayName.c_str(), isSelected))
                            {
                                if (key.getResourceId().has_value())
                                {
                                    if (m_modelEditor)
                                    {
                                        m_modelEditor->createUndoRestorePoint(
                                            "Change ImageStack");
                                    }
                                    setImageStackId(key.getResourceId().value());
                                    if (m_modelEditor)
                                    {
                                        m_modelEditor->markModelAsModified();
                                    }
                                    changed = true;
                                }
                            }

                            // Tooltip preview on hover (T040)
                            if (ImGui::IsItemHovered() && stack->getImageStack())
                            {
                                ImGui::BeginTooltip();
                                ImGui::Text("Dimensions: %zux%zux%zu",
                                            stack->getWidth(),
                                            stack->getHeight(),
                                            stack->getNumSheets());
                                ImGui::Text("Channels: %zu", stack->getNumChannels());
                                ImGui::EndTooltip();
                            }

                            if (isSelected)
                            {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No document loaded");
            }
        }

        ImGui::Separator();

        // Filter mode combo
        int currentFilter = static_cast<int>(getFilter());
        if (ImGui::Combo("Filter", &currentFilter, filterNames, IM_ARRAYSIZE(filterNames)))
        {
            if (m_modelEditor)
            {
                m_modelEditor->createUndoRestorePoint("Change filter mode");
            }
            setFilter(static_cast<SamplingFilter>(currentFilter));
            if (m_modelEditor)
            {
                m_modelEditor->markModelAsModified();
            }
            changed = true;
        }

        ImGui::Separator();
        ImGui::Text("Tile Style:");

        // Tile style U
        int tileU = static_cast<int>(getTileStyleU());
        if (ImGui::Combo("U Axis", &tileU, tileStyleNames, IM_ARRAYSIZE(tileStyleNames)))
        {
            if (m_modelEditor)
            {
                m_modelEditor->createUndoRestorePoint("Change U tile style");
            }
            setTileStyle(0, static_cast<TextureTileStyle>(tileU));
            if (m_modelEditor)
            {
                m_modelEditor->markModelAsModified();
            }
            changed = true;
        }

        // Tile style V
        int tileV = static_cast<int>(getTileStyleV());
        if (ImGui::Combo("V Axis", &tileV, tileStyleNames, IM_ARRAYSIZE(tileStyleNames)))
        {
            if (m_modelEditor)
            {
                m_modelEditor->createUndoRestorePoint("Change V tile style");
            }
            setTileStyle(1, static_cast<TextureTileStyle>(tileV));
            if (m_modelEditor)
            {
                m_modelEditor->markModelAsModified();
            }
            changed = true;
        }

        // Tile style W
        int tileW = static_cast<int>(getTileStyleW());
        if (ImGui::Combo("W Axis", &tileW, tileStyleNames, IM_ARRAYSIZE(tileStyleNames)))
        {
            if (m_modelEditor)
            {
                m_modelEditor->createUndoRestorePoint("Change W tile style");
            }
            setTileStyle(2, static_cast<TextureTileStyle>(tileW));
            if (m_modelEditor)
            {
                m_modelEditor->markModelAsModified();
            }
            changed = true;
        }

        ImGui::Separator();

        // Offset (stored in local state for now, actual node update TODO for later)
        static float offset = 0.0f;
        if (ImGui::DragFloat("Offset", &offset, 0.01f))
        {
            if (m_modelEditor)
            {
                m_modelEditor->createUndoRestorePoint("Change offset");
            }
            setOffset(offset);
            if (m_modelEditor)
            {
                m_modelEditor->markModelAsModified();
            }
            changed = true;
        }

        // Scale (stored in local state for now, actual node update TODO for later)
        static float scale = 1.0f;
        if (ImGui::DragFloat("Scale", &scale, 0.01f))
        {
            if (m_modelEditor)
            {
                m_modelEditor->createUndoRestorePoint("Change scale");
            }
            setScale(scale);
            if (m_modelEditor)
            {
                m_modelEditor->markModelAsModified();
            }
            changed = true;
        }

        ImGui::Separator();

        // T043-T046: Preview section
        renderPreview();

        // T045: Invalidate preview when settings change
        if (changed)
        {
            invalidatePreview();
        }

        return changed;
    }
}
