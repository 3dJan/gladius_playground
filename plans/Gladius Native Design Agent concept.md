# Gladius Native Design Agent

## Concept for hierarchical, variant-driven generative design

**Status:** Initial architecture concept  
**Target application:** Gladius, the 3MF Consortium reference and design application  
**Primary implementation environment:** C++, Dear ImGui, Gladius implicit geometry, native and WebAssembly builds

---

## 1. Executive summary

Gladius can evolve from an MCP-enabled modeling application into an **agent-native 3D design environment**. The proposed system embeds an AI design agent directly in the native Gladius user interface. The agent accesses Gladius through a dedicated internal tool API instead of routing calls through MCP.

The defining idea is not one-shot text-to-CAD generation. The agent follows a systematic engineering-design process inspired by:

- hierarchical divide and conquer;
- functional decomposition;
- the morphological box (*Morphologischer Kasten*);
- engineering design catalogues (*Konstruktionskataloge*);
- explicit component interfaces;
- multi-fidelity evaluation;
- iterative rendering, validation, and refinement.

The agent first decomposes a design problem into functions and components. It then proposes multiple solution principles for each function, forms compatible system concepts, eliminates weak combinations cheaply, and realizes only the most promising variants as geometry. Deterministic evaluation from Gladius is combined with visual assessment and user preferences.

The result is a design-space exploration system rather than a chat-driven macro recorder.

---

## 2. Motivation

A conventional LLM integration might expose geometry primitives and ask the model to construct an entire object. This has several weaknesses:

- the model is unlikely to produce a good design in one pass;
- long sequences of low-level geometry calls are fragile and expensive;
- the model has insufficient feedback about the actual result;
- one mutable design path suppresses alternative concepts too early;
- subjective LLM scoring can be mistaken for engineering validation;
- small user changes can unintentionally alter unrelated parts of the model;
- the final model may lack useful parameters, structure, and design rationale.

Gladius has properties that support a stronger approach:

- hierarchical implicit geometry;
- programmatic and parametric construction;
- fast rendering through WebGPU/OpenGL;
- access to model semantics and 3MF data;
- deterministic geometry inspection and validation;
- native C++ and WebAssembly targets;
- an existing MCP tool surface that can inform a common agent-facing API.

The central principle is:

> **Generate diversity symbolically, evaluate cheaply, instantiate selectively, and add detail only after selecting a coherent system concept.**

---

## 3. Product vision

The user works with an AI assistant inside Gladius. Chat is one interaction channel, but the assistant is also connected to:

- a design tree;
- a graph of alternatives;
- the active model configuration;
- generated reference images;
- the current 3D viewport;
- engineering constraints and validation results;
- a catalogue of reusable solution principles.

A representative workflow is:

1. The user describes the desired product and manufacturing constraints.
2. The agent clarifies only decisions that materially affect the architecture.
3. The agent derives functions, requirements, and interfaces.
4. It constructs a morphological box with several solution principles per function.
5. Compatibility constraints eliminate invalid combinations.
6. A small beam of promising system concepts is retained.
7. Gladius generates low-fidelity envelope models for these concepts.
8. Geometry, printability, proportions, and visual character are evaluated.
9. The agent or user selects, combines, or rejects variants.
10. The selected concept is developed hierarchically to component and feature level.
11. Gladius performs deterministic validation before completion.
12. The user receives a structured, editable, parametric model rather than a flattened generated mesh.

---

## 4. Architectural overview

