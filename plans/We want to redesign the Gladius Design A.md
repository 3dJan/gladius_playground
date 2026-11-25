We want to redesign the Gladius Design Agent UI.

The application uses Dear ImGui with dockable windows. The existing Design Agent UI is functional but currently exposes several largely independent panels such as:

- Design Tree / requirements / planning state
- Variant Comparison
- Design Agent conversation/activity
- developer trace / diagnostics
- prompt input

The new UI should consolidate the user-facing agent experience into ONE dockable "Design Agent" side panel.

Do not redesign the main Gladius viewport. The 3D viewport should remain the primary workspace. The agent should live completely in its dockable side panel.

Before changing code:
1. Inspect the existing Design Agent UI implementation.
2. Identify the current model/state objects for phases, requirements, design structure, variants, agent activity and conversation.
3. Reuse those models where practical.
4. Do not invent a second parallel state model just for the new UI.
5. Keep developer/debug functionality available, but it may be moved into collapsible diagnostics sections.

## Core UX concept

The central UI element should be a VERTICAL WORKFLOW TREE.

The high-level phases are:

    Understand
        ↓
    Decompose
        ↓
    Explore
        ↓
    Develop
        ↓
    Validate

This is simultaneously:

1. a progress indicator,
2. a navigation mechanism,
3. a hierarchy of artifacts produced by the design agent.

Each phase is represented by a circle on a continuous vertical line.

Example:

    ✓  Understand
    │
    │    Requirements
    │      Diameter        100 mm
    │      Thickness         8 mm
    │      Min. wall       ≥ 0.8 mm
    │      Periodicity     Required
    │
    ✓  Decompose
    │
    │    Design structure
    │      ▾ Filter disc
    │          Envelope
    │          Rib field
    │          Rim
    │
    ◉  Explore
    │
    │    Rib field
    │
    │      A  Alternating waves
    │      B  Gentle waves
    │      C  Straight ribs
    │
    ○  Develop
    │
    ○  Validate

The important idea is that these are NOT separate panels inside the Design Agent window. They form one visual tree.

## Phase states

Support at least:

    Pending
    Active
    Completed
    Attention/Error

Suggested appearance:

Pending:
- dark/gray hollow circle
- muted title

Active:
- larger accent-colored ring
- subtle animated glow/pulse
- bold/bright title

Completed:
- green or muted accent circle with checkmark
- no animation

Attention/Error:
- warning/error accent
- clear but restrained indication

Do NOT show fake percentage completion.

The agent often cannot know that it is "67% through Explore".

## Vertical progress line

Draw one continuous vertical line connecting the phase circles.

The line should visually establish the workflow direction.

For the active phase, add restrained animation.

For example:
- a small luminous particle/pulse slowly moves along a small section of the line near the active phase
- or the active circle has a slow breathing/pulsating ring

Target animation cycle:
approximately 1.5–3 seconds.

Avoid:
- rainbow effects
- fast spinners
- aggressive flashing
- excessive glow
- web/consumer AI aesthetics

This is engineering/CAD software. The visual language should feel like a sophisticated engineering instrument.

Use ImGui::GetTime() for animation and keep it independent of agent execution.

## Expandable phases

Each phase should behave similarly to an ImGui tree node.

The user can expand/collapse completed and active phases.

The phase's artifacts appear underneath it, indented to the right of the vertical progress line.

The connecting progress line must remain visually continuous even when a phase is expanded.

### Understand

When expanded, Understand contains Requirements.

Example:

    ✓ Understand
    │
    │  Requirements
    │
    │    Diameter       100 mm
    │    Thickness        8 mm
    │    Min. wall      ≥ 0.8 mm
    │    Periodicity    Required

Distinguish where possible between:
- hard requirements/constraints
- preferences/objectives
- assumptions

Do not present requirements as validated simply because the agent extracted them.

### Decompose

When expanded, Decompose contains the functional/component Design Structure.

Example:

    ✓ Decompose
    │
    │  Design structure
    │
    │  ▾ Filter disc
    │      Envelope
    │      Rib field
    │      Rim

Reuse our existing design tree information.

The user should eventually be able to select these items and have them correspond to/designate elements in the Gladius scene/graph, but do not introduce large architectural changes solely to accomplish this in the initial UI refactoring.

### Explore

Explore contains design alternatives generated for the currently explored design element.

