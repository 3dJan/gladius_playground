# Function Extraction (Refactoring) – Plan

## Goal

- Enable users to select nodes in a function graph and extract them into a new reusable function, replacing the selection with a FunctionCall node wired to the new function. Handle automatic discovery of inputs/outputs (boundary ports) and rewire all connections.

## Design

- Core logic implemented in a dedicated utility: nodes::FunctionExtractor.
- UI integration in ModelEditor:
  - Menu item: “Extract Function”.
  - Modal dialog to enter the new function name.
  - Uses Document::createNewFunction() to create 3MF resource and nodes::FunctionExtractor to perform extraction and rewiring.

## Contracts and Behavior

- Input: a non-empty selection of nodes, not containing Begin/End.
- New function’s Begin outputs represent external inputs used by the selection.
- New function’s End inputs represent values used by nodes outside the selection.
- Replace original selection by a FunctionCall in the source model, set its FunctionId and rewire inputs/outputs.
- Undo support: store a restore point before extraction.

## Steps

1. Identify boundary inputs: parameters on selected nodes driven by ports whose parents are outside the selection.
2. Identify boundary outputs: output ports of nodes in the selection that are consumed by parameters outside the selection.
3. Clone selected nodes into a fresh model (with Begin/End) and recreate internal links.
4. Create Begin outputs for each unique external input (deduplicated by source port), connect them to the matching cloned parameters.
5. Create End inputs for each unique external output, connect from the cloned output ports.
6. In source, insert a FunctionCall, update its inputs/outputs from the new model, set FunctionId, wire inputs from original external sources and outputs to original consumers.
7. Remove the selected nodes from the source model, place and focus the new call node, update types and layout.

## Data Structures

- Selection: set(NodeId).
- extInputs: vector of { targetParam*, targetParamName, externalPort* }.
- extOutputs: map uniquePortName -> { srcPort*, vector<consumerParam*> }.

## Edge Cases

- Duplicate external inputs: deduplicate by unique port name.
- Multiple consumers of same output: connect the same function output to all consumers.
- Empty outputs (no external consumers): function has no outputs; still allowed.
- Invalid selections (include Begin/End): extraction aborted gracefully.

## Testing

- Unit test: simple selection with external inputs and external consumer.
- Validate: new model has Begin/End and at least one argument and output; source contains a FunctionCall output wired to the End.

## Next Steps (Optional)

- UI preview of inferred inputs/outputs with rename support before extraction.
- Conflict resolution for name collisions with existing function names.
- Preserve spatial arrangement: auto layout inside the new function.
