struct FrameUniforms {
    eye_and_max_distance: vec4<f32>,
    forward_and_field_of_view: vec4<f32>,
    right_and_width: vec4<f32>,
    up_and_height: vec4<f32>,
};

@group(0) @binding(0)
var<uniform> frame: FrameUniforms;

@group(0) @binding(1)
var<storage, read_write> output_pixels: array<u32>;

struct Parameters {
    values: array<f32>,
};

@group(0) @binding(2)
var<storage, read> parameters: Parameters;

// GLADIUS_MODEL_EVALUATOR

fn pack_rgba8(color: vec4<f32>) -> u32 {
    let rgba = vec4<u32>(round(clamp(color, vec4<f32>(0.0), vec4<f32>(1.0)) * 255.0));
    return rgba.x | (rgba.y << 8u) | (rgba.z << 16u) | (rgba.w << 24u);
}

fn estimate_normal(position: vec3<f32>) -> vec3<f32> {
    let epsilon = 0.001f;
    let x = evaluateModel(position + vec3<f32>(epsilon, 0.0, 0.0)).w -
        evaluateModel(position - vec3<f32>(epsilon, 0.0, 0.0)).w;
    let y = evaluateModel(position + vec3<f32>(0.0, epsilon, 0.0)).w -
        evaluateModel(position - vec3<f32>(0.0, epsilon, 0.0)).w;
    let z = evaluateModel(position + vec3<f32>(0.0, 0.0, epsilon)).w -
        evaluateModel(position - vec3<f32>(0.0, 0.0, epsilon)).w;
    return normalize(vec3<f32>(x, y, z));
}

@compute @workgroup_size(16, 16, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let width = u32(frame.right_and_width.w);
    let height = u32(frame.up_and_height.w);
    if (global_id.x >= width || global_id.y >= height) {
        return;
    }

    let aspect_ratio = f32(width) / f32(height);
    let screen = ((vec2<f32>(global_id.xy) + vec2<f32>(0.5)) / vec2<f32>(f32(width), f32(height))) * 2.0 - 1.0;
    let half_height = tan(frame.forward_and_field_of_view.w * 0.5);
    let ray_direction = normalize(
        frame.forward_and_field_of_view.xyz +
        frame.right_and_width.xyz * (screen.x * aspect_ratio * half_height) +
        frame.up_and_height.xyz * (-screen.y * half_height));
    let eye = frame.eye_and_max_distance.xyz;
    let max_distance = frame.eye_and_max_distance.w;

    var traveled = 0.0f;
    var hit = false;
    var model = vec4<f32>(0.0);
    for (var step = 0u; step < 256u; step++) {
        let position = eye + ray_direction * traveled;
        model = evaluateModel(position);
        if (abs(model.w) < 0.001f) {
            hit = true;
            break;
        }
        traveled = traveled + max(abs(model.w), 0.0005f);
        if (traveled > max_distance) {
            break;
        }
    }

    let pixel_index = global_id.y * width + global_id.x;
    if (!hit) {
        output_pixels[pixel_index] = pack_rgba8(vec4<f32>(vec3<f32>(0.03, 0.05, 0.08), 1.0));
        return;
    }

    let normal = estimate_normal(eye + ray_direction * traveled);
    let light_direction = normalize(vec3<f32>(0.6, 0.8, 0.5));
    let shade = 0.15 + 0.85 * max(dot(normal, light_direction), 0.0);
    output_pixels[pixel_index] = pack_rgba8(vec4<f32>(model.xyz * shade, 1.0));
}