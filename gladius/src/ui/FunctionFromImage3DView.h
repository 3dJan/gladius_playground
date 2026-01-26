#pragma once

#include "ViewStates.h"
#include "kernel/types.h"

namespace gladius::nodes
{
    class Model;
    class Assembly;
    class ImageSampler;
}

namespace gladius::ui
{
    class ModelEditor;

    /// View for configuring FunctionFromImage3D sampling parameters
    class FunctionFromImage3DView
    {
      public:
        FunctionFromImage3DView();
        ~FunctionFromImage3DView();

        /// Set the function model to configure
        /// @param model Function model containing ImageSampler
        /// @param assembly Assembly containing resources
        void setFunction(nodes::Model * model, nodes::Assembly * assembly);

        /// Set ModelEditor for undo integration
        void setModelEditor(ModelEditor * editor);

        /// Render the configuration panel
        /// @return true if any settings were changed
        bool render();

        /// Force preview update
        void invalidatePreview();

      private:
        // Configuration accessors
        SamplingFilter getFilter() const;
        void setFilter(SamplingFilter filter);

        TextureTileStyle getTileStyleU() const;
        TextureTileStyle getTileStyleV() const;
        TextureTileStyle getTileStyleW() const;
        void setTileStyle(int axis, TextureTileStyle style);

        float getOffset() const;
        void setOffset(float offset);

        float getScale() const;
        void setScale(float scale);

        ResourceId getImageStackId() const;
        void setImageStackId(ResourceId id);

        // Internal helpers
        nodes::ImageSampler * findImageSampler();
        void renderPreview();
        void updatePreviewTexture();

        FunctionFromImage3DViewState m_state;
        nodes::Model * m_function = nullptr;
        nodes::Assembly * m_assembly = nullptr;
        ModelEditor * m_modelEditor = nullptr;
    };
}
