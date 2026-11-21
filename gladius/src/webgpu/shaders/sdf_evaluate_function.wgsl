struct EvaluationUniforms {
    reserved0: f32,
    point_count: u32,
    workgroups_x: u32,
    reserved1: u32,
};

@group(0) @binding(0)
var<uniform> evaluation: EvaluationUniforms;

// One record of argument components per sample, in the order the generated entry point declares them.
struct FunctionArguments {
    values: array<f32>,
};

@group(0) @binding(1)
var<storage, read> function_arguments: FunctionArguments;

// One record of output components per sample, in the order the generated entry point declares them.
struct FunctionResults {
    values: array<f32>,
};

@group(0) @binding(2)
var<storage, read_write> function_results: FunctionResults;

struct Parameters {
    values: array<f32>,
};

@group(0) @binding(3)
var<storage, read> parameters: Parameters;

// GLADIUS_FUNCTION_ABI
// GLADIUS_MODEL_EVALUATOR

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let index = global_id.x + global_id.y * evaluation.workgroups_x * 64u;
    if (index >= evaluation.point_count) {
        return;
    }

    gladiusCallRecord(index * gladius_argument_components, index * gladius_output_components);
}