```text
┌──────────────────────────── Gladius ─────────────────────────────┐
│                                                                  │
│  Dear ImGui                                                      │
│  ┌──────────────┐  ┌─────────────────────┐  ┌─────────────────┐ │
│  │ Design tree  │  │ 3D viewport         │  │ Agent / chat    │ │
│  │ and variants │  │ and comparisons     │  │ and activity    │ │
│  └──────┬───────┘  └──────────▲──────────┘  └────────┬────────┘ │
│         │                     render                   │          │
│         └──────────────────────┼───────────────────────┘          │
│                                │                                  │
│                     ┌──────────▼──────────┐                       │
│                     │ Native agent harness│                       │
│                     │                     │                       │
│                     │ conversation        │                       │
│                     │ design process      │                       │
│                     │ provider adapters   │                       │
│                     │ permission policy   │                       │
│                     │ transactions        │                       │
│                     └──────────┬──────────┘                       │
│                                │                                  │
│                     ┌──────────▼──────────┐                       │
│                     │ Gladius Agent API   │                       │
│                     │ and Tool Registry   │                       │
│                     └──────────┬──────────┘                       │
│                                │                                  │
│           ┌────────────────────┼────────────────────┐             │
│           ▼                    ▼                    ▼             │
│      Design graph       Implicit geometry       Validation        │
│           │                    │                    │             │
│           └────────────────────┼────────────────────┘             │
│                                ▼                                  │
│                         Gladius model                              │
└──────────────────────────────────────────────────────────────────┘
                  │
                  ▼
        LLM and image-generation APIs
```

MCP is not used internally. Instead, MCP and eventually WebMCP become adapters around the same canonical tool registry:

```text
                         Gladius Tool Registry
                                  ▲
                  ┌───────────────┼───────────────┐
                  │               │               │
          Native design agent   MCP server      WebMCP
                  │               │               │
                ImGui           stdio           browser
```

---

## 5. The design process

### 5.1 Design phases

The agent operates in explicit phases:

```cpp
enum class DesignPhase
{
    ClarifyRequirements,
    DecomposeFunctions,
    GenerateSolutionPrinciples,
    BuildSystemConcepts,
    EvaluateConcepts,
    InstantiateEnvelopes,
    DevelopComponents,
    IntegrateSystem,
    Validate,
    Refine,
    Complete
};
```

The phase is not intended to rigidly script the design. It constrains which activities are currently useful and prevents premature detailing.

### 5.2 Hierarchical decomposition

The design is represented at several levels:

```text
System
├── Function groups
│   ├── Functions
│   └── Solution principles
├── Components
│   ├── Component variants
│   └── Interfaces
├── Features
│   ├── Feature variants
│   └── Parameters
└── Complete configurations
```

For a tea-light cottage:

```text
Tea-light cottage
├── Contain light source
├── Provide access
├── Distribute light
├── Form enclosure
├── Provide roof/weather motif
├── Integrate garden
├── Separate colors/materials
└── Enable support-free manufacture
```

### 5.3 Morphological box

Each function receives several possible solution principles:

| Function | Variant A | Variant B | Variant C |
|---|---|---|---|
| Light insertion | Removable roof | Bottom opening | Rear door |
| Light distribution | Open windows | Thin translucent walls | Perforated pattern |
| Main structure | Hollow shell | Log construction | Frame and panels |
| Roof | Gable | Hipped | Stepped snow roof |
| Color separation | Separate parts | Material regions | Inlays |
| Garden integration | Shared base | Detachable modules | Interlocking tiles |
| Ventilation | Chimney | Roof gap | Hidden rear slots |

A system concept selects compatible principles across multiple rows.

### 5.4 Compatibility filtering

The Cartesian product of all variants must not be instantiated. Compatibility rules eliminate impossible or undesirable combinations before geometry creation.

Examples:

- a bottom-inserted light is incompatible with a permanently closed shared base;
- a removable roof conflicts with snow geometry that joins the roof to another fixed component;
- a thin translucent wall may not support a heavy detachable roof without a structural frame;
- four-tool FDM limits the number and arrangement of material regions.

Compatibility can be represented as:

```cpp
struct CompatibilityRule
{
    std::vector<VariantPredicate> conditions;
    CompatibilityResult result;
    std::string rationale;
};
```

### 5.5 Beam search over concepts

A practical staged search might be:

```text
20 symbolic combinations
        ↓ filter and estimate
5 promising system concepts
        ↓ instantiate envelopes
3 coarse geometric concepts
        ↓ render and validate
2 structural concepts
        ↓ select or hybridize
1 detailed design
```

The exact beam width should remain configurable. Interactive use will likely favor approximately three visible geometric variants at a time.

---

## 6. Multi-fidelity realization

Alternatives should be evaluated at the cheapest fidelity that can answer the current question.

### Level 0: symbolic concept

Contains:

- functions;
- selected solution principles;
- interfaces;
- estimated properties;
- constraints and rationale.

No geometry is generated.

### Level 1: envelope model

Uses simple boxes, cylinders, profiles, and major openings.

Evaluates:

- overall size;
- spatial layout;
- approximate volume;
- collisions;
- proportions;
- basic fit and access.

### Level 2: structural model

Adds:

- shells;
- wall thickness;
- assemblies;
- interfaces;
- functional openings;
- load-bearing or printable structures.

Evaluates:

- printability;
- assembly;
- light access;
- stability;
- clearance;
- component compatibility.

### Level 3: detailed design

Adds:

- decorative patterns;
- logs, snow, plants, window frames;
- final color/material regions;
- manufacturing details.

Only selected concepts should reach this level.

---

## 7. Design graph rather than one mutable model

The agent should operate on a design graph containing alternatives, not merely on one active CSG tree.

```text
Design graph
├── requirements
├── functions
├── constraints
├── solution principles
├── interfaces
├── variants
├── compatibility relationships
├── evaluations
├── geometric realizations
└── configurations
```

The model displayed in the viewport is one selected configuration through the graph:

```text
Configuration A =
    Base.Integrated
  + Shell.LogWall
  + Roof.Gable
  + LightAccess.RearDoor
  + Garden.SharedBase
```

This enables:

- switching one component variant without rebuilding everything;
- preserving rejected alternatives;
- comparison and hybridization;
- local branching;
- traceable design decisions;
- freezing completed components;
- returning to earlier concepts.

Possible core structures:

```cpp
struct DesignNode
{
    DesignNodeId id;
    DesignNodeType type;
    std::string name;

    std::vector<RequirementId> addressedRequirements;
    std::vector<DesignNodeId> children;
    std::vector<DesignVariantId> alternatives;

    DesignStatus status;
};

struct DesignVariant
{
    DesignVariantId id;
    std::string name;
    std::string rationale;

    nlohmann::json parameters;
    std::vector<Constraint> constraints;
    std::vector<Interface> interfaces;

    Evaluation evaluation;
    GeometryHandle geometry;
};
```

---

## 8. Interfaces and substitutable components

Component alternatives must expose explicit interfaces. Without stable interfaces, replacing one variant would force the agent to reconstruct dependent components.

A roof family might require:

```text
Inputs
- wall-top outline
- local coordinate system
- target pitch or target height
- overhang
- material assignment

Outputs
- lower mounting region
- chimney attachment region
- snow attachment region
- removable grip region
- exclusion volume
```

For implicit geometry, interfaces may include:

- coordinate systems;
- named parameters;
- boundary fields;
- attachment regions;
- clearance and exclusion volumes;
- material boundaries;
- dependency references.

A conceptual interface could be:

```cpp
class IRoofVariant
{
public:
    virtual GeometryHandle instantiate(
        const RoofInterface& host,
        const RoofParameters& parameters) = 0;
};
```

This should be treated as a design protocol rather than necessarily as a C++ inheritance hierarchy.

---

## 9. Engineering design catalogues as executable knowledge

A design catalogue should contain reusable solution principles that are partly machine-readable and directly instantiable.

Example:

