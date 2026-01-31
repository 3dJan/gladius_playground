# Data Model: Image3D & FunctionFromImage3D UI

**Date**: 2026-01-26  
**Branch**: `016-image3d-ui`

## Entity Diagram

```
+------------------+         +------------------------+
|    ImageStack    |         | FunctionFromImage3D    |
+------------------+         +------------------------+
| - resourceId     |<--------| - image3dId            |
| - layers[]       |         | - filter               |
+------------------+         | - tileStyleU/V/W       |
        |                    | - valueOffset          |
        v                    | - valueScale           |
+------------------+         +------------------------+
|     Image        |                   |
+------------------+                   |
| - data           |                   v
| - width          |         +------------------------+
| - height         |         |    ImageSampler        |
| - format         |         |    (internal node)     |
| - bitDepth       |         +------------------------+
+------------------+         | - filter               |
                             | - tileStyleU/V/W       |
                             | - uvw input            |
                             | - color/alpha outputs  |
                             +------------------------+
```

## Existing Entities (Reference)

### ImageStack

**Location**: `gladius/src/io/3mf/ImageStack.h`

```cpp
class ImageStack
{
    std::vector<Image> m_stack;
    ResourceId m_resourceId;
public:
    void setResourceId(ResourceId resourceId);
    ResourceId getResourceId() const;
    auto begin() const;
    auto end() const;
    auto size() const;
    bool empty() const;
    auto front() const;
    void push_back(Image const & image);
    auto emplace_back(Image && image);
    void reserve(size_t size);
};
```

### Image

**Location**: `gladius/src/io/3mf/ImageStack.h`

```cpp
enum class PixelFormat {
    GRAYSCALE_1BIT,
    RGBA_8BIT, RGB_8BIT, GRAYSCALE_8BIT, GRAYSCALE_ALPHA_8BIT,
    RGBA_16BIT, RGB_16BIT, GRAYSCALE_16BIT, GRAYSCALE_ALPHA_16BIT
};

class Image
{
    ImageData m_data;  // std::vector<unsigned char>
    unsigned int m_width;
    unsigned int m_height;
    PixelFormat m_format;
    size_t m_bitDepth;
public:
    ImageData const & getData() const;
    unsigned int getWidth() const;
    unsigned int getHeight() const;
    PixelFormat getFormat() const;
    void setFormat(PixelFormat format);
    size_t getBitDepth() const;
    void setBitDepth(size_t bitDepth);
    void swapXYData();  // Existing transform method
};
```

### SamplingSettings

**Location**: `gladius/src/nodes/SamplingSettings.h`

```cpp
enum TextureTileStyle { TTS_WRAP = 0, TTS_MIRROR = 1, TTS_CLAMP = 2 };
enum SamplingFilter { SF_NEAREST = 0, SF_LINEAR = 1 };

struct SamplingSettings {
    TextureTileStyle tileStyleU = TTS_WRAP;
    TextureTileStyle tileStyleV = TTS_WRAP;
    TextureTileStyle tileStyleW = TTS_WRAP;
    SamplingFilter filter = SF_LINEAR;
    float offset = 0.0f;
    float scale = 1.0f;
};
```

## New/Modified Entities

### ImageStack (Modified)

Add transform methods:

```cpp
class ImageStack
{
    // ... existing members ...
public:
    // NEW: Transform all layers
    void flipHorizontal();
    void flipVertical();
    void rotate90CW();    // Clockwise
    void rotate90CCW();   // Counter-clockwise
    
    // NEW: Access by index
    Image const & at(size_t index) const;
    Image & at(size_t index);
};
```

### ImageStackViewState (New)

UI state for the layer viewer:

```cpp
struct ImageStackViewState
{
    ResourceId imageStackId = 0;
    int currentLayerIndex = 0;
    float zoom = 1.0f;
    ImVec2 pan = {0.f, 0.f};
    
    // Cached texture handle for current layer
    GLuint textureId = 0;
    bool textureDirty = true;
};
```

### FunctionFromImage3DViewState (New)

UI state for the configuration panel:

```cpp
struct FunctionFromImage3DViewState
{
    ResourceId functionId = 0;
    ResourceId selectedImageStackId = 0;
    
    // Preview configuration
    int previewAxis = 2;        // 0=X, 1=Y, 2=Z (default Z-slice)
    float previewPosition = 0.5f;
    float previewRangeMin = -0.5f;  // Allow outside [0,1] for tile demo
    float previewRangeMax = 1.5f;
    
    // Cached preview texture
    GLuint previewTextureId = 0;
    bool previewDirty = true;
};
```

## State Management

### Current Model Detection

The ModelEditor needs to detect when the current model is a FunctionFromImage3D:

```cpp
bool ModelEditor::isFunctionFromImage3D(Model const * model) const
{
    if (!model) return false;
    // Check if model has the characteristic node structure:
    // - ImageSampler node
    // - Resource node connected to ImageSampler
    // - Standard begin/end with pos input, color/alpha outputs
    return model->contains<nodes::ImageSampler>();
}
```

### Configuration Update Flow

1. User changes setting in Properties panel
2. Find `ImageSampler` node in function graph
3. Update parameter on node
4. Trigger `createUndoRestorePoint()`
5. Mark model as dirty for recompile
6. Update preview texture

```cpp
void FunctionFromImage3DView::setFilter(SamplingFilter filter)
{
    if (auto* sampler = findImageSampler(m_currentFunction))
    {
        m_modelEditor.createUndoRestorePoint("Change filter mode");
        sampler->parameter().at(FieldNames::Filter) = VariantParameter(static_cast<int>(filter));
        m_previewDirty = true;
        m_modelEditor.markModelAsModified();
    }
}
```

## Validation Rules

### ImageStack

| Field | Validation | Error |
|-------|------------|-------|
| layers | Non-empty | "ImageStack must contain at least one layer" |
| layer dimensions | All layers same size or padded | Warning if padding applied |
| format | Supported formats only | "Unsupported pixel format" |

### FunctionFromImage3D

| Field | Validation | Error |
|-------|------------|-------|
| image3dId | Must reference valid ImageStack | "Referenced ImageStack not found" |
| filter | SF_NEAREST or SF_LINEAR | N/A (enum) |
| tileStyle* | TTS_WRAP, TTS_MIRROR, or TTS_CLAMP | N/A (enum) |
| scale | Non-zero | "Scale cannot be zero" |
