struct FrameUniforms {
    eye_and_max_distance: vec4<f32>,
    forward_and_horizontal_scale: vec4<f32>,
    right_and_width: vec4<f32>,
    up_and_height: vec4<f32>,
    vertical_scale_and_max_steps: vec4<f32>,
    first_row_and_count: vec4<f32>,
    time_slice_quality_normal: vec4<f32>,
    flags_mode_reserved: vec4<u32>,
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

const RF_SHOW_BUILDPLATE: u32 = 1u;
const RF_CUT_OFF_OBJECT: u32 = 2u;
const RF_SHOW_FIELD: u32 = 4u;
const RF_SHOW_STACK: u32 = 8u;
const RF_SHOW_COORDINATE_SYSTEM: u32 = 16u;
const RF_DISABLE_SHADOWS: u32 = 65536u;
const RF_DISABLE_AO: u32 = 131072u;

fn evaluate_surface(position: vec3<f32>) -> vec4<f32> {
    let model = evaluateModel(position);
    var distance = model.w;
    let flags = frame.flags_mode_reserved.x;
    let slice_height = frame.time_slice_quality_normal.y;
    if ((flags & RF_CUT_OFF_OBJECT) != 0u && slice_height > 0.0001f) {
        distance = max(distance, position.z - slice_height);
    }
    return vec4<f32>(model.xyz, distance);
}

fn estimate_normal(position: vec3<f32>) -> vec3<f32> {
    let epsilon = max(frame.time_slice_quality_normal.w, 0.0001f);
    let x = evaluate_surface(position + vec3<f32>(epsilon, 0.0, 0.0)).w -
        evaluate_surface(position - vec3<f32>(epsilon, 0.0, 0.0)).w;
    let y = evaluate_surface(position + vec3<f32>(0.0, epsilon, 0.0)).w -
        evaluate_surface(position - vec3<f32>(0.0, epsilon, 0.0)).w;
    let z = evaluate_surface(position + vec3<f32>(0.0, 0.0, epsilon)).w -
        evaluate_surface(position - vec3<f32>(0.0, 0.0, epsilon)).w;
    return normalize(vec3<f32>(x, y, z));
}

fn calc_soft_shadow(position: vec3<f32>, direction: vec3<f32>) -> f32 {
    var result = 1.0f;
    var distance = 0.01f;
    for (var step = 0u; step < 32u; step++) {
        let sample_position = position + direction * distance;
        let sample_distance = evaluate_surface(sample_position).w;
        if (sample_distance < 0.0005f) {
            return 0.0f;
        }
        result = min(result, 12.0f * sample_distance / distance);
        distance = distance + clamp(sample_distance, 0.01f, 0.5f);
        if (distance > 25.0f) {
            break;
        }
    }
    return clamp(result, 0.0f, 1.0f);
}

fn calc_ambient_occlusion(position: vec3<f32>, normal: vec3<f32>) -> f32 {
    var result = 0.0f;
    var scale = 1.0f;
    for (var step = 1u; step <= 5u; step++) {
        let distance = 0.02f * f32(step);
        let sample_distance = evaluate_surface(position + normal * distance).w;
        result = result + (distance - sample_distance) * scale;
        scale = scale * 0.65f;
    }
    return clamp(1.0f - 1.5f * result, 0.0f, 1.0f);
}

fn shade_surface(position: vec3<f32>, ray_direction: vec3<f32>, model: vec4<f32>) -> vec3<f32> {
    let flags = frame.flags_mode_reserved.x;
    let normal = estimate_normal(position);
    let light_direction = normalize(vec3<f32>(0.6f, 0.8f, 0.5f));
    let view_direction = normalize(-ray_direction);
    let half_direction = normalize(light_direction + view_direction);
    let diffuse = max(dot(normal, light_direction), 0.0f);
    let shadow = select(calc_soft_shadow(position + normal * 0.002f, light_direction),
                        1.0f,
                        (flags & RF_DISABLE_SHADOWS) != 0u);
    let occlusion = select(calc_ambient_occlusion(position, normal),
                           1.0f,
                           (flags & RF_DISABLE_AO) != 0u);
    let specular = pow(max(dot(normal, half_direction), 0.0f), 32.0f) * shadow;
    let fresnel = pow(1.0f - max(dot(normal, view_direction), 0.0f), 5.0f);
    let base_color = max(model.xyz, vec3<f32>(0.0f));
    let ambient = 0.12f + 0.18f * occlusion;
    return base_color * (ambient + 0.78f * diffuse * shadow) +
           vec3<f32>(0.18f + 0.32f * fresnel) * specular;
}

@compute @workgroup_size(16, 16, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let width = u32(frame.right_and_width.w);
    let height = u32(frame.up_and_height.w);
    let first_row = u32(frame.first_row_and_count.x);
    let row_count = u32(frame.first_row_and_count.y);
    if (global_id.x >= width || global_id.y >= row_count) {
        return;
    }

    let source_row = global_id.y + first_row;

    let screen = vec2<f32>(
        f32(global_id.x) / f32(max(width - 1u, 1u)) - 0.5,
        0.5 - f32(source_row) / f32(max(height - 1u, 1u)));
    let ray_direction = normalize(
        frame.forward_and_horizontal_scale.xyz +
        frame.right_and_width.xyz * (screen.x * frame.forward_and_horizontal_scale.w) +
        frame.up_and_height.xyz * (-screen.y * frame.vertical_scale_and_max_steps.x));
    let eye = frame.eye_and_max_distance.xyz;
    let max_distance = frame.eye_and_max_distance.w;

    var traveled = 0.0f;
    var hit = false;
    var model = vec4<f32>(0.0);
    let max_ray_steps = u32(frame.vertical_scale_and_max_steps.y);
    for (var step = 0u; step < max_ray_steps; step++) {
        let position = eye + ray_direction * traveled;
        model = evaluate_surface(position);
        if (abs(model.w) < 0.001f) {
            hit = true;
            break;
        }
        traveled = traveled + max(abs(model.w) * max(frame.time_slice_quality_normal.z, 0.25f), 0.0005f);
        if (traveled > max_distance) {
            break;
        }
    }

    let pixel_index = global_id.y * width + global_id.x;
    if (!hit) {
        output_pixels[pixel_index] = pack_rgba8(vec4<f32>(vec3<f32>(0.03, 0.05, 0.08), 1.0));
        return;
    }

    let hit_position = eye + ray_direction * traveled;
    let shaded = shade_surface(hit_position, ray_direction, model);
    let background = vec3<f32>(0.03f, 0.05f, 0.08f);
    let fog = exp(-0.0015f * traveled);
    output_pixels[pixel_index] = pack_rgba8(vec4<f32>(mix(background, shaded, fog), 1.0));
}