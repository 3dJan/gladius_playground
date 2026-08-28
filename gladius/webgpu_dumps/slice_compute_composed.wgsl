struct SliceUniforms {
    slice_z: f32,
    width: u32,
    height: u32,
    scale: f32,
};

@group(0) @binding(0)
var<uniform> slice: SliceUniforms;

@group(0) @binding(1)
var<storage, read_write> output_pixels: array<u32>;

struct Parameters {
    values: array<f32>,
};

@group(0) @binding(2)
var<storage, read> parameters: Parameters;

fn evaluateModel(position: vec3<f32>) -> vec4<f32> {
let Input_1_pos: vec3<f32> = position;
return vec4<f32>(vec3<f32>(0f, 0f, 0f), -0.25f);
}


fn pack_rgba8(color: vec4<f32>) -> u32 {
    let rgba = vec4<u32>(round(clamp(color, vec4<f32>(0.0), vec4<f32>(1.0)) * 255.0));
    return rgba.x | (rgba.y << 8u) | (rgba.z << 16u) | (rgba.w << 24u);
}

@compute @workgroup_size(16, 16, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    if (global_id.x >= slice.width || global_id.y >= slice.height) {
        return;
    }

    let pixel_index = global_id.y * slice.width + global_id.x;
    let uv = (vec2<f32>(global_id.xy) + vec2<f32>(0.5)) /
        vec2<f32>(f32(slice.width), f32(slice.height));
    let position = vec3<f32>((uv - vec2<f32>(0.5)) * slice.scale, slice.slice_z);
    let model = evaluateModel(position);
    let distance = model.w;
    let shade = select(0.12, 0.92, distance <= 0.0);
    let color = model.xyz * shade;

    output_pixels[pixel_index] = pack_rgba8(vec4<f32>(color, 1.0));
}