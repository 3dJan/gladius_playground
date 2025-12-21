# Plan: Color-Aware 3MF Mesh Export with Material Zone Mapping

## Overview

This plan outlines the implementation of **volumetric color to material zone mapping** for 3MF mesh exports in Gladius. The feature leverages Gladius's GPU-based volumetric color evaluation to sample face colors during mesh generation, then maps those colors to discrete **material zones** for downstream manufacturing processes.

The system is designed to be **technology-agnostic**, supporting various output scenarios:

| Technology | Material Zone Usage |
|------------|---------------------|
| FDM multi-extruder | Extruder/filament assignment (MMU, AMS, tool changer) |
| Resin multi-material | Material vat selection |
| Full-color printing | Color palette indices (binder jetting, PolyJet) |
| Laser marking/engraving | Power level or pass assignment |
| CNC machining | Tool selection |
| Generic workflows | Named regions for downstream processing |

---

## 1. Problem Statement

### Current State
- Gladius supports volumetric/implicit color definitions via GPU evaluation
- Mesh export produces geometry without color/material information
- Various manufacturing tools expect per-triangle material assignments

### Goal
- Export meshes with per-face **material zone** assignments based on volumetric color sampling
- Provide intuitive UI for mapping RGB colors/ranges to named material zones
- Support multiple output formats and manufacturing technologies
- Persist material configurations across sessions

---

## 2. User Experience Design

### 2.1 Export Dialog Integration

**New "Material Zones" Tab in Mesh Export Dialog:**

```
┌─────────────────────────────────────────────────────────────────┐
│ Export Mesh                                              [X]    │
├─────────────────────────────────────────────────────────────────┤
│ [Geometry] [Material Zones] [Advanced]                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ☑ Enable Material Zone Export                                  │
│                                                                 │
│  Output Profile: [FDM Multi-Extruder ▼]                        │
│  Preset: [My Prusa MMU Setup ▼]  [Save] [Save As] [Delete]     │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │ Color → Material Zone Mapping                       [+ Add] ││
│  ├─────────────────────────────────────────────────────────────┤│
│  │ #  │ Source Color │ Tolerance │ Zone        │ Faces        ││
│  ├────┼──────────────┼───────────┼─────────────┼──────────────┤│
│  │ 1  │ [■] #FF0000  │ 15%       │ Zone 0      │ 12,450       ││
│  │ 2  │ [■] #00FF00  │ 10%       │ Zone 1      │ 8,230        ││
│  │ 3  │ [■] #0000FF  │ 10%       │ Zone 2      │ 5,120        ││
│  │ 4  │ [■] Default  │ —         │ Zone 0      │ 24,200       ││
│  └─────────────────────────────────────────────────────────────┘│
│                                                                 │
│  Color Matching Mode:  ○ Nearest   ● Tolerance   ○ Range        │
│  Unmapped Color:       [Zone 0 ▼] (Default Zone)                │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │ [Auto-Detect Colors]  [Preview in 3D]  [Import Palette]     ││
│  └─────────────────────────────────────────────────────────────┘│
│                                                                 │
│  Statistics:                                                    │
│  • Total faces: 50,000                                          │
│  • Mapped: 25,800 (51.6%)   Unmapped: 24,200 (48.4%)           │
│                                                                 │
├─────────────────────────────────────────────────────────────────┤
│                              [Cancel]  [Export]                 │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 Output Profiles

Output profiles define how material zones are exported for different technologies:

| Profile | Zone Semantics | Export Format | Typical Use |
|---------|---------------|---------------|-------------|
| **FDM Multi-Extruder** | Extruder index (T0, T1...) | 3MF with materials | PrusaSlicer, OrcaSlicer, Cura |
| **Resin Multi-Material** | Material vat ID | 3MF or custom | Multi-material SLA/LCD |
| **Full Color (Indexed)** | Palette index | 3MF with colors | PolyJet, binder jetting |
| **Full Color (Direct)** | Preserve RGB | 3MF with vertex colors | Full-color 3D printers |
| **Laser/Engraving** | Power level (0-100%) | Custom/SVG layers | Laser cutters, engravers |
| **CNC Tooling** | Tool number | Custom | CNC machining |
| **Generic Zones** | Named regions | 3MF with metadata | Downstream processing |

### 2.3 Workflow Options

**Option A: Manual Color Picking**
1. User clicks "Add" to add a mapping rule
2. Color picker opens (can also eyedrop from 3D view)
3. User sets tolerance and assigns material zone
4. Live preview updates face count

**Option B: Auto-Detect Colors**
1. User clicks "Auto-Detect Colors"
2. System samples N random faces via GPU
3. Clusters colors using k-means or similar
4. Suggests mapping rules based on detected palette
5. User refines and assigns zones

**Option C: Import Palette**
1. User imports a JSON/CSV palette file
2. Palette contains color-to-zone mappings
3. Useful for batch processing or team workflows

**Option D: Load Preset**
1. User selects a previously saved preset from dropdown
2. All mapping rules and settings are restored
3. User can modify and re-save

### 2.4 3D Preview Mode

- Toggle to visualize zone assignments in viewport
- Each zone shown in a distinct preview color
- Highlights unmapped faces in warning color (yellow stripes?)
- Useful for verifying mapping before export

---

## 3. Persistence Architecture

### 3.1 What to Persist

| Data | Scope | Storage |
|------|-------|---------|
| Output profiles | Built-in + User-defined | User config |
| Material zone definitions | Per-profile | User config |
| Mapping presets | Global | User config |
| Last used preset | Per-project | Project file |
| Custom mapping (unsaved) | Per-project | Project file |

### 3.2 Data Structures for Persistence

```cpp
/// Defines how material zones are interpreted for a specific technology
struct OutputProfile
{
    std::string id;                    ///< Unique identifier
    std::string name;                  ///< User-friendly name
    std::string description;           ///< Usage description
    bool isBuiltIn = false;            ///< System-provided vs user-created
    
    /// How zones are semantically interpreted
    enum class ZoneSemantics
    {
        Indexed,           ///< Simple 0-based index (extruder, tool, etc.)
        Named,             ///< Named regions with metadata
        Continuous,        ///< Continuous value (power level, opacity)
        DirectColor        ///< Preserve original RGB color
    };
    ZoneSemantics semantics = ZoneSemantics::Indexed;
    
    /// Export format options
    enum class ExportFormat
    {
        ThreeMF_Materials,     ///< 3MF with material extension
        ThreeMF_Colors,        ///< 3MF with color group
        ThreeMF_VertexColors,  ///< 3MF with per-vertex colors
        SeparateMeshes,        ///< One mesh file per zone
        CustomMetadata         ///< 3MF with custom metadata extension
    };
    ExportFormat format = ExportFormat::ThreeMF_Materials;
    
    int maxZones = 16;                 ///< Maximum zones supported
    bool supportsZoneNames = true;     ///< Can zones have custom names?
    bool supportsZoneColors = true;    ///< Can zones have display colors?
    
    /// Format-specific options (stored as JSON)
    std::string formatOptions;         
};

