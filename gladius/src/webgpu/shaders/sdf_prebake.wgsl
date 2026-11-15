struct PrebakeSdfUniforms {
    bbox_min_and_tolerance: vec4<f32>,
    bbox_max_and_valid: vec4<f32>,
    grid_dims: vec4<u32>,
};

@group(0) @binding(0)
var<uniform> prebake: PrebakeSdfUniforms;

@group(0) @binding(1)
var output_sdf: texture_storage_3d<r32float, write>;

struct Parameters {
    values: array<f32>,
};

@group(0) @binding(2)
var<storage, read> parameters: Parameters;

// GLADIUS_MESH_SDF_MODULE

// GLADIUS_BEAM_SDF_MODULE

// GLADIUS_IMAGE_SAMPLING_MODULE

// GLADIUS_MODEL_EVALUATOR

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let dims = prebake.grid_dims.xyz;
    if (global_id.x >= dims.x || global_id.y >= dims.y || global_id.z >= dims.z) {
        return;
    }

    let denom = vec3<f32>(max(dims - vec3<u32>(1u), vec3<u32>(1u)));
    let uvw = vec3<f32>(global_id) / denom;
    let position = mix(prebake.bbox_min_and_tolerance.xyz, prebake.bbox_max_and_valid.xyz, uvw);
    let signed_distance = evaluateModel(position).w;
    textureStore(output_sdf, vec3<i32>(global_id), vec4<f32>(signed_distance, 0.0, 0.0, 0.0));
}