```json
{
  "id": "enclosure.hollow_shell",
  "functions": [
    "contain_light",
    "transmit_light",
    "support_roof"
  ],
  "parameters": {
    "width": {
      "type": "length",
      "minimum": 40,
      "maximum": 200
    },
    "depth": {
      "type": "length",
      "minimum": 40,
      "maximum": 200
    },
    "height": {
      "type": "length",
      "minimum": 40,
      "maximum": 250
    },
    "wallThickness": {
      "type": "length",
      "minimum": 1.2
    }
  },
  "manufacturing": {
    "processes": ["FDM", "SLS"],
    "supportsRequired": false
  },
  "interfaces": [
    "roof_mount",
    "base_mount",
    "window_opening"
  ],
  "generator": "create_hollow_enclosure"
}
```

The LLM provides:

- interpretation of the user's goal;
- decomposition into functions;
- selection and combination of catalogue entries;
- generation of new principles when catalogues are insufficient;
- trade-off reasoning;
- design rationale;
- subjective visual assessment.

Gladius provides:

- parameter enforcement;
- geometry instantiation;
- interface checking;
- measurement;
- rendering;
- deterministic validation;
- model transactions and undo.

---

## 10. Evaluation model

Evaluation must clearly separate calculated facts from judgments.

### 10.1 Deterministic metrics

Calculated by Gladius:

- dimensions and build-volume compliance;
- volume and approximate material usage;
- number of components;
- number of materials;
- minimum wall or feature thickness;
- unsupported overhang area;
- bridge lengths;
- clearances;
- disconnected regions;
- collisions and intersections;
- tea-light or component fit;
- 3MF validity;
- CSG or implicit evaluation validity.

### 10.2 Rule-based heuristic metrics

Derived from explicit engineering rules:

- assembly complexity;
- accessibility;
- expected fragility;
- cleaning difficulty;
- number of manufacturing steps;
- color/tool transition complexity;
- likelihood of support requirement.

### 10.3 Subjective criteria

Assessed by a multimodal model and preferably exposed to the user:

- visual coherence;
- style match;
- perceived character;
- similarity to a reference image;
- novelty;
- balance and proportions.

Results should be presented as evidence rather than collapsed immediately into an arbitrary score:

```text
Concept B

Deterministic
✓ Fits the target footprint
✓ Uses four materials
✓ Minimum wall thickness: 1.6 mm
⚠ Two bridges are approximately 18 mm
✓ Tea-light radial clearance: 2.4 mm

Heuristic
+ Simple two-part assembly
- Roof removal is partly obstructed by the chimney

Subjective
+ Strong silhouette
+ Good match to the concept image
- Garden appears visually disconnected
```

Weighted scores may be used internally for pruning, but the underlying evidence should remain accessible.

---

## 11. Iterative agent loop

The native harness can remain intentionally small. It does not need to encode all design intelligence in C++.

```cpp
while (!finished && iteration++ < limits.maxModelTurns)
{
    auto response = co_await provider.complete(
        buildContext(),
        tools.definitions(),
        cancellationToken);

    conversation.append(response);
    publishToUi(response);

    for (const auto& call : response.toolCalls())
    {
        auto result = co_await executeWithPolicy(call);
        conversation.appendToolResult(call.id, result);
    }

    finished = designState.isComplete();
}
```

The quality comes primarily from `buildContext()`:

```text
system instructions
+ conversation
+ user requirements
+ current design phase
+ current design graph summary
+ active configuration
+ available tools
+ manufacturing constraints
+ relevant catalogue entries
+ deterministic evaluation results
+ current viewport render
+ optional generated reference image
```

The model is instructed to work iteratively:

1. inspect before modifying;
2. operate at the correct design level;
3. preserve frozen components;
4. branch when alternatives are useful;
5. use low fidelity first;
6. render and inspect intermediate models;
7. validate before increasing detail;
8. explicitly finish rather than stopping implicitly.

---

## 12. Tool hierarchy

Tools should exist at several abstraction levels.

### Observation

```text
get_design_summary
get_active_configuration
get_model_summary
get_node
get_children
get_bounding_box
get_materials
measure_distance
measure_thickness
get_volume
capture_viewport
validate_model
check_printability
```