Example:

    ◉ Explore

       Rib field
       2 of 3 alternatives ready

       ┌──────────────────────────┐
       │ [thumbnail] A Waves      │
       │             Ready        │
       └──────────────────────────┘

       ┌──────────────────────────┐
       │ [thumbnail] B Diamond    │
       │             Ready        │
       └──────────────────────────┘

       ┌──────────────────────────┐
       │ [thumbnail] C Gyroid     │
       │             Rendering... │
       └──────────────────────────┘

       [ Compare ]

Variants should remain compact because the Design Agent is a side panel.

If thumbnails/snapshots already exist in our variant infrastructure, display them.

Do not create fake thumbnails.

The currently inspected/selected variant gets a subtle accent border.

Variant generation is asynchronous, so handle states such as:

- queued
- generating
- rendering/evaluating
- ready
- rejected/failed
- selected

Do not block the UI while alternatives are created.

### Develop

Initially this can contain development/refinement tasks and the currently modified design element.

For example:

    ◉ Develop

       Rib field

       ✓ Set initial wavelength
       ✓ Adjust amplitude
       ◉ Optimize minimum wall
       ○ Check connectivity

Do not expose raw LLM reasoning.

These are user-facing operations/status derived from explicit agent/tool state.

### Validate

Validation should contain deterministic checks where available.

Example:

    ◉ Validate

       ✓ Envelope
         100.0 mm ≤ 100.0 mm

       ✓ Minimum wall
         0.83 mm ≥ 0.80 mm

       ◉ Connectivity
         Evaluating...

       ○ Periodicity

This distinction is important:

An LLM saying that a criterion is satisfied is NOT equivalent to
a deterministic Gladius evaluator measuring that it is satisfied.

The UI should make measured/evaluated results visually different from
agent assumptions or unevaluated requirements.

## Transitions / eye candy

We explicitly want attractive but restrained transitions between phases.

When a phase completes:

1. briefly brighten/pulse its circle
2. replace active state with completed checkmark
3. animate a light pulse down the connecting line
4. activate the next phase circle
5. automatically expand the new phase if appropriate

Suggested timings:

completion pulse:     ~200 ms
line transition:      ~300-500 ms
new phase fade/pulse: ~250 ms

Approximately 0.5–1 second total.

The animation must never delay actual execution.

Agent state is authoritative; animation simply interpolates the visual presentation.

If the agent moves through states faster than the animation, the UI must gracefully catch up rather than queue several seconds of animations.

## Tree item appearance animations

When new artifacts appear, e.g. a newly discovered requirement, decomposition node or variant, they may subtly fade/slide into the tree.

Keep this restrained:
~150–250 ms.

Avoid layout instability.

## Persistent agent controls

At the bottom of the Design Agent panel, keep a compact persistent agent area.

Something like:

    ─────────────────────────────

    ◌ Rendering candidate C...

             [ Pause ] [ Stop ]

    ┌────────────────────────────┐
    │ Ask for a change...      > │
    └────────────────────────────┘

    ▸ Activity log

The prompt input should always be readily accessible rather than being nested into one workflow phase.

Current agent status should use meaningful operation names when available:

Good:
    Rendering candidate C...
    Evaluating minimum wall...
    Generating flow alternatives...

Avoid generic status where better information exists:
    Agent is thinking (421s)...

A small animated activity indicator is sufficient.

## Chat

Do not make the raw conversation transcript the dominant UI.

User/agent communication remains useful, but the DESIGN STATE represented by the workflow tree is the primary record.

Use:
- short agent messages/results
- actionable information
- user requests

Avoid putting full internal reasoning or huge model responses into the normal user UI.

Existing detailed output can remain accessible through Activity Log / diagnostics.

## Developer diagnostics

We still need the existing developer capabilities while developing the agent.

Preserve functionality such as:
- model information
- developer trace
- copy trace
- tool activity where available
- raw/diagnostic logs
- stop/pause controls

Move developer-specific information underneath a collapsible section such as:

    ▸ Activity log
    ▸ Developer diagnostics

Do not remove useful debugging functionality simply to make the UI prettier.

## ImGui implementation

This should remain idiomatic Dear ImGui.

Do not introduce a new heavyweight UI framework.

Prefer normal ImGui primitives plus ImDrawList for custom workflow rendering.

