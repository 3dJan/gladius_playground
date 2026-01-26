#pragma once

#include "ResourceKey.h"
#include "imgui.h"

namespace gladius::ui
{
    /// UI state for the ImageStack layer viewer
    struct ImageStackViewState
    {
        ResourceId imageStackId = 0;
        int currentLayerIndex = 0;
        float zoom = 1.0f;
        ImVec2 pan = {0.f, 0.f};

        // Cached texture handle for current layer
        unsigned int textureId = 0;
        bool textureDirty = true;
    };

    /// UI state for the FunctionFromImage3D configuration panel
    struct FunctionFromImage3DViewState
    {
        ResourceId functionId = 0;
        ResourceId selectedImageStackId = 0;

        // Preview configuration
        int previewAxis = 2;          // 0=X, 1=Y, 2=Z (default Z-slice)
        float previewPosition = 0.5f;
        float previewRangeMin = -0.5f;  // Allow outside [0,1] for tile demo
        float previewRangeMax = 1.5f;

        // Cached preview texture
        unsigned int previewTextureId = 0;
        bool previewDirty = true;
    };
}