### Design-space management

```text
add_requirement
add_function
propose_solution_principle
define_interface
create_variant
reject_variant
freeze_component
create_configuration
activate_configuration
compare_variants
hybridize_variants
```

### Low-level geometry

```text
create_box
create_cylinder
create_sphere
create_profile
boolean_union
boolean_intersection
boolean_difference
transform
set_parameter
```

### Structural geometry

```text
create_shell
create_pattern
create_repetition
create_profile_extrusion
create_hollow_enclosure
create_interface_feature
```

### Design-level catalogue tools

```text
create_gabled_roof
create_hollow_lantern_body
create_log_wall
create_window_array
create_ventilation_pattern
create_support_free_overhang
```

The LLM should prefer the highest abstraction level that preserves the required design freedom. Low-level tools remain available for exceptions and novel geometry.

---

## 13. Rendering and multimodal feedback

The agent should repeatedly compare:

1. the intended design, represented by requirements and optional generated reference images;
2. the actual current design, represented by model semantics and viewport captures.

```text
Reference concept image ─┐
                         ├──► multimodal model ─► proposed changes
Current viewport render ─┘
```

A `capture_viewport` tool should support:

```cpp
struct CaptureViewportRequest
{
    CameraSelection camera;
    int width;
    int height;
    bool includeOverlays;
    std::optional<DesignVariantId> variant;
};
```

This enables the loop:

```text
modify geometry
      ↓
render
      ↓
capture viewport
      ↓
visual assessment
      ↓
select next modification
```

Image generation is an agent-side capability, not a Gladius domain tool. Generated images become conversation and design-reference artifacts.

---

## 14. Transactions, checkpoints, and undo

Agent changes must be grouped into meaningful operations.

```text
begin_edit("Create cottage roof")
    create profile
    extrude roof
    add overhang
    assign material
    render preview
commit_edit()
```

or:

```text
rollback_edit()
```

This prevents dozens of primitive operations from polluting the user-facing undo history.

Recommended checkpoint levels:

```text
Checkpoint 0: initial model
Checkpoint 1: selected system architecture
Checkpoint 2: envelope model
Checkpoint 3: structural model
Checkpoint 4: integrated model
Checkpoint 5: detailed design
```

Useful operations:

```text
create_checkpoint
restore_checkpoint
compare_checkpoint
fork_from_checkpoint
```

The agent should be able to revert a failed experiment without manually reversing every tool call.

---

## 15. Safety and authority model

Tools should declare their effect category:

```cpp
enum class ToolPolicy
{
    ReadOnly,
    ViewModification,
    DesignGraphModification,
    ModelModification,
    ExternalSideEffect
};
```

Suggested behavior:

- read-only inspection executes automatically;
- camera, selection, and variant previews execute automatically;
- changes inside a temporary variant branch execute automatically;
- committing changes to the user's active model requires a visible transaction;
- destructive or external actions require confirmation;
- exporting or overwriting files requires confirmation.

An autonomy setting can control interaction frequency:

```text
Ask frequently
Ask at major design decisions
Work autonomously until validation or completion
```

The recommended default is **ask at major design decisions**.

---

## 16. Native UI concept

Chat alone is insufficient because it hides the design state. A practical Gladius layout is:

```text
┌────────────────────┬──────────────────────────┬──────────────────┐
│ Design tree        │ 3D viewport              │ Agent            │
│                    │                          │                  │
│ Cottage            │                          │ Three roof       │
│ ├─ Base       ✓    │                          │ variants are     │
│ ├─ Shell      ✓    │                          │ ready.           │
│ ├─ Roof       ◉    │                          │                  │
│ │  ├─ Gable   A    │                          │ A is simplest.   │
│ │  ├─ Hip     B    │                          │ C best matches   │
│ │  └─ Curved  C    │                          │ the reference.   │
│ ├─ Windows         │                          │                  │
│ └─ Garden          │                          │ [Compare A/B/C]  │
└────────────────────┴──────────────────────────┴──────────────────┘
```

