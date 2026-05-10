# Gladius & 3MF Volumetric Extension Technical Overview

This document provides a comprehensive technical overview of Gladius and how it implements the 3MF Volumetric & Implicit Extension. It is intended to help coding agents and developers quickly understand the domain and architecture.

## Table of Contents

1. [What is Gladius](#what-is-gladius)
2. [3MF Format Overview](#3mf-format-overview)
3. [3MF Volumetric Extension](#3mf-volumetric-extension)
4. [3MF Implicit Extension](#3mf-implicit-extension)
5. [How Gladius Implements Volumetric/Implicit](#how-gladius-implements-volumetricimplicit)
6. [Key Concepts](#key-concepts)
7. [Architecture Deep Dive](#architecture-deep-dive)
8. [Common Development Scenarios](#common-development-scenarios)

---

## What is Gladius

**Gladius** is a development tool and library for processing **implicit geometries** using the 3MF Volumetric Extension. It serves as:

- A **GUI application** with an interactive node/graph editor for designing implicit geometry
- A **library-style core** providing compute + IO pipelines for integration into other applications
- A **playground/reference implementation** for the 3MF Volumetric Extension specification

### Core Capabilities

| Feature | Description |
|---------|-------------|
| **Import/Export** | 3MF with volumetric extension, STL, OpenVDB, SVG/CLI contours |
| **Function Graphs** | Visual node-based programming for implicit surfaces |
| **Real-time Preview** | GPU-accelerated ray marching visualization |
| **Mesh Generation** | Dual Contouring algorithms (standard, hierarchical, manifold) |
| **API** | C++, C#, Python bindings via Automatic Component Toolkit |

---

## 3MF Format Overview

**3MF** (3D Manufacturing Format) is an XML-based file format for 3D printing developed by the 3MF Consortium. The core specification defines:

- **Triangle meshes** for representing object geometry
- **Materials and colors** for appearance
- **Build items** for print job configuration

The format is extensible through "a la carte" extensions that add capabilities without breaking backward compatibility.

---

## 3MF Volumetric Extension

The **Volumetric Extension** (namespace: `http://schemas.3mf.io/3dmanufacturing/volumetric/2022/01`) extends 3MF with:

### Core Concepts

#### 1. Functions (`<function>`)

Functions are the building blocks for defining volumetric properties. A function can be evaluated at any 3D position to produce scalar or vector values.

```xml
<v:function id="3" displayname="myFunction">
  <!-- Function implementation goes here -->
</v:function>
```

**Function Types:**

| Type | Description | Required Support |
|------|-------------|------------------|
| `<functionfromimage3d>` | Samples from voxel grid data | **MUST** |
| `<i:implicitfunction>` | Node-based mathematical functions | OPTIONAL |
| `PrivateExtensionFunction` | Vendor-specific functions | OPTIONAL |

#### 2. Image3D (`<image3d>`)

Represents voxel data as a stack of 2D images (PNG):

```xml
<v:image3d id="2">
  <v:imagestack rowcount="256" columncount="256" sheetcount="64">
    <v:imagesheet path="/volume/layer_01.png"/>
    <v:imagesheet path="/volume/layer_02.png"/>
    <!-- ... -->
  </v:imagestack>
</v:image3d>
```

#### 3. FunctionFromImage3D

Defines a function that samples from Image3D data:

```xml
<v:functionfromimage3d id="3" 
  image3did="2"
  filter="linear"
  tilestyleu="clamp" 
  tilestylev="clamp" 
  tilestylew="clamp"
  valueoffset="0.0"
  valuescale="1.0"/>
```

**Outputs:** `color` (vector), `red`, `green`, `blue`, `alpha` (scalars)

**Filter modes:** `linear` (trilinear interpolation), `nearest`

**Tile styles:** `wrap`, `mirror`, `clamp`

#### 4. LevelSet (`<levelset>`)

Defines object geometry using an implicit function (iso-surface at value 0):

```xml
<v:levelset functionid="5" 
  channel="shape"
  meshid="1"
  transform="..."
  minfeaturesize="0.1"/>
```

| Attribute | Description |
|-----------|-------------|
| `functionid` | Reference to function resource |
| `channel` | Name of scalar output to use |
| `meshid` | Bounding mesh for evaluation domain |
| `meshbboxonly` | If true, only use bounding box of mesh |
| `transform` | Object-to-function coordinate transform |
| `minfeaturesize` | Hint for minimum resolvable features |

#### 5. VolumeData (`<volumedata>`)

Attaches volumetric properties to a shape:

```xml
<v:volumedata id="10">
  <v:color functionid="3" channel="color" transform="..."/>
  <v:composite basematerialid="4">
    <v:materialmapping functionid="5" channel="ratio"/>
  </v:composite>
  <v:property name="custom:opacity" functionid="6" channel="value"/>
</v:volumedata>
```

**Child elements:**
- `<color>`: Spatially varying RGB color
- `<composite>`: Multi-material mixing ratios
- `<property>`: Arbitrary named properties

---

## 3MF Implicit Extension

The **Implicit Extension** (namespace: `http://schemas.3mf.io/3dmanufacturing/implicit/2023/12`) adds node-based mathematical function definitions.

### ImplicitFunction Structure

```xml
<i:implicitfunction id="5" displayname="sphere">
  <i:in>
    <i:vector identifier="pos" displayname="pos"/>
    <i:scalar identifier="radius" displayname="radius"/>
  </i:in>
  
  <!-- Computation nodes -->
  <i:length identifier="len1">
    <i:in><i:vectorref identifier="A" ref="inputs.pos"/></i:in>
    <i:out><i:scalar identifier="result"/></i:out>
  </i:length>
  
  <i:subtraction identifier="sub1">
    <i:in>
      <i:scalarref identifier="A" ref="len1.result"/>
      <i:scalarref identifier="B" ref="inputs.radius"/>
    </i:in>
    <i:out><i:scalar identifier="result"/></i:out>
  </i:subtraction>
  
  <i:out>
    <i:scalarref identifier="shape" ref="sub1.result"/>
  </i:out>
</i:implicitfunction>
```

### Data Types

| Type | Description |
|------|-------------|
| `scalar` | Single floating-point value |
| `vector` | 3D vector (float3) |
| `matrix` | 4x4 transformation matrix |
| `resourceid` | Reference to another resource |

### Native Node Categories

**Arithmetic:** `addition`, `subtraction`, `multiplication`, `division`

**Trigonometric:** `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`

**Hyperbolic:** `sinh`, `cosh`, `tanh`

**Mathematical:** `sqrt`, `pow`, `exp`, `log`, `log2`, `log10`, `abs`, `min`, `max`, `clamp`, `fmod`, `mod`

**Vector Operations:** `length`, `dot`, `cross`, `composevector`, `decomposevector`

**Matrix Operations:** `transpose`, `inverse`, `matvecmultiplication`, `composematrix`, `matrixfromcolumns`, `matrixfromrows`

**Comparison/Selection:** `select`

**Rounding:** `round`, `ceil`, `floor`, `sign`, `fract`

**Geometry:** `mesh` (signed distance), `unsignedmesh`, `beamlattice`

**Function:** `functioncall`, `functiongradient`, `normalizedistance`

### Connection References

Nodes are connected via references using `nodename.outputname` syntax:
- `inputs.pos` - Function input named "pos"
- `len1.result` - Output "result" from node "len1"

---

## How Gladius Implements Volumetric/Implicit

### Node System (`gladius/src/nodes/`)

Gladius represents implicit functions as a **directed acyclic graph (DAG)** of nodes:

| Gladius Class | 3MF Equivalent |
|---------------|----------------|
| `Begin` | Function inputs (`<i:in>`) |
| `End` | Function outputs (`<i:out>`) |
| `Addition`, `Subtraction`, etc. | Native nodes |
| `SignedDistanceToMesh` | `<i:mesh>` node |
| `FunctionCall` | `<i:functioncall>` |
| `FunctionGradient` | `<i:functiongradient>` |
| `ImageSampler` | `<v:functionfromimage3d>` |

### Key Classes

```
gladius/src/nodes/
├── Model.h           # Collection of nodes forming a function graph
├── NodeBase.h        # Base class for all nodes
├── DerivedNodes.h    # Concrete node implementations (2000+ lines)
├── Assembly.h        # Collection of Models (functions) + build configuration
├── Builder.h         # Utility for programmatic graph construction
├── ToOCLVisitor.h    # Generates OpenCL code from graph
└── GraphFlattener.h  # Inlines function calls for compilation
```

### Graph to OpenCL Pipeline

```
Assembly (editable graph)
    │
    ▼ LowerFunctionGradient
    │ LowerNormalizeDistanceField
    │ OptimizeOutputs
    ▼ GraphFlattener::flatten()
    │
Flat Assembly
    │
    ▼ ToOclVisitor / ToCommandStreamVisitor
    │
OpenCL Source Code
    │
    ▼ clBuildProgram()
    │
GPU Kernel
```

### IO System (`gladius/src/io/`)

**Import:** `Importer3mf` reads 3MF files using lib3mf and constructs:
- `Assembly` containing all `Model` (function) definitions
- `ImageStackResource` for voxel data
- Build items and mesh resources

**Export:** `Writer3mf` / `MeshWriter3mf` serialize:
- Function graphs back to XML
- Generated meshes with optional per-vertex colors
- Volumetric properties

### Resource Management

Resources are managed via typed keys:

```cpp
ResourceKey{resourceId, ResourceType::ImageStack}
ResourceKey{resourceId, ResourceType::Vdb}
ResourceKey{resourceId, ResourceType::Mesh}
```

### Implicit Surface Representation (SDF)

Gladius uses **Signed Distance Fields (SDF)** as the primary implicit representation:

- **Negative values:** Inside the object
- **Zero:** On the surface (iso-surface)
- **Positive values:** Outside the object

Common SDF operations:
- **Union:** `min(sdf_a, sdf_b)`
- **Intersection:** `max(sdf_a, sdf_b)`
- **Difference:** `max(sdf_a, -sdf_b)`
- **Smooth blending:** Various smoothmin/smoothmax functions

---

## Key Concepts

### Coordinate Systems

1. **Object coordinates:** Local space of each object
2. **Model coordinates:** Global 3MF model space
3. **Function coordinates:** Normalized [0,1]³ for image sampling

The `transform` attribute on volumetric elements maps between coordinate systems.

### Boolean Operations via SDF

Since implicit functions return distance values:
```cpp
// Union (logical OR)
float combined = min(shape_a, shape_b);

// Intersection (logical AND)
float combined = max(shape_a, shape_b);

// Subtraction (A minus B)
float combined = max(shape_a, -shape_b);
```

### Graph Validation Rules

Per the 3MF Implicit specification:
- Graphs MUST be acyclic and directed
- Functions MUST NOT reference themselves (no recursion)
- Inputs MUST only reference outputs of compatible types
- Resources MUST be defined before use (define-before-use)

---

## Architecture Deep Dive

### Core Components

```
┌─────────────────────────────────────────────────────────────┐
│                      Application Layer                       │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐ │
│  │ MainWindow  │  │  Document   │  │  MCP Server         │ │
│  │ (ImGui UI)  │  │ (Session)   │  │  (Optional API)     │ │
│  └─────────────┘  └─────────────┘  └─────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                              │
┌─────────────────────────────────────────────────────────────┐
│                       Nodes Layer                            │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ Assembly        ┌─────────┐ ┌─────────┐             │   │
│  │                 │ Model 1 │ │ Model 2 │ ...         │   │
│  │                 │(Function)│ │(Function)│            │   │
│  │                 └─────────┘ └─────────┘             │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
                              │
┌─────────────────────────────────────────────────────────────┐
│                      Compute Layer                           │
│  ┌──────────────┐  ┌───────────────┐  ┌─────────────────┐  │
│  │ ComputeCore  │  │ ProgramManager│  │ RenderProgram   │  │
│  │              │  │               │  │ SlicerProgram   │  │
│  │              │  │               │  │ MDC/HDC         │  │
│  └──────────────┘  └───────────────┘  └─────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                              │
┌─────────────────────────────────────────────────────────────┐
│                       IO Layer                               │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │
│  │ Importer3mf  │  │ Writer3mf    │  │ MeshExporter3mf  │  │
│  │              │  │              │  │ STL/VDB/CLI      │  │
│  └──────────────┘  └──────────────┘  └──────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### Data Flow: Loading a 3MF File

```
3MF File (zip with XML + images)
    │
    ▼ lib3mf parsing
    │
Lib3MF Model Object
    │
    ▼ Importer3mf::import()
    │
    ├── Read implicit functions → Create nodes::Model for each
    ├── Read image3d resources → Create ImageStackResource
    ├── Read mesh objects → Create mesh buffers
    └── Read build items → Configure Assembly
    │
    ▼
Assembly + ResourceManager
    │
    ▼ Document::refreshModelAsync()
    │
    ├── Lower + flatten graphs
    ├── Generate OpenCL
    └── Compile programs
    │
    ▼
GPU-ready for rendering/meshing
```

---

## Common Development Scenarios

### Adding a New Node Type

1. Add class in `DerivedNodes.h` deriving from `ClonableNode<MyNode>`
2. Define `TypeRules` for input/output types
3. Add to `NodeTypes` tuple in `nodesfwd.h`
4. Implement OpenCL generation in `ToOCLVisitor.cpp`
5. Add 3MF serialization in `Importer3mf.cpp` and `Writer3mf.cpp`

### Modifying Mesh Generation

Entry points:
- `Document::exportAsStl()` - triggers export pipeline
- `ManifoldDualContouringStlExporter` - MDC algorithm
- `HierarchicalDualContouringStlExporter` - HDC algorithm

### Working with 3MF IO

- Import: `gladius/src/io/3mf/Importer3mf.cpp`
- Export functions: `gladius/src/io/3mf/Writer3mf.cpp`
- Export meshes: `gladius/src/io/3mf/MeshWriter3mf.cpp`

### Debugging Implicit Functions

1. Enable debug visualization in the viewport
2. Check kernel compilation logs
3. Use `GLADIUS_DEBUG_MDC_CONFIG=1` for MDC debugging
4. Validate graph validity via `Model::isValid()`

---

## References

- [3MF Volumetric Extension Specification](https://github.com/3MFConsortium/spec_volumetric)
- [3MF Core Specification](https://github.com/3MFConsortium/spec_core)
- [Gladius Developer Onboarding](docs/developer_onboarding.md)
- [Graph to OpenCL Architecture](docs/architecture/graph_to_opencl.md)

---

*Last updated: 2026-01-26*
