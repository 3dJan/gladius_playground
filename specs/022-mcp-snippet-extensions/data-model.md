# Data Model: MCP Snippet Tool Extensions

**Feature**: 022-mcp-snippet-extensions  
**Date**: 2026-02-26

## Entities

### FunctionArgument (existing)

Represents a named, typed input parameter of a function. Maps to a Begin node output port.

| Field | Type | Description |
|-------|------|-------------|
| name  | string | Identifier for the argument (e.g., "pos", "radius") |
| type  | ArgumentType (Scalar \| Vector) | Data type: float or vec3 |

**Serialization to JSON** (MCP wire format):
```json
{"name": "pos", "type": "vec3"}
```

**Serialization to code** (program snippet format):
```glsl
vec3 pos
float radius
```

### FunctionOutput (existing)

| Field | Type | Description |
|-------|------|-------------|
| name  | string | Output port name (default: "result" → "shape") |
| type  | ArgumentType (Scalar \| Vector) | Data type: float or vec3 |

### FunctionSignature (new, conceptual — not a C++ class)

The combination of arguments and output type that defines a function's interface. Not stored as a separate entity; derived from Begin/End nodes.

| Component | Source |
|-----------|--------|
| arguments | Begin node output ports |
| output_type | End node parameter type |
| unique_name | `generateUniqueFunctionName(displayName, resourceId)` |

### BuildItemRootInfo (new, conceptual — MCP response-only)

Metadata about which functions serve as root shapes in the scene. Built at query time from the Document's build items and levelset/object chain.

| Field | Type | Description |
|-------|------|-------------|
| function_id | uint32 | Resource ID of the function used as a root shape |
| build_item_name | string | Name of the build item referencing this function |

### ReservedKeywords (new, compile-time constant)

Set of names that cannot be used as function argument names.

```
float, vec3, vec2, vec4, mat3, mat4, int, void, bool,
return, if, else, for, while, do, break, continue,
sin, cos, tan, asin, acos, atan, atan2, exp, log, sqrt, abs,
sign, floor, ceil, round, fract, length, dot, cross, normalize,
pow, min, max, clamp, mod, fmod, select, mix, step, smoothstep
```

## Relationships

```
Document 1──* BuildItem
BuildItem 1──1 Object (via ResourceId)
Object 1──1 LevelSet (optional)
LevelSet 1──1 Function (via ResourceId)

Assembly 1──* Function (keyed by ResourceId)
Function 1──1 BeginNode (arguments as output ports)
Function 1──1 EndNode (output type from parameter sources)
Function *──* Function (via FunctionCall nodes → call graph)
```

## State Transitions

### Function Argument Update (via set_function_snippet)

```
1. Parse snippet body + arguments list
2. Create temp Model with Begin/End nodes
3. Add argument ports to Begin node from arguments list
4. Convert snippet body to graph nodes
5. Validate: all referenced variables must resolve to arguments or local assigns
6. On success: replace real model with temp model (atomic swap)
7. On failure: return error, real model unchanged
```

### Build-Item Root Resolution (via get_program_snippet)

```
1. Get Document's BuildItems list
2. For each BuildItem: follow ResourceId → Object → LevelSet → Function chain
3. Collect set of root function ResourceIds
4. Pass to response builder: annotate each function as "root" or "helper"
```