Required interactions include:

- activate a variant;
- compare variants side by side;
- freeze a component;
- request more alternatives;
- reject a principle;
- hybridize variants;
- restore a checkpoint;
- inspect deterministic evaluations;
- approve a change transaction;
- stop or cancel agent execution;
- expand a developer trace of raw tool calls.

Routine tool activity should be collapsed for normal users:

```text
✓ Inspected model · 4 tools
✓ Generated 3 roof variants
⚠ Variant C has an unsupported overhang
```

---

## 17. C++ implementation strategy

### 17.1 Recommended dependency boundary

Use established libraries for commodity plumbing but own the agent and design abstractions.

Likely dependencies:

- Dear ImGui for UI;
- `nlohmann-json` for JSON;
- `libcurl` or an existing Gladius HTTP layer for HTTPS and streaming;
- operating-system credential stores for API keys;
- existing Gladius geometry, rendering, and 3MF infrastructure.

Avoid making an immature C++ agent framework a foundational dependency. The provider protocol is primarily HTTPS plus JSON or SSE. The strategically important parts are Gladius-specific and should be owned by the project.

### 17.2 Core native abstractions

```text
Agent/
├── Conversation
├── AgentLoop
├── DesignProcess
├── ContextBuilder
├── PermissionPolicy
├── TransactionManager
└── ProviderCapabilities

Providers/
├── IAiProvider
├── OpenAiProvider
└── OpenAiCompatibleProvider

Design/
├── DesignGraph
├── DesignNode
├── DesignVariant
├── Configuration
├── Requirement
├── Constraint
├── Interface
├── Evaluation
└── Catalogue

Tools/
├── ToolRegistry
├── ToolDefinition
├── ToolExecutor
└── Gladius tools

UI/
├── AiChatPanel
├── DesignTreePanel
├── VariantComparisonPanel
├── AgentActivityPanel
└── ProviderSettingsPanel
```

### 17.3 Provider-neutral message model

```cpp
struct TextContent
{
    std::string text;
};

struct ImageContent
{
    std::vector<std::byte> data;
    std::string mimeType;
};

struct ToolCall
{
    std::string id;
    std::string name;
    nlohmann::json arguments;
};

struct ToolResult
{
    std::string callId;
    nlohmann::json value;
};

using Content = std::variant<
    TextContent,
    ImageContent,
    ToolCall,
    ToolResult>;
```

### 17.4 Tool registry

```cpp
struct ToolDefinition
{
    std::string name;
    std::string description;
    nlohmann::json inputSchema;
    ToolPolicy policy;
};

class ITool
{
public:
    virtual ~ITool() = default;
    virtual ToolDefinition definition() const = 0;
    virtual Task<nlohmann::json> execute(
        const nlohmann::json& arguments,
        CancellationToken cancellation) = 0;
};
```

The registry should serve:

- the embedded native agent;
- the existing MCP server;
- automated tests;
- a future WebMCP adapter.

### 17.5 Threading and streaming

Provider requests should execute outside the render thread. Events are passed to the UI through a thread-safe queue:

```text
HTTP worker
    │
    │ AgentEvent
    ▼
thread-safe queue
    │
    ▼
main/render thread
    │
    ▼
ImGui panels
```

Events may include:

```text
TextDelta
ToolCallStarted
ToolCallCompleted
ImageGenerated
VariantCreated
ValidationCompleted
UserDecisionRequired
AgentCompleted
AgentFailed
```

---

## 18. BYOK and privacy

A native implementation makes BYOK safer than browser storage because credentials can be stored in:

- Windows Credential Manager;
- macOS Keychain;
- Linux Secret Service/libsecret.

The settings UI should distinguish AI capabilities:

```text
Reasoning provider
Reasoning model
Image-generation provider
Image-generation model
API endpoint
API key
```

Privacy controls should be explicit:

```text
Allow sending conversation text
Allow sending model metadata
Allow sending deterministic validation results
Allow sending viewport images
Allow sending generated reference images
Allow sending raw geometry
```

The recommended default is:

- keep 3MF files and geometry local;
- send only tool schemas, selected structured results, conversation text, and explicitly captured viewport images;
- do not expose a raw-mesh tool by default;
- make every remote image submission visible in the activity trace.

---

## 19. Completion and validation

The absence of further tool calls should not implicitly mean that a design is complete. The agent should call an explicit operation:

```text
finish_design
```

Before completion is accepted, Gladius runs configured validation:

```text
finish_design
      │
      ▼
CSG / implicit validity
3MF validity
build-volume compliance
minimum feature checks
material-count checks
manufacturing constraints
      │
  ┌───┴────┐
 valid    invalid
  │          │
complete   structured issues returned to agent
```

Hard execution limits remain necessary:

```cpp
struct AgentLimits
{
    std::size_t maxModelTurns;
    std::size_t maxToolCalls;
    std::size_t maxViewportReviews;
    std::chrono::minutes maxWallTime;
};
```

These limits should prevent uncontrolled cost and endless polishing.

---

## 20. Proposed first prototype

The first prototype should demonstrate design-space exploration, not merely chat and tool calling.

### Target object: parametric table lamp

Functional decomposition:

```text
Lamp
├── Stand
├── Shade
├── Light opening
├── Cable route
└── Decorative pattern
```

Provide two or three catalogue variants for each major component.

### Prototype workflow

1. User requests a support-free FDM lamp.
2. Agent extracts requirements and constraints.
3. Agent constructs a small morphological box.
4. Agent generates three compatible system concepts.
5. Gladius instantiates envelope geometry for all three.
6. The viewport renders comparable previews.
7. Deterministic checks calculate size, wall thickness, bridges, and overhangs.
8. The multimodal model assesses proportions and visual character.
9. The user or agent selects or hybridizes concepts.
10. The selected concept is developed to structural fidelity.
11. A decorative pattern is explored through local variants.
12. The final model is validated and committed.

### Initial tool set

A useful initial target is approximately 12–20 tools:

```text
Design
- add_requirement
- add_function
- create_variant
- create_configuration
- activate_configuration
- freeze_component

Geometry
- create_primitive
- create_shell
- create_pattern
- transform
- set_parameter

Inspection
- get_design_summary
- get_model_summary
- get_bounding_box
- capture_viewport

Validation
- measure_thickness
- check_printability
- validate_model

Lifecycle
- begin_edit
- commit_edit
- rollback_edit
- finish_design
```

### Success criteria

The prototype is successful if it demonstrates:

- at least three genuinely different system concepts;
- explicit functional decomposition;
- selective geometric instantiation rather than full Cartesian exploration;
- variant substitution without rebuilding unrelated components;
- deterministic pruning of at least one weak concept;
- visual comparison through viewport captures;
- user selection or hybridization;
- a valid editable parametric result;
- a comprehensible transaction and undo history.

---

## 21. Development roadmap

### Phase 1: common agent-facing API

- Extract a provider-neutral Gladius tool registry.
- Refactor existing MCP tools to call the registry.
- Introduce tool policies and structured results.
- Add transaction support for grouped AI operations.

### Phase 2: minimal native harness

- Implement OpenAI-compatible HTTPS/JSON access.
- Add SSE streaming and cancellation.
- Build provider-neutral conversation structures.
- Implement the bounded tool-calling loop.
- Store BYOK credentials in the OS credential store.

### Phase 3: minimal ImGui experience

- Add chat and activity panels.
- Render streaming text and images.
- Add stop, retry, confirmation, and developer trace.
- Add viewport capture as multimodal input.

### Phase 4: design graph and variants