For example:
- ImGui::TreeNode / custom disclosure interaction
- ImGui::BeginChild where useful
- ImGui::InvisibleButton for custom phase nodes
- ImDrawList::AddLine
- ImDrawList::AddCircle
- ImDrawList::AddCircleFilled
- ImDrawList::AddPolyline
- ImGui::GetTime() for animation
- ImGui::GetColorU32 for theme integration

Avoid hard-coding the workflow visualization to a particular screen resolution.

It must behave correctly when:
- the panel becomes narrower/wider
- the user docks it elsewhere
- UI scaling changes
- labels become longer
- phases contain many artifacts

The panel must remain scrollable.

The prompt/footer should remain usable when the workflow is taller than the available panel.

Use clipping where appropriate because design trees and variant lists may eventually become large.

## Styling

Follow the existing Gladius theme rather than introducing an unrelated UI style.

Use the mockup concept as direction, not something that must be reproduced pixel-for-pixel.

Desired visual hierarchy:

normal background:
    existing Gladius dark theme

completed:
    restrained success green

active:
    Gladius accent / violet-blue

pending:
    muted gray

error:
    existing warning/error colors

The active phase is the only element that should have noticeable animation/glow.

Avoid excessive rounded web-style cards. This is still Dear ImGui and should visually belong to Gladius.

## Architecture

Please separate:

1. workflow STATE
2. workflow PRESENTATION
3. animation/transitional PRESENTATION STATE

For example, conceptually:

    AgentWorkflowState
        currentPhase
        phaseStates
        requirements
        designStructure
        alternatives
        validationResults
        ...

    AgentWorkflowView
        render(...)

    AgentWorkflowAnimationState
        previousPhase
        transitionStartTime
        newlyAddedArtifacts
        ...

Do not mutate actual agent state for animation purposes.

Do not have the UI infer important workflow state by parsing human-readable log strings if structured state already exists or can be exposed cleanly.

Prefer enums/structured state over strings such as:

    if (status == "Exploring") ...

The exact architecture should fit the existing Gladius codebase, so inspect it before introducing new types.

## Important behavioral principle

The user should perceive:

    "The agent is progressing through a structured engineering design process."

NOT:

    "A chatbot is calling CAD tools."

The workflow tree should therefore communicate:
- what the agent currently understands
- how it decomposed the problem
- which alternatives it considered
- which concept it is developing
- which requirements have actually been validated

This is much more important than displaying the LLM's textual reasoning.

## Implementation strategy

Please implement iteratively rather than rewriting the entire Design Agent UI at once.

### Step 1
Inspect the existing implementation and identify reusable state and widgets.

### Step 2
Implement the vertical phase rail/widget with:
- pending
- active
- completed
- error states
- expand/collapse
- transition animation

Use representative existing agent state if already available.

### Step 3
Move/represent Requirements under Understand.

### Step 4
Move/represent Design Structure under Decompose.

### Step 5
Integrate existing alternatives/variant data under Explore.

Do not fake functionality if variants are not currently exposed in a reusable form.

### Step 6
Integrate development/validation state that already exists.

### Step 7
Move the normal agent controls/prompt to the fixed bottom portion.

### Step 8
Move existing verbose logs/debug tools into collapsible diagnostics.

Only then spend time on fine styling and animation polish.

## Acceptance criteria

The implementation is successful when:

1. There is one dockable Design Agent panel.
2. The main viewport is not occupied by permanent agent UI.
3. The five design phases form a vertical connected workflow.
4. The active phase is immediately obvious.
5. Completed, active and pending phases are visually distinct.
6. Phase transitions are smoothly animated without affecting execution.
7. Understand expands to show requirements.
8. Decompose expands to show the design structure.
9. Explore can show the existing variant state/preview data where available.
10. Validation distinguishes deterministic results from unvalidated claims.
11. Phases can be expanded/collapsed.
12. Existing important developer diagnostics remain accessible.
13. The UI works at different docked panel widths and DPI scales.
14. The panel remains usable with enough content to require scrolling.
15. No raw chain-of-thought/internal model reasoning is presented as part of the workflow.
16. No fake progress percentages or fake evaluation results are introduced.
17. Existing Design Agent behavior continues to work.

Most importantly: first understand the existing implementation and make the smallest architectural changes needed to support this UI cleanly. Do not replace working agent infrastructure purely because the new UI suggests a different abstraction.