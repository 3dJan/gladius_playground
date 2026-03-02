# Data Model: Graph ↔ Code View

**Phase 1 output for [plan.md](plan.md)**

## Entities

### 1. CodeSnippet (value object — not persisted)

A GLSL-like text representation of a single function graph.

| Field | Type | Description | Constraints |
|-------|------|-------------|-------------|
| text | `std::string` | The GLSL-like source code | Valid snippet syntax |
| functionId | `ResourceId` | The function this snippet represents | Must reference existing function |
| isDirty | `bool` | Whether text has been edited since last sync | Default: false |
| lastSyncedText | `std::string` | Text from the last successful sync | Empty until first sync |

**Validation rules**:
- `text` must parse without errors to be syncable
- `functionId` must correspond to a function in the current document

**State transitions**:
- `Clean` → `Dirty`: user edits text in Code tab
- `Dirty` → `Clean`: successful sync (text regenerated from graph)
- `Dirty` → `Clean`: user discards changes (text reverted to lastSyncedText)

### 2. UniqueFunctionName (value object — derived)

A valid GLSL identifier uniquely naming a function for code representation.

| Field | Type | Description | Constraints |
|-------|------|-------------|-------------|
| name | `std::string` | The generated identifier | Valid GLSL identifier |
| displayName | `std::string` | Original display name | From function metadata |
| resourceId | `ResourceId` | The function's resource ID | Unique in document |

**Generation rule**: `sanitize(displayName) + "_" + std::to_string(resourceId)`

**Sanitization**:
1. Replace non-alphanumeric characters with `_`
2. Collapse consecutive `_` to single `_`
3. If result starts with a digit, prepend `f_`
4. Trim trailing `_`

### 3. WholeProgramListing (value object — not persisted)

A concatenation of all function snippets in dependency order.

| Field | Type | Description | Constraints |
|-------|------|-------------|-------------|
| text | `std::string` | All functions as code | No circular deps |
| functionOrder | `std::vector<ResourceId>` | Topological order of functions | DAG (no cycles) |

**Validation rules**:
- Function call graph must be a DAG — circular dependencies rejected with error

### 4. CodeBuffer (UI state — per function, in ModelEditor)

Holds the code editor state for one function while the Code tab is open.

| Field | Type | Description | Constraints |
|-------|------|-------------|-------------|
| buffer | `std::string` | Current editor text content | Up to ~64KB |
| functionId | `ResourceId` | Associated function | Must match current function |
| generated | `bool` | Whether code has been generated for this function | Default: false |
| syncedText | `std::string` | Text at last successful sync | For dirty detection |

**Lifecycle**:
- Created lazily when Code tab is first opened for a function
- Populated from `convertGraphToSnippet` on first open
- Preserved across tab switches (Code ↔ Graph)
- Cleared when function is deleted or document changes

## Relationships

```
Document 1──* Assembly
Assembly 1──* Function (via std::map<ResourceId, SharedModel>)
Function 1──1 Model (node graph)
Function 1──0..1 CodeBuffer (UI state, created lazily)
Function *──* Function (via FunctionCall nodes — DAG)
Model 1──* Node
Model 1──* Link
FunctionCall ──1 Resource ──> Function (via ResourceId)
```

## Key Type Mappings (Graph ↔ Code)

### Scalar types
| Graph Type | Code Type | Example |
|-----------|-----------|---------|
| Float | `float` | `float v0 = sin(pos.x);` |

### Vector types
| Graph Type | Code Type | Example |
|-----------|-----------|---------|
| Float3 | `vec3` | `vec3 v0 = vec3(1.0, 2.0, 3.0);` |

### Matrix types
| Graph Type | Code Type | Example |
|-----------|-----------|---------|
| Matrix4 | `mat4` | `mat4 v0 = mat4(1,0,0,0, ...);` |

### Node → Code Syntax (complete mapping)

| Node Category | Graph Node | Code Syntax |
|--------------|-----------|-------------|
| Binary ops | Addition, Subtraction, Multiplication, Division | `a + b`, `a - b`, `a * b`, `a / b` |
| Unary math | Sin, Cos, Tan, etc. (21 types) | `sin(x)`, `cos(x)`, etc. |
| Binary math | Pow, ArcTan2, Fmod, Mod, Min, Max | `pow(a,b)`, `atan2(a,b)`, etc. |
| Ternary | Clamp | `clamp(x, lo, hi)` |
| Quaternary | Select | `select(a, b, c, d)` |
| Constants | ConstantScalar | literal: `3.14` |
| Vector access | DecomposeVector | `expr.x`, `.y`, `.z` |
| Vector construct | ComposeVector | `vec3(x, y, z)` |
| Vector constant | ConstantVector | `vec3(x, y, z)` |
| Vector broadcast | VectorFromScalar | `vec3(s)` |
| Dot/Cross | DotProduct, CrossProduct | `dot(a, b)`, `cross(a, b)` |
| Matrix constant | ConstantMatrix | `mat4(m00..m33)` |
| Matrix ops | Transpose, Inverse | `transpose(m)`, `inverse(m)` |
| Matrix×vector | MatrixVectorMultiplication | `matmul(m, v)` |
| Function call | FunctionCall + Resource | `name_id(args...)` |
| Gradient | FunctionGradient + Resource | `gradient_name_id(pos)` |
| Mesh SDF | SignedDistanceToMesh | `sdfMesh_id(pos)` |
| Mesh UDF | UnsignedDistanceToMesh | `udfMesh_id(pos)` |
| Beam SDF | SignedDistanceToBeamLattice | `sdfBeamLattice_id(pos)` |
| Image sampler | ImageSampler | `sampleImage3D_id(pos)` |
| Transform | Transformation | `transform(pos, matrix)` |
| Normalize | NormalizeDistanceField | `normalizeSDF(expr)` |
| Begin | Begin | Parameter names as identifiers |
| End | End | `return expr;` |

### Resource-backed nodes

These nodes reference external resources by ID. In code, the ID is embedded in the function name. In the graph, a `Resource` node holds the `ResourceId` and links to the node's ID input.

| Code Pattern | Graph Pattern |
|-------------|--------------|
| `sdfMesh_42(pos)` | Resource(id=42) → SignedDistanceToMesh.MeshId |
| `functionName_42(a, b)` | Resource(id=42) → FunctionCall.FunctionId |
| `sampleImage3D_42(pos)` | Resource(id=42) → ImageSampler.Image3DId |
