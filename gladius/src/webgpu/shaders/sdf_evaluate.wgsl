struct EvaluationUniforms {
    iso_value: f32,
    point_count: u32,
    reserved0: u32,
    reserved1: u32,
};

@group(0) @binding(0)
var<uniform> evaluation: EvaluationUniforms;

struct Positions {
    values: array<vec4<f32>>,
};

@group(0) @binding(1)
var<storage, read> positions: Positions;

@group(0) @binding(2)
var<storage, read_write> output_values: array<f32>;

struct Parameters {
    values: array<f32>,
};

@group(0) @binding(3)
var<storage, read> parameters: Parameters;

// GLADIUS_MODEL_EVALUATOR

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let index = global_id.x;
    if (index >= evaluation.point_count) {
        return;
    }

    let model = evaluateModel(positions.values[index].xyz);
    output_values[index] = model.w - evaluation.iso_value;
}