/// Represents a single material zone (technology-agnostic)
struct MaterialZone
{
    std::string id;                    ///< Unique identifier
    std::string name;                  ///< User-friendly name (e.g., "Red PLA", "Tool 1")
    int index;                         ///< 0-based zone index
    Eigen::Vector3f displayColor;      ///< Color shown in UI/preview (sRGB)
    
    /// Optional technology-specific metadata
    std::map<std::string, std::string> metadata;
    
    /// For continuous semantics (e.g., laser power)
    float continuousValue = 0.0f;      ///< Value in [0, 1] range
};

/// Represents a color-to-zone mapping rule
struct ZoneMappingRule
{
    std::string name;                  ///< User-friendly name (optional)
    Eigen::Vector3f sourceColor;       ///< Target color in linear RGB [0,1]
    float tolerance;                   ///< Color distance tolerance (0-1 normalized)
    std::string zoneId;                ///< Reference to MaterialZone
    int priority;                      ///< Higher priority rules match first
    
    /// Match mode for this rule
    enum class MatchMode
    {
        Exact,          ///< Exact color match within tolerance
        Range,          ///< Color falls within RGB range
        Nearest,        ///< Nearest neighbor (used with priority)
        Channel         ///< Match based on single channel (R, G, B, or Alpha)
    };
    MatchMode mode = MatchMode::Exact;
    
    /// For Range mode: min/max bounds
    Eigen::Vector3f rangeMin;
    Eigen::Vector3f rangeMax;
    
    /// For Channel mode
    int channelIndex = 0;              ///< 0=R, 1=G, 2=B, 3=A
    float channelMin = 0.0f;
    float channelMax = 1.0f;
};

/// A complete, named mapping configuration (preset)
struct ZoneMappingPreset
{
    std::string id;                        ///< Unique identifier (UUID)
    std::string name;                      ///< User-friendly name
    std::string description;               ///< Optional description
    std::string profileId;                 ///< Associated output profile
    
    std::vector<MaterialZone> zones;       ///< Zone definitions for this preset
    std::vector<ZoneMappingRule> rules;    ///< Mapping rules
    std::string defaultZoneId;             ///< Zone for unmapped colors
    
    /// Global matching strategy
    enum class MatchStrategy
    {
        FirstMatch,    ///< First rule that matches wins (priority order)
        NearestMatch,  ///< Closest color match wins
        Threshold      ///< Must be within tolerance, else default
    };
    MatchStrategy strategy = MatchStrategy::Threshold;
    
    /// Color space for distance calculation
    enum class ColorSpace
    {
        LinearRgb,     ///< Euclidean distance in linear RGB
        SrgbLab,       ///< Delta-E in CIE Lab (perceptually uniform)
        Hsv,           ///< Distance in HSV (good for saturated colors)
        Hsl            ///< Distance in HSL
    };
    ColorSpace colorSpace = ColorSpace::SrgbLab;
    
    /// Metadata
    std::string createdAt;             ///< ISO 8601 timestamp
    std::string modifiedAt;            ///< ISO 8601 timestamp
};

/// Global configuration
struct MaterialZoneConfiguration
{
    std::vector<OutputProfile> profiles;       ///< Available output profiles
    std::vector<ZoneMappingPreset> presets;    ///< Saved presets
    std::string lastUsedProfileId;
    std::string lastUsedPresetId;
};
```

### 3.3 Storage Locations

```
~/.config/gladius/                    # Linux
%APPDATA%/gladius/                    # Windows
~/Library/Application Support/gladius/ # macOS

├── config.json                       # Main application config
├── output_profiles.json              # Output profile definitions
├── zone_presets/                     # Mapping presets
│   ├── fdm_prusa_mmu_default.json
│   ├── fdm_bambu_ams_setup.json
│   ├── laser_power_levels.json
│   └── custom_preset_xyz.json
└── filaments/                        # Optional filament library
    └── filament_library.json
```

### 3.4 JSON Schema for Persistence

**output_profiles.json:**
```json
{
  "version": 1,
  "profiles": [
    {
      "id": "fdm_multi_extruder",
      "name": "FDM Multi-Extruder",
      "description": "For multi-material FDM printers (MMU, AMS, tool changers)",
      "is_built_in": true,
      "semantics": "indexed",
      "format": "threemf_materials",
      "max_zones": 16,
      "supports_zone_names": true,
      "supports_zone_colors": true
    },
    {
      "id": "laser_power",
      "name": "Laser Power Levels",
      "description": "Map colors to laser power percentages",
      "is_built_in": true,
      "semantics": "continuous",
      "format": "separate_meshes",
      "max_zones": 256,
      "supports_zone_names": true,
      "supports_zone_colors": false
    },
    {
      "id": "full_color_direct",
      "name": "Full Color (Direct RGB)",
      "description": "Preserve volumetric colors as vertex colors",
      "is_built_in": true,
      "semantics": "direct_color",
      "format": "threemf_vertex_colors",
      "max_zones": 0,
      "supports_zone_names": false,
      "supports_zone_colors": false
    }
  ]
}
```

**zone_presets/my_preset.json:**
```json
{
  "version": 1,
  "id": "preset_abc123",
  "name": "My MMU Setup",
  "description": "Color mapping for my lithophane project",
  "profile_id": "fdm_multi_extruder",
  "created_at": "2025-12-02T10:30:00Z",
  "modified_at": "2025-12-02T14:45:00Z",
  "strategy": "threshold",
  "color_space": "lab",
  "default_zone_id": "zone_0",
  "zones": [
    {
      "id": "zone_0",
      "name": "Prusament Galaxy Black",
      "index": 0,
      "display_color_srgb": [0.1, 0.1, 0.12],
      "metadata": { "filament_id": "prusament_galaxy_black" }
    },
    {
      "id": "zone_1",
      "name": "Prusament Orange",
      "index": 1,
      "display_color_srgb": [1.0, 0.5, 0.1],
      "metadata": {}
    }
  ],
  "rules": [
    {
      "name": "Red regions",
      "source_color_srgb": [1.0, 0.0, 0.0],
      "tolerance": 0.15,
      "zone_id": "zone_0",
      "priority": 1,
      "mode": "exact"
    },
    {
      "name": "Green regions",
      "source_color_srgb": [0.0, 1.0, 0.0],
      "tolerance": 0.10,
      "zone_id": "zone_1",
      "priority": 2,
      "mode": "exact"
    }
  ]
}
```

### 3.5 Project-Level Storage

For per-project overrides, store in the Gladius project file (.gladius or embedded in 3MF):

```json
{
  "material_zone_export": {
    "enabled": true,
    "profile_id": "fdm_multi_extruder",
    "preset_id": "preset_abc123",
    "override_rules": [],
    "last_export_stats": {
      "total_faces": 50000,
      "mapped_faces": 25800,
      "timestamp": "2025-12-02T15:00:00Z"
    }
  }
}
```

### 3.6 Configuration Manager

```cpp
/// Manages loading/saving of output profiles and zone presets
class MaterialZoneConfigManager
{
public:
    MaterialZoneConfigManager(std::filesystem::path const& configDir);
    
    /// Load all configurations on startup
    void loadAll();
    
    /// Save current state
    void saveAll();
    