- Implement functions, principles, variants, configurations, and interfaces.
- Add freeze, fork, compare, and activate operations.
- Build a design-tree panel.
- Implement low-fidelity variant realization.

### Phase 5: catalogues and evaluation

- Add machine-readable catalogue entries.
- Implement compatibility rules.
- Add deterministic manufacturing metrics.
- Implement beam-style concept pruning.
- Add side-by-side variant evaluation.

### Phase 6: image-supported design

- Add image-generation capability.
- Store generated references as design artifacts.
- Compare reference images with viewport renders.
- Support the progression from visual concept to structured geometry.

### Phase 7: external adapters

- Reuse the canonical tool registry for MCP.
- Add WebMCP when browser support is mature enough.
- Consider integration with external Copilot-style agents without making them a dependency of the native workflow.

---

## 22. Key risks and mitigations

### Combinatorial explosion

**Risk:** Too many functions and variants create an unmanageable design space.

**Mitigation:** Compatibility filtering, cheap symbolic evaluation, configurable beam width, and multi-fidelity instantiation.

### Arbitrary LLM scoring

**Risk:** The model presents subjective preferences as objective engineering results.

**Mitigation:** Separate deterministic, heuristic, and subjective evidence. Keep calculated metrics visible.

### Low-level tool-call explosion

**Risk:** Primitive-only geometry requires hundreds of calls.

**Mitigation:** Hierarchical tools, reusable catalogue generators, patterns, arrays, and parameterized components.

### Unstable edits

**Risk:** Changing one feature unintentionally alters the entire design.

**Mitigation:** Component interfaces, frozen nodes, variant branches, transactions, and checkpoints.

### Infinite refinement

**Risk:** The agent keeps polishing without converging.

**Mitigation:** Hard limits, explicit phase progression, explicit `finish_design`, and deterministic completion criteria.

### Provider lock-in

**Risk:** The architecture mirrors one vendor's API.

**Mitigation:** Provider-neutral conversation, content, tool, vision, and image-generation abstractions.

### Privacy and industrial data

**Risk:** Proprietary 3MF geometry is sent to an external model.

**Mitigation:** Keep geometry local, expose semantic summaries, require explicit permission for screenshots or geometry, and maintain a visible transfer trace.

### Immature C++ agent libraries

**Risk:** A framework dependency becomes abandoned or constrains the architecture.

**Mitigation:** Own the small harness and use established libraries only for HTTP, TLS, JSON, and credential storage.

---

## 23. Central design principles

1. **The LLM proposes and orchestrates; Gladius calculates and validates.**
2. **The design graph contains alternatives; the viewport shows one configuration.**
3. **Generate alternatives before committing to detail.**
4. **Use the cheapest representation that answers the current design question.**
5. **Prefer semantic and parametric tools over long primitive call sequences.**
6. **Treat component interfaces as first-class design objects.**
7. **Keep deterministic facts separate from subjective judgments.**
8. **Preserve user control through freezing, variants, checkpoints, and transactions.**
9. **Keep raw geometry local unless the user explicitly permits transfer.**
10. **Build one canonical Gladius tool API and adapt it to native agents, MCP, and WebMCP.**

---

## 24. Conclusion

The proposed native Gladius design agent is not primarily a chatbot and not a one-shot text-to-CAD generator. It is a structured design-space exploration system combining classical engineering design methods with modern multimodal models and deterministic implicit geometry.

The strongest differentiator is the combination of:

- functional decomposition;
- morphological exploration;
- executable engineering catalogues;
- substitutable component variants;
- multi-fidelity geometry;
- deterministic manufacturing validation;
- visual feedback through rendering;
- user-guided selection and hybridization.

This architecture fits Gladius particularly well because the application can expose design semantics, generate parameterized implicit geometry, render variants quickly, and retain a structured editable model. The native agent should therefore be developed as a first-class Gladius subsystem, while MCP and WebMCP remain external adapters to the same underlying domain API.