    // Profile management
    std::vector<OutputProfile> const& getProfiles() const;
    OutputProfile const* getProfile(std::string const& id) const;
    void saveProfile(OutputProfile const& profile);       ///< User-defined only
    void deleteProfile(std::string const& id);            ///< User-defined only
    
    // Preset management
    std::vector<ZoneMappingPreset> const& getPresets() const;
    std::vector<ZoneMappingPreset> getPresetsForProfile(std::string const& profileId) const;
    ZoneMappingPreset const* getPreset(std::string const& id) const;
    void savePreset(ZoneMappingPreset const& preset);
    void deletePreset(std::string const& id);
    ZoneMappingPreset duplicatePreset(std::string const& id, 
                                       std::string const& newName);
    
    // Import/Export
    void importPreset(std::filesystem::path const& path);
    void exportPreset(std::string const& id, 
                      std::filesystem::path const& path);
    
    // Convenience
    std::string const& getLastUsedProfileId() const;
    std::string const& getLastUsedPresetId() const;
    void setLastUsedIds(std::string const& profileId, std::string const& presetId);
    
private:
    void loadBuiltInProfiles();
    void loadUserProfiles();
    void loadPresets();
    void savePreset(ZoneMappingPreset const& preset, 
                    std::filesystem::path const& path);
    
    std::filesystem::path m_configDir;
    MaterialZoneConfiguration m_config;
};
```

---

## 4. Technical Architecture

### 4.1 Component Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        Export Dialog UI                          │
│                    (MaterialZoneExportTab)                       │
└─────────────────────────────────┬───────────────────────────────┘
                                  │
                    ┌─────────────┴─────────────┐
                    ▼                           ▼
┌───────────────────────────────┐ ┌───────────────────────────────┐
│  MaterialZoneConfigManager    │ │     ZoneMappingEngine         │
│ • Load/save profiles/presets  │ │ • Stores mapping rules        │
│ • Manage output profiles      │ │ • Handles color matching      │
│ • Handle import/export        │ │ • Provides palette detection  │
└───────────────────────────────┘ └───────────────┬───────────────┘
                                                  │
                                                  ▼
┌─────────────────────────────────────────────────────────────────┐
│                    FaceColorSampler (GPU)                        │
│  • Samples volumetric color at face centroids                    │
│  • Batched GPU evaluation                                        │
│  • Returns color buffer for all faces                            │
└─────────────────────────────────┬───────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────┐
│                    ZoneAssigner                                  │
│  • Maps sampled colors to zone IDs                               │
│  • Applies matching rules                                        │
│  • Generates per-face zone array                                 │
└─────────────────────────────────┬───────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────┐
│                    ZoneExporterFactory                           │
│  • Creates appropriate exporter based on OutputProfile           │
└───────┬─────────────┬─────────────┬─────────────┬───────────────┘
        │             │             │             │
        ▼             ▼             ▼             ▼
┌───────────┐ ┌───────────┐ ┌───────────┐ ┌───────────────┐
│ 3MF       │ │ 3MF       │ │ Separate  │ │ Custom        │
│ Materials │ │ VertexClr │ │ Meshes    │ │ Metadata      │
└───────────┘ └───────────┘ └───────────┘ └───────────────┘
```

### 4.2 Additional Data Structures

```cpp
/// Result of face color sampling
struct FaceColorSample
{
    std::size_t faceIndex;
    Eigen::Vector3f color;         ///< Sampled color (linear RGB)
    Eigen::Vector3f centroid;      ///< Face centroid position
};

/// Per-face zone assignment result
struct FaceZoneAssignment
{
    std::size_t faceIndex;
    std::string zoneId;            ///< Assigned zone ID
    int zoneIndex;                 ///< 0-based zone index
    Eigen::Vector3f originalColor; ///< For DirectColor mode
    float matchConfidence;         ///< How well the color matched (0-1)
    int matchedRuleIndex;          ///< Which rule matched (-1 if default)
};

/// Statistics for UI display
struct ZoneMappingStats
{
    std::size_t totalFaces;
    std::size_t mappedFaces;
    std::map<std::string, std::size_t> facesPerZone;   ///< Count per zone
    std::vector<std::size_t> facesPerRule;             ///< Count per rule
    float averageConfidence;
};
```

### 4.3 GPU Face Color Sampling

All color evaluation happens on the GPU via direct function evaluation. The existing `model()` OpenCL function already returns `float4` where `.xyz` contains the RGB color (linear) and `.w` contains the SDF. We leverage this by:

1. **Detecting if model has color output** (check if End node's Color parameter is connected)
2. **Computing face centroids** (GPU or CPU - simple operation)
3. **Batched GPU evaluation** of the volumetric function at centroid positions
4. **Extracting RGB colors** from the `.xyz` components and **converting linear → sRGB**

**Color output detection:**

Not all volumetric models define colors. If no color is defined, the `model()` function returns a default (typically white). We must detect this **before** sampling to avoid unnecessary GPU work and misleading exports:

```cpp
/// Check if a model has meaningful color output
/// @return true if the End node's Color parameter has a connected source
bool hasVolumetricColor(nodes::Model const& model);
```

If `hasVolumetricColor()` returns false:
- Skip color sampling entirely
- Export mesh without color/material information
- UI shows "No volumetric color defined" instead of zone mapping options

**Linear to sRGB conversion:**

The `model()` function returns colors in **linear RGB** (as used for physically-based rendering). 3MF and most display/export formats expect **sRGB**. We must convert:

```cpp
/// Convert linear RGB [0,1] to sRGB [0,1]
/// Uses standard sRGB transfer function
inline float linearToSrgb(float linear)
{
    if (linear <= 0.0031308f)
    {
        return 12.92f * linear;
    }
    return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

inline Eigen::Vector3f linearToSrgb(Eigen::Vector3f const& linear)
{
    return {linearToSrgb(linear.x()), 
            linearToSrgb(linear.y()), 
            linearToSrgb(linear.z())};
}
```

**Why batching is essential:**
- Large meshes may have millions of faces
- Each evaluation invokes the full implicit function graph
- GPU memory is limited; uploading/downloading in chunks prevents OOM
- Allows progress reporting and cancellation for long operations

```cpp
/// GPU-accelerated face color sampler using batched evaluation
class FaceColorSampler
{
public:
    explicit FaceColorSampler(ComputeContext& context);
    
    /// Check if the model has volumetric color output
    /// @return true if End node's Color parameter has a connected source
    static bool hasVolumetricColor(nodes::Model const& model);
    
    /// Sample colors at face centroids for entire mesh
    /// @param mesh The mesh geometry (vertices, faces)
    /// @param program The compiled OpenCL program with model() function
    /// @param progressCallback Optional callback for progress updates
    /// @return Vector of RGB colors (sRGB), one per face
    /// @throws std::runtime_error if model has no color output
    std::vector<Eigen::Vector3f> sampleFaceColors(
        MeshData const& mesh,
        CLProgram& program,
        std::function<void(float)> progressCallback = nullptr);
    
    /// Configure batch size (faces per GPU dispatch)
    void setBatchSize(std::size_t batchSize) { m_batchSize = batchSize; }
    
    /// Default batch size - tuned for typical GPU memory
    static constexpr std::size_t DefaultBatchSize = 100000;
    
private:
    /// Compute face centroids from mesh vertices
    /// Can be done on CPU (simple) or GPU (for very large meshes)
    std::vector<Eigen::Vector3f> computeCentroids(MeshData const& mesh);
    
    /// Evaluate colors for a batch of positions
    /// Reuses the existing model() evaluation infrastructure
    /// Colors are returned in linear RGB (conversion to sRGB happens after)
    void evaluateBatch(
        std::vector<Eigen::Vector3f> const& positions,
        std::size_t startIdx,
        std::size_t count,
        CLProgram& program,
        std::vector<Eigen::Vector3f>& outColors);
    
    /// Convert linear RGB to sRGB for all colors
    void convertToSrgb(std::vector<Eigen::Vector3f>& colors);
    
    ComputeContext& m_context;
    std::size_t m_batchSize = DefaultBatchSize;
};
```

**Integration with existing infrastructure:**

The `DualContouringSamplingProgram` already provides GPU batch sampling for SDF values. We extend this pattern to also extract colors:

```cpp
/// Extended sampling that extracts both SDF and color from model()
class ColorSamplingProgram
{
public:
    ColorSamplingProgram(ComputeContext& context);
    
    /// Sample colors at arbitrary positions (batched internally)
    /// @param positions World-space positions to sample
    /// @param model The compiled model program
    /// @return RGB colors (sRGB) at each position
    std::vector<Eigen::Vector3f> sampleColors(
        std::vector<Eigen::Vector3f> const& positions,
        CLProgram& model);
    
private:
    /// OpenCL kernel that calls model() and extracts .xyz
    void buildKernel();
    
    /// Convert linear RGB buffer to sRGB (can be done on GPU or CPU)
    void applyGammaCorrection(std::vector<Eigen::Vector3f>& colors);
    
    ComputeContext& m_context;
    CLProgram m_sampleKernel;
    
    // Reusable GPU buffers (resized as needed)
    Buffer m_positionBuffer;
    Buffer m_colorBuffer;
};
```

**OpenCL Kernel - Color Extraction:**

The kernel extracts linear RGB colors. Conversion to sRGB happens on the CPU after readback (simpler and avoids precision issues with GPU pow()):

```opencl
/// Sample colors by calling the existing model() function
/// model() returns float4: .xyz = RGB color (linear), .w = signed distance
__kernel void sampleColors(
    __global float3 const* positions,
    __global float3* colors,
    uint numPositions
    COMMA_PAYLOAD_ARGS)  // Existing macro for model parameters
{
    uint const idx = get_global_id(0);
    if (idx >= numPositions) return;
    
    float3 const pos = positions[idx];
    
    // Call existing model() - returns color in linear RGB
    float4 const result = model(pos, PASS_PAYLOAD_ARGS);
    
    // Extract and clamp color to valid range (still linear RGB)
    colors[idx] = clamp(result.xyz, 0.0f, 1.0f);
}
```

**Batching strategy:**

```cpp
std::vector<Eigen::Vector3f> FaceColorSampler::sampleFaceColors(
    MeshData const& mesh,
    CLProgram& program,
    std::function<void(float)> progressCallback)
{
    // Step 1: Compute centroids (CPU - fast enough for most meshes)
    auto const centroids = computeCentroids(mesh);
    std::size_t const numFaces = centroids.size();
    
    // Step 2: Allocate output
    std::vector<Eigen::Vector3f> colors(numFaces);
    
    // Step 3: Process in batches to avoid GPU overload
    std::size_t processed = 0;
    while (processed < numFaces)
    {
        std::size_t const batchCount = std::min(m_batchSize, numFaces - processed);
        
        evaluateBatch(centroids, processed, batchCount, program, colors);
        
        processed += batchCount;
        
        if (progressCallback)
        {
            progressCallback(static_cast<float>(processed) / numFaces);
        }
    }
    
    // Step 4: Convert linear RGB to sRGB for export
    convertToSrgb(colors);
    
    return colors;
}
```

**Memory considerations:**

| Batch Size | GPU Memory per Batch | Suitable For |
|------------|---------------------|--------------|
| 10,000 | ~240 KB | Low-end GPUs, integrated graphics |
| 100,000 | ~2.4 MB | Mid-range GPUs (default) |
| 500,000 | ~12 MB | High-end GPUs |
| 1,000,000 | ~24 MB | Workstation GPUs |

The batch size can be auto-tuned based on available GPU memory or set manually for specific hardware.

### 4.4 Color Matching Algorithm

```cpp
class ZoneAssigner
{
public:
    ZoneAssigner(ZoneMappingPreset const& preset);
    
    /// Assign zones to all faces based on their colors
    std::vector<FaceZoneAssignment> assignZones(
        std::vector<Eigen::Vector3f> const& faceColors);
    
    /// Get statistics about the assignment
    ZoneMappingStats computeStatistics(
        std::vector<FaceZoneAssignment> const& assignments) const;
    
private:
    /// Compute color distance based on configured color space
    float colorDistance(Eigen::Vector3f const& a, 
                        Eigen::Vector3f const& b) const;
    
    /// Find best matching rule for a color
    std::pair<int, float> findBestMatch(Eigen::Vector3f const& color) const;
    
    /// Resolve zone ID to index
    int resolveZoneIndex(std::string const& zoneId) const;
    
    ZoneMappingPreset m_preset;
};
```

### 4.5 Export Strategy Pattern

```cpp
/// Abstract base for zone exporters
class IZoneExporter
{
public:
    virtual ~IZoneExporter() = default;
    
    /// Export mesh with zone assignments
    virtual void exportMesh(
        std::filesystem::path const& outputPath,
        MeshData const& mesh,
        std::vector<FaceZoneAssignment> const& assignments,
        ZoneMappingPreset const& preset) = 0;
    
    /// Get supported file extensions
    virtual std::vector<std::string> getSupportedExtensions() const = 0;
};

/// 3MF export with material groups (for indexed zones)
class ThreeMfMaterialExporter : public IZoneExporter
{
public:
    void exportMesh(...) override;
    std::vector<std::string> getSupportedExtensions() const override 
    { 
        return {".3mf"}; 
    }
    
private:
    void writeColorGroup(XmlWriter& writer, ZoneMappingPreset const& preset);
    void writeTrianglesWithMaterials(XmlWriter& writer, MeshData const& mesh,
                                      std::vector<FaceZoneAssignment> const& assignments);
};

/// 3MF export with vertex colors (for direct color mode)
class ThreeMfVertexColorExporter : public IZoneExporter
{
public:
    void exportMesh(...) override;
    std::vector<std::string> getSupportedExtensions() const override 
    { 
        return {".3mf"}; 
    }
};

/// Export as separate mesh files per zone
class SeparateMeshExporter : public IZoneExporter
{
public:
    void exportMesh(...) override;
    std::vector<std::string> getSupportedExtensions() const override 
    { 
        return {".stl", ".obj", ".ply"}; 
    }
};

/// Factory for creating appropriate exporter
class ZoneExporterFactory
{
public:
    static std::unique_ptr<IZoneExporter> create(OutputProfile const& profile);
};
```

---

## 5. Auto-Detection Algorithm

### 5.1 Color Palette Detection

```cpp
struct DetectedPalette
{
    std::vector<Eigen::Vector3f> colors;      ///< Cluster centers
    std::vector<std::size_t> clusterSizes;    ///< Faces per cluster
    std::vector<float> clusterVariances;      ///< Color variance per cluster
};

class PaletteDetector
{
public:
    /// Detect distinct colors in sampled face colors
    /// @param faceColors All face colors from GPU sampling
    /// @param maxColors Maximum number of colors to detect
    /// @param minClusterSize Minimum faces to form a valid cluster
    DetectedPalette detectPalette(
        std::vector<Eigen::Vector3f> const& faceColors,
        int maxColors = 8,
        std::size_t minClusterSize = 100);
    
private:
    /// K-means clustering in Lab color space
    std::vector<int> kMeansClustering(
        std::vector<Eigen::Vector3f> const& colors,
        int k,
        int maxIterations = 100);
    
    /// Find optimal k using elbow method or silhouette score
    int findOptimalK(
        std::vector<Eigen::Vector3f> const& colors,
        int maxK);
};
```

### 5.2 Suggested Workflow

1. **Sample subset** (e.g., 10,000 random faces) for quick detection
2. **Cluster in Lab space** for perceptual grouping
3. **Merge similar clusters** within Delta-E threshold
4. **Present to user** sorted by cluster size
5. **User assigns extruders** to detected colors
6. **Save as preset** for future use

---

## 6. UI Implementation Details

### 6.1 New Files

| File | Purpose |
|------|---------|
| `MaterialZoneExportTab.h/cpp` | ImGui tab widget for export dialog |
| `ZoneMappingEditor.h/cpp` | Table editor for mapping rules |
| `OutputProfileSelector.h/cpp` | Profile dropdown with zone config |
| `ZonePresetSelector.h/cpp` | Preset dropdown with save/load |
| `ZoneDefinitionDialog.h/cpp` | Dialog to edit zone definitions |
| `ColorPickerWidget.h/cpp` | Color picker with eyedropper |
| `ZonePreviewRenderer.h/cpp` | 3D preview with zone colors |

### 6.2 ImGui Layout Sketch

```cpp
void MaterialZoneExportTab::render()
{
    ImGui::Checkbox("Enable Material Zone Export", &m_enabled);
    
    if (!m_enabled) return;
    
    // Output profile selector
    renderProfileSelector();
    
    // Preset selector
    renderPresetSelector();
    
    ImGui::Separator();
    
    // Zone definitions (inline editing)
    renderZoneDefinitions();
    
    ImGui::Separator();
    
    // Mapping rules table
    if (ImGui::BeginTable("ZoneMapping", 5, tableFlags))
    {
        ImGui::TableSetupColumn("Source Color", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Tolerance", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Target Zone", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Faces", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();
        
        for (std::size_t i = 0; i < m_currentPreset.rules.size(); ++i)
        {
            renderMappingRow(i);
        }
        
        ImGui::EndTable();
    }
    
    // Action buttons
    if (ImGui::Button("Add Rule")) { addEmptyRule(); }
    ImGui::SameLine();
    if (ImGui::Button("Auto-Detect")) { runAutoDetection(); }
    ImGui::SameLine();
    if (ImGui::Button("Preview 3D")) { togglePreview(); }
    
    // Statistics
    renderStatistics();
}

void MaterialZoneExportTab::renderProfileSelector()
{
    auto const& profiles = m_configManager.getProfiles();
    
    if (ImGui::BeginCombo("Output Profile", m_currentProfile.name.c_str()))
    {
        for (auto const& profile : profiles)
        {
            bool const isSelected = (profile.id == m_currentProfile.id);
            if (ImGui::Selectable(profile.name.c_str(), isSelected))
            {
                loadProfile(profile.id);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", profile.description.c_str());
            }
        }
        ImGui::EndCombo();
    }
}

void MaterialZoneExportTab::renderZoneDefinitions()
{
    ImGui::Text("Material Zones:");
    
    for (auto& zone : m_currentPreset.zones)
    {
        ImGui::PushID(zone.id.c_str());
        
        // Color swatch
        ImGui::ColorEdit3("##color", zone.displayColor.data(), 
                          ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        
        // Zone name (editable)
        char nameBuf[128];
        strncpy(nameBuf, zone.name.c_str(), sizeof(nameBuf));
        if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
        {
            zone.name = nameBuf;
            m_presetModified = true;
        }
        
        ImGui::PopID();
    }
    
    if (m_currentProfile.maxZones == 0 || 
        m_currentPreset.zones.size() < static_cast<size_t>(m_currentProfile.maxZones))
    {
        if (ImGui::Button("+ Add Zone"))
        {
            addNewZone();
        }
    }
}
```

### 6.3 3D Preview Integration

- Add render mode to existing viewport
- Override face colors with zone preview colors
- Use distinct, easily distinguishable colors
- Highlight unmapped faces with hatching or bright warning color
- For DirectColor mode, show actual sampled colors

---

## 7. Output Format Compatibility

### 7.1 3MF with Materials (Indexed Zones)

For FDM multi-extruder and similar indexed workflows:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<model xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
       xmlns:m="http://schemas.microsoft.com/3dmanufacturing/material/2015/02">
  
  <resources>
    <m:colorgroup id="1">
      <m:color color="#FF0000FF"/>  <!-- Zone 0 -->
      <m:color color="#00FF00FF"/>  <!-- Zone 1 -->
      <m:color color="#0000FFFF"/>  <!-- Zone 2 -->
    </m:colorgroup>
    
    <object id="2" type="model">
      <mesh>
        <vertices>...</vertices>
        <triangles>
          <triangle v1="0" v2="1" v3="2" p1="1"/>  <!-- Zone 0 -->
          <triangle v1="2" v2="3" v3="4" p1="2"/>  <!-- Zone 1 -->
        </triangles>
      </mesh>
    </object>
  </resources>
</model>
```

### 7.2 3MF with Vertex Colors (Direct Color)

For full-color 3D printing:

```xml
<m:colorgroup id="1">
  <!-- Unique colors for each vertex -->
  <m:color color="#A52A2AFF"/>
  <m:color color="#8B4513FF"/>
  <!-- ... -->
</m:colorgroup>

<triangles>
  <triangle v1="0" v2="1" v3="2" p1="1" p2="2" p3="3"/>
</triangles>
```

### 7.3 Separate Mesh Files

For workflows requiring physical separation:

```
output/
├── model_zone0_red_pla.stl
├── model_zone1_green_pla.stl
├── model_zone2_blue_pla.stl
└── model_manifest.json
```

**manifest.json:**
```json
{
  "source": "model.gladius",
  "profile": "fdm_multi_extruder",
  "zones": [
    { "file": "model_zone0_red_pla.stl", "name": "Red PLA", "index": 0 },
    { "file": "model_zone1_green_pla.stl", "name": "Green PLA", "index": 1 },
    { "file": "model_zone2_blue_pla.stl", "name": "Blue PLA", "index": 2 }
  ]
}
```

### 7.4 Software Compatibility Matrix

| Software | 3MF Materials | 3MF Vertex Colors | Separate Meshes |
|----------|--------------|-------------------|-----------------|
| **PrusaSlicer** | ✓ Full | ⚠ Display only | ✓ Import as parts |
| **OrcaSlicer** | ✓ Full | ⚠ Display only | ✓ Import as parts |
| **BambuStudio** | ✓ Full | ⚠ Display only | ✓ Import as parts |
| **Cura** | ⚠ Partial | ✗ | ✓ Import as parts |
| **Simplify3D** | ⚠ Per-object | ✗ | ✓ Import as parts |
| **Formlabs PreForm** | ⚠ Limited | ✓ Full | ✓ |
| **Chitubox** | ✗ | ✓ Display | ✓ |
| **Fusion 360** | ✓ | ✓ | ✓ |
| **Blender** | ✓ (addon) | ✓ | ✓ |

---

## 8. Implementation Phases

### Phase 1: Core Infrastructure (Week 1-2)
- [ ] `OutputProfile`, `MaterialZone`, `ZoneMappingPreset` data structures
- [ ] `FaceColorSampler` GPU implementation
- [ ] `ZoneAssigner` color matching logic
- [ ] Unit tests for color matching

### Phase 2: Persistence Layer (Week 2-3)
- [ ] `MaterialZoneConfigManager` implementation
- [ ] Built-in output profiles
- [ ] JSON serialization/deserialization
- [ ] Config directory setup and migration
- [ ] Unit tests for persistence

### Phase 3: Export Backends (Week 3-4)
- [ ] `IZoneExporter` interface and factory
- [ ] `ThreeMfMaterialExporter` for indexed zones
- [ ] `ThreeMfVertexColorExporter` for direct color
- [ ] `SeparateMeshExporter` for split output
- [ ] Integration tests with sample models

### Phase 4: Basic UI (Week 4-5)
- [ ] `MaterialZoneExportTab` in export dialog
- [ ] Output profile selector
- [ ] Zone definition editor
- [ ] Mapping rules table editor
- [ ] Preset selector with save/load

### Phase 5: Auto-Detection (Week 5-6)
- [ ] `PaletteDetector` k-means implementation
- [ ] UI for auto-detect workflow
- [ ] Palette import/export (JSON)

### Phase 6: 3D Preview (Week 6-7)
- [ ] Zone preview render mode
- [ ] Viewport integration
- [ ] Unmapped face highlighting

### Phase 7: Additional Profiles (Week 7-8)
- [ ] Continuous value mapping (laser power)
- [ ] Custom metadata export
- [ ] Profile creation UI

### Phase 8: Polish & Testing (Week 8-9)
- [ ] Cross-software compatibility testing
- [ ] Performance optimization for large meshes
- [ ] User documentation
- [ ] Edge case handling

---

## 9. Performance Considerations

### 9.1 Large Mesh Handling

For meshes with millions of faces:

1. **Batched GPU sampling**: Process faces in batches of 100K
2. **Progressive UI updates**: Show statistics as sampling progresses
3. **Caching**: Cache sampled colors if mesh/color field unchanged
4. **LOD preview**: Use simplified mesh for 3D preview

### 9.2 Memory Budget

```
Typical mesh: 500K faces
- Face colors: 500K × 12 bytes (float3) = 6 MB
- Extruder assignments: 500K × 4 bytes (int) = 2 MB
- GPU buffers: ~10 MB
Total: ~20 MB additional memory
```

### 9.3 Persistence Performance

- Lazy loading of presets (load metadata only, full preset on demand)
- Async save operations to avoid UI blocking
- File watching for external preset modifications (optional)

---

## 10. Future Extensions

### 10.1 Potential Enhancements

1. **Gradient support**: Map color gradients to dithered zone patterns
2. **Texture baking**: Bake volumetric colors to UV-mapped texture
3. **Material properties**: Export with PBR material definitions
4. **Preset sharing**: Cloud sync or export for team workflows
5. **Batch export**: Apply same mapping to multiple models
6. **Filament library integration**: Link zones to filament database with optical properties

### 10.2 Automatic Shell Thickness from Color (Transmission & Reflection Modes)

**Concept**: Use physical color models to compute optimal shell thickness per face, supporting both backlit (lithophane) and frontlit (HueForge-style) scenarios on arbitrary 3D geometry.

#### Two Fundamental Optical Modes

| Mode | Light Path | Physical Model | Use Case |
|------|------------|----------------|----------|
| **Transmission (Lithophane)** | Light passes through | Beer-Lambert absorption | Backlit prints, lamp shades, window art |
| **Reflection (Frontlit)** | Light reflects off layers | Kubelka-Munk / empirical | Wall art, decorative objects, HueForge-style |

#### 10.2.1 Transmission Mode (Lithophane)

For backlit objects, light passes **through** the material. Color is determined by absorption:

$$C_{out} = C_{in} \cdot \exp(-\alpha \cdot t)$$

- Thicker = darker (more absorption)
- Uses Beer-Lambert model from [filament_color_reproduction.md](filament_color_reproduction.md)
- Works well for translucent filaments

#### 10.2.2 Reflection Mode (Frontlit / HueForge-style)

For frontlit objects, light enters the surface, scatters through layers, and exits back toward the viewer. The perceived color depends on:

1. **Layer colors** in the stack
2. **Layer thicknesses** (how much of each color is visible)
3. **Scattering/opacity** of each material
4. **Viewing angle** (less critical for diffuse materials)

**Key insight**: Unlike lithophanes, frontlit color perception is NOT purely transmissive. A thin top layer reveals colors beneath it. This is closer to **glazing** in painting.

##### Simplified Frontlit Model

For stacked opaque/translucent layers viewed from the front:

$$C_{visible} = \sum_{i=1}^{n} C_i \cdot V_i(t_1, \ldots, t_i)$$

where $V_i$ is the visibility of layer $i$ considering all layers above it:

$$V_i = T_1 \cdot T_2 \cdots T_{i-1} \cdot (1 - T_i)$$

with $T_i = \exp(-\alpha_i \cdot t_i)$ being the transmittance of layer $i$.

##### More Accurate: Kubelka-Munk Theory

For materials with both absorption and scattering:

$$\frac{K}{S} = \frac{(1-R_\infty)^2}{2R_\infty}$$

where:
- $K$ = absorption coefficient
- $S$ = scattering coefficient  
- $R_\infty$ = reflectance of infinitely thick layer

This model better captures how pigmented filaments behave in frontlit scenarios.

#### Use Cases Comparison

| Scenario | Mode | Example |
|----------|------|---------|
| Lamp shade | Transmission | Light inside, view from outside |
| Window decoration | Transmission | Sunlight through print |
| Wall art | Reflection | Room lighting on surface |
| Decorative vase | Both | Depends on lighting setup |
| HueForge portrait | Reflection | Frontlit wall display |
| Arbitrary 3D model | Per-face | Mixed based on intended display |

#### Extended Data Structures

```cpp
/// Optical mode for color computation
enum class OpticalMode
{
    Transmission,      ///< Backlit (lithophane) - Beer-Lambert
    Reflection,        ///< Frontlit (HueForge-style) - layered reflection
    Hybrid            ///< Per-face determination based on surface normal/intent
};

/// Extended filament properties for both modes
struct FilamentOpticalProperties
{
    std::string name;
    
    // Transmission properties (Beer-Lambert)
    Eigen::Vector3f transmissionColor;     ///< Color after one transmission distance
    float transmissionDistance;            ///< mm
    Eigen::Vector3f absorptionAlpha;       ///< Precomputed: -ln(T)/d
    
    // Reflection properties (Kubelka-Munk / empirical)
    Eigen::Vector3f reflectanceColor;      ///< Observed color of thick layer (R∞)
    float opacity;                         ///< 0=transparent, 1=fully opaque at ref thickness
    float referenceThickness;              ///< Thickness for opacity measurement (mm)
    Eigen::Vector3f scatteringCoeff;       ///< S in K-M model (optional)
    
    /// Compute derived coefficients
    void computeCoefficients();
};

/// Face assignment with mode-aware thickness
struct FaceColorAssignment
{
    std::size_t faceIndex;
    Eigen::Vector3f targetColor;
    OpticalMode mode;
    
    // Computed results
    std::vector<float> layerThicknesses;   ///< Per-material thickness
    Eigen::Vector3f achievedColor;
    float colorError;
    
    // For hybrid mode
    float transmissionWeight;              ///< 0=pure reflection, 1=pure transmission
};
```

#### Frontlit Thickness Solver

```cpp
/// Solve for layer thicknesses in frontlit (reflection) mode
class FrontlitThicknessSolver
{
public:
    FrontlitThicknessSolver(
        std::vector<FilamentOpticalProperties> const& filaments,
        ThicknessConstraints const& constraints);
    
    /// Compute thicknesses to achieve target color when frontlit
    /// Uses visibility-weighted color mixing model
    std::vector<float> solve(Eigen::Vector3f const& targetColor) const;
    
    /// Predict perceived color given layer thicknesses (frontlit)
    Eigen::Vector3f predictFrontlitColor(
        std::vector<float> const& thicknesses) const;
    
private:
    /// Compute layer visibility factors
    std::vector<float> computeVisibility(
        std::vector<float> const& thicknesses) const;
    
    /// Iterative solver for non-linear visibility model
    std::vector<float> solveIterative(
        Eigen::Vector3f const& targetColor) const;
    
    std::vector<FilamentOpticalProperties> m_filaments;
    ThicknessConstraints m_constraints;
};

/// Predict frontlit color using layered visibility model
Eigen::Vector3f FrontlitThicknessSolver::predictFrontlitColor(
    std::vector<float> const& thicknesses) const
{
    Eigen::Vector3f result = Eigen::Vector3f::Zero();
    float remainingLight = 1.0f;  // Light that hasn't been absorbed/reflected yet
    
    // Process layers from top (viewer side) to bottom (back)
    for (std::size_t i = 0; i < m_filaments.size(); ++i)
    {
        auto const& f = m_filaments[i];
        
        // How much light penetrates this layer?
        // opacity determines how much is reflected vs transmitted
        float const effectiveOpacity = 1.0f - std::exp(-f.opacity * thicknesses[i] / f.referenceThickness);
        
        // This layer contributes its reflectance * opacity * remaining light
        result += f.reflectanceColor * effectiveOpacity * remainingLight;
        
        // Remaining light continues deeper
        remainingLight *= (1.0f - effectiveOpacity);
        
        if (remainingLight < 0.001f) break;  // Fully occluded
    }
    
    // Any remaining light is absorbed by backing (assume black/no reflection)
    return result;
}
```

#### Hybrid Mode for Arbitrary 3D Geometry

For complex 3D objects, different faces may be viewed differently:

```cpp
/// Determine optical mode per face based on geometry and intent
class HybridModeClassifier
{
public:
    /// Classify each face as transmission, reflection, or hybrid
    std::vector<OpticalMode> classifyFaces(
        MeshData const& mesh,
        HybridModeSettings const& settings) const;
    
private:
    /// Heuristics for mode classification
    OpticalMode classifyFace(
        Eigen::Vector3f const& normal,
        Eigen::Vector3f const& centroid,
        HybridModeSettings const& settings) const;
};

struct HybridModeSettings
{
    /// Primary light direction (for mode inference)
    Eigen::Vector3f lightDirection = {0, 0, 1};
    
    /// Strategy for mode selection
    enum class Strategy
    {
        AllTransmission,     ///< Force lithophane mode everywhere
        AllReflection,       ///< Force frontlit mode everywhere
        ByNormal,            ///< Faces facing light = reflection, away = transmission
        ByUserPaint,         ///< User paints transmission/reflection zones
        Mixed                ///< Interpolate based on dot(normal, lightDir)
    };
    Strategy strategy = Strategy::ByNormal;
    
    /// For Mixed mode: how to blend
    float transitionAngle = 45.0f;  ///< Degrees from light direction
};
```

#### Output Profiles for Transmission vs Reflection

```json
{
  "id": "lithophane_transmission",
  "name": "Lithophane (Backlit)",
  "description": "Variable thickness for transmission/backlit viewing",
  "optical_mode": "transmission",
  "semantics": "continuous",
  "format": "offset_mesh",
  "options": {
    "min_thickness_mm": 0.4,
    "max_thickness_mm": 3.0,
    "layer_height_mm": 0.1,
    "invert": false
  }
}
```

```json
{
  "id": "frontlit_hueforge",
  "name": "Frontlit Color (HueForge-style)",
  "description": "Multi-material layers for frontlit viewing on 3D surfaces",
  "optical_mode": "reflection",
  "semantics": "indexed",
  "format": "separate_meshes",
  "options": {
    "min_layer_mm": 0.1,
    "max_total_mm": 5.0,
    "layer_height_mm": 0.2,
    "filament_order": "auto",
    "backing_material": "white"
  }
}
```

```json
{
  "id": "hybrid_3d",
  "name": "Hybrid (3D Object)",
  "description": "Mixed mode for arbitrary 3D geometry",
  "optical_mode": "hybrid",
  "semantics": "continuous",
  "format": "offset_mesh_with_materials",
  "options": {
    "light_direction": [0, 0, 1],
    "mode_strategy": "by_normal",
    "transition_angle_deg": 45
  }
}
```

#### Workflow Comparison

| Step | Transmission (Lithophane) | Reflection (Frontlit) |
|------|--------------------------|----------------------|
| 1. Sample face colors | GPU volumetric eval | GPU volumetric eval |
| 2. Convert to target | Grayscale or RGB | RGB |
| 3. Solve thickness | Beer-Lambert inverse | Visibility model inverse |
| 4. Generate geometry | Offset inner surface | Multi-layer shells |
| 5. Export | Single variable-thickness mesh | Separate meshes per layer |
| 6. Print orientation | Light behind | Light in front |

#### Implementation Priority

1. **Phase A**: Transmission mode (simpler, reuses existing math)
2. **Phase B**: Reflection mode with empirical opacity model
3. **Phase C**: Hybrid mode with per-face classification
4. **Phase D**: Full Kubelka-Munk for advanced accuracy

### 10.3 Connection to Filament Color Reproduction Plan

The shell thickness computation directly uses the algorithms from [filament_color_reproduction.md](filament_color_reproduction.md):

| Component | From Filament Plan | Used For |
|-----------|-------------------|----------|
| `Filament` struct | Section 1 | Optical properties per material |
| `computeAlpha()` | Section 1 | Absorption coefficients |
| `predictColor()` | Section 2.1 | Forward model validation |
| `solveForThicknesses()` | Section 2.2 | Inverse problem for thickness |
| `boundedNnls()` | Section 3.2 | Constrained optimization |
| `ThicknessConstraints` | Section 1 | Min/max/quantization |

This creates a unified color workflow:
1. **Gladius volumetric colors** → sampled per face
2. **Filament optical model** → thickness computation  
3. **Zone export** → geometry with material assignment
4. **Slicer** → final print with correct colors

---

## 11. Open Questions

1. **Sampling density**: Should we sample at face centroids only, or multiple points per face for large triangles?

2. **Edge handling**: How to handle faces that span color boundaries? Options:
   - Use centroid color (simple)
   - Majority vote from multiple samples
   - Split faces at color boundaries (complex)

3. **Transparency**: Should we support translucent colors mapping to specific extruders?

4. **Slicer presets**: Should we bundle slicer-specific export presets?

5. **Preset versioning**: How to handle breaking changes to preset format?

---

## 12. References

- 3MF Core Specification: https://3mf.io/specification/
- 3MF Materials Extension: https://github.com/3MFConsortium/spec_materials
- PrusaSlicer MMU Documentation: https://help.prusa3d.com/article/multi-material-upgrade-2s_5476
- lib3mf (C++ library): https://github.com/3MFConsortium/lib3mf (available via VCPKG)

---

## 12. File Structure

Proposed new files in Gladius:

```
gladius/src/
├── export/
│   ├── MaterialZone.h                 # Core data structures
│   ├── MaterialZone.cpp
│   ├── OutputProfile.h                # Output profile definitions
│   ├── OutputProfile.cpp
│   ├── MaterialZoneConfigManager.h    # Persistence manager
│   ├── MaterialZoneConfigManager.cpp
│   ├── FaceColorSampler.h             # GPU color sampling
│   ├── FaceColorSampler.cpp
│   ├── ZoneAssigner.h                 # Color-to-zone logic
│   ├── ZoneAssigner.cpp
│   ├── PaletteDetector.h              # Auto-detection
│   ├── PaletteDetector.cpp
│   ├── IZoneExporter.h                # Exporter interface
│   ├── ZoneExporterFactory.h          # Factory for exporters
│   ├── ZoneExporterFactory.cpp
│   ├── ThreeMfMaterialExporter.h      # 3MF with materials
│   ├── ThreeMfMaterialExporter.cpp
│   ├── ThreeMfVertexColorExporter.h   # 3MF with vertex colors
│   ├── ThreeMfVertexColorExporter.cpp
│   ├── SeparateMeshExporter.h         # Split mesh output
│   └── SeparateMeshExporter.cpp
├── ui/
│   ├── MaterialZoneExportTab.h        # Export dialog tab
│   ├── MaterialZoneExportTab.cpp
│   ├── ZoneMappingEditor.h            # Mapping rules editor
│   ├── ZoneMappingEditor.cpp
│   ├── OutputProfileSelector.h        # Profile dropdown
│   ├── OutputProfileSelector.cpp
│   ├── ZonePresetSelector.h           # Preset dropdown
│   └── ZonePresetSelector.cpp
└── compute/
    └── kernels/
        └── face_color_sampling.cl     # OpenCL kernels
```

## 13. Built-in Output Profiles

The following profiles are provided out-of-the-box:

### 13.1 FDM Multi-Extruder
- **Use case**: Multi-material FDM (MMU, AMS, tool changers)
- **Semantics**: Indexed (extruder slot)
- **Format**: 3MF with materials
- **Max zones**: 16
- **Compatible with**: PrusaSlicer, OrcaSlicer, BambuStudio, Cura

### 13.2 Resin Multi-Material
- **Use case**: Multi-material SLA/LCD printers
- **Semantics**: Indexed (material vat)
- **Format**: 3MF with materials or separate meshes
- **Max zones**: 8
- **Compatible with**: Formlabs PreForm, Chitubox (via separate meshes)

### 13.3 Full Color (Indexed Palette)
- **Use case**: Full-color printers with limited palette
- **Semantics**: Indexed (palette entry)
- **Format**: 3MF with colors
- **Max zones**: 256
- **Compatible with**: Stratasys PolyJet, 3D Systems ColorJet

### 13.4 Full Color (Direct RGB)
- **Use case**: Full-color printers with continuous color
- **Semantics**: Direct color (no mapping)
- **Format**: 3MF with vertex colors
- **Max zones**: N/A (continuous)
- **Compatible with**: HP MJF, Mimaki, Stratasys J-series

### 13.5 Laser Power Levels
- **Use case**: Laser engraving/marking
- **Semantics**: Continuous (0-100% power)
- **Format**: Separate meshes or custom metadata
- **Max zones**: 256 levels
- **Compatible with**: Custom post-processing

### 13.6 Generic Named Zones
- **Use case**: Downstream CAD/CAM processing
- **Semantics**: Named regions
- **Format**: 3MF with custom metadata
- **Max zones**: Unlimited
- **Compatible with**: Custom workflows

---

## 14. Recommended Libraries

| Library | Purpose | Availability |
|---------|---------|--------------|
| **Eigen** | Linear algebra, color math | VCPKG ✓ |
| **nlohmann/json** | JSON persistence | VCPKG ✓ |
| **lib3mf** | 3MF file writing | VCPKG ✓ |
| **fmt** | String formatting | VCPKG ✓ |

All recommended libraries are available through VCPKG and are widely used in the C++ community.

---

This plan provides a comprehensive, technology-agnostic roadmap for implementing material zone export with persistent configurations, supporting various manufacturing workflows from FDM multi-material to full-color 3D printing and beyond.

### 10.4 Integration with GPU Shell Generation

The "Transmission" and "Reflection" optical modes described in section 10.2 require generating geometry with variable thickness or multiple nested shells. This is computationally intensive and best handled on the GPU.

A detailed plan for this specific geometric operation is available in **[gpu_shell_generation.md](gpu_shell_generation.md)**.

**Key Integration Points:**

1.  **Shared Data**: The `FaceColorSampler` (Section 4.3) provides the input color data used to drive the shell generation parameters.
2.  **Geometry Generation**: The "Multi-Pass Nested Surfaces" approach (Approach A in `gpu_shell_generation.md`) is the primary mechanism for creating the physical geometry required by the optical modes.
3.  **Workflow**:
    *   **Step 1**: Sample colors (this plan).
    *   **Step 2**: Compute required thicknesses/offsets based on optical models (this plan).
    *   **Step 3**: Generate shell geometry using GPU Dual Contouring with variable iso-values (GPU Shell Generation plan).
    *   **Step 4**: Export resulting meshes (this plan).

This separation of concerns allows the color/material logic to remain distinct from the low-level geometric operations.
