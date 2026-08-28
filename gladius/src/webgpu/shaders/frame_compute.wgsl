struct FrameUniforms {
    eye_and_max_distance: vec4<f32>,
    forward_and_horizontal_scale: vec4<f32>,
    right_and_width: vec4<f32>,
    up_and_height: vec4<f32>,
    vertical_scale_and_max_steps: vec4<f32>,
    first_row_and_count: vec4<f32>,
    time_slice_quality_normal: vec4<f32>,
    flags_mode_reserved: vec4<u32>,
    clipping_box_min: vec4<f32>,
    clipping_box_max: vec4<f32>,
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
const RF_DISABLE_ADAPTIVE_OMEGA: u32 = 16384u;
const RF_DISABLE_SHADOWS: u32 = 65536u;
const RF_DISABLE_AO: u32 = 131072u;

struct SceneSample {
    distance: f32,
    color: vec3<f32>,
    object_type: f32,
};

fn signed_box(position: vec3<f32>, half_size: vec3<f32>) -> f32 {
    let distance = abs(position) - half_size;
    return length(max(distance, vec3<f32>(0.0))) + min(max(distance.x, max(distance.y, distance.z)), 0.0);
}

fn bounding_box_distance(position: vec3<f32>, minimum: vec3<f32>, maximum: vec3<f32>) -> f32 {
    return signed_box(position - 0.5f * (minimum + maximum), 0.5f * (maximum - minimum));
}

fn has_model_bounds() -> bool {
    return frame.flags_mode_reserved.z != 0u;
}

fn clipping_box_distance(position: vec3<f32>, slice_height: f32) -> f32 {
    let maximum_z = max(slice_height - 0.1f, frame.clipping_box_min.z);
    let maximum = vec3<f32>(frame.clipping_box_max.x,
                            frame.clipping_box_max.y,
                            maximum_z);
    return bounding_box_distance(position, frame.clipping_box_min.xyz, maximum);
}

fn signed_cylinder(position: vec3<f32>, radius: f32, height: f32) -> f32 {
    let distance = abs(vec2<f32>(length(position.xz), position.y)) - vec2<f32>(radius, height * 0.5f);
    return min(max(distance.x, distance.y), 0.0f) + length(max(distance, vec2<f32>(0.0f)));
}

fn signed_cylinder_from_to(position: vec3<f32>, start: vec3<f32>, end: vec3<f32>, radius: f32) -> f32 {
    let axis = end - start;
    let local_position = position - start;
    let axis_squared_length = dot(axis, axis);
    let projected_position = dot(local_position, axis);
    let radial_distance = length(local_position * axis_squared_length - axis * projected_position) -
                          radius * axis_squared_length;
    let axial_distance = abs(projected_position - axis_squared_length * 0.5f) - axis_squared_length * 0.5f;
    let radial_squared = radial_distance * radial_distance;
    let axial_squared = axial_distance * axial_distance * axis_squared_length;
    let distance = select(max(radial_squared, axial_squared),
                          -min(radial_squared, axial_squared),
                          max(radial_distance, axial_distance) < 0.0f);
    return sign(distance) * sqrt(abs(distance)) / axis_squared_length;
}

fn build_platform(position: vec3<f32>) -> f32 {
    let width = 400.0f;
    let depth = 400.0f;
    let height = 20.0f;
    var local_position = position - vec3<f32>(0.5f * width, 0.5f * depth, 0.0f);
    local_position.x = abs(local_position.x);
    local_position.y = abs(local_position.y);

    let position_below_build_plane = local_position - vec3<f32>(0.25f * width,
                                                                  0.25f * depth,
                                                                  -0.5f * height);
    var platform = signed_box(position_below_build_plane, vec3<f32>(0.25f * width,
                                                                      0.25f * depth,
                                                                      0.5f * height));

    let mounting_position = local_position.xzy - vec3<f32>(0.5f * width - 6.0f,
                                                            0.0f,
                                                            0.5f * depth - 18.0f);
    let mounting_hole = length(mounting_position.xz) - 4.2f;
    let lowering = signed_cylinder(mounting_position, 7.0f, 0.5f * height);
    let lowering_box = signed_box(mounting_position - vec3<f32>(7.0f, 0.0f, 0.0f),
                                  vec3<f32>(7.0f, 0.5f * height, 7.0f));
    platform = max(platform, -mounting_hole);
    platform = max(platform, -lowering);
    platform = max(platform, -lowering_box);
    return platform;
}

fn field_overlay(position: vec3<f32>, slice_height: f32) -> f32 {
    let minimum = vec3<f32>(frame.clipping_box_min.x,
                            frame.clipping_box_min.y,
                            slice_height - 1.0f);
    let maximum = vec3<f32>(frame.clipping_box_max.x,
                            frame.clipping_box_max.y,
                            slice_height);
    return bounding_box_distance(position, minimum, maximum);
}

fn stack_overlay(position: vec3<f32>, slice_height: f32) -> f32 {
    let stack_height = 2.5f;
    let stack_position = position.z - round(position.z / stack_height) * stack_height;
    let minimum = vec3<f32>(frame.clipping_box_min.x, frame.clipping_box_min.y, 0.0f);
    let maximum = vec3<f32>(frame.clipping_box_max.x, frame.clipping_box_max.y, 0.5f);
    let stack_distance = bounding_box_distance(
      vec3<f32>(position.x, position.y, stack_position), minimum, maximum);
    return max(clipping_box_distance(position, slice_height), stack_distance);
}

fn evaluate_surface(position: vec3<f32>) -> SceneSample {
    let model =  evaluateModel(position);
    var sample = SceneSample(model.w, model.xyz, 1.0f);
    let flags = frame.flags_mode_reserved.x;
    let slice_height = frame.time_slice_quality_normal.y;
    if ((flags & RF_CUT_OFF_OBJECT) != 0u) {
        var clipping_distance = position.z - slice_height;
        if (has_model_bounds()) {
            clipping_distance = clipping_box_distance(position, slice_height);
        }
        sample.distance = max(sample.distance, clipping_distance);
    }

    if ((flags & RF_SHOW_BUILDPLATE) != 0u) {
        let platform_distance = build_platform(position);
        if (platform_distance < sample.distance) {
            sample.distance = platform_distance;
            sample.color = vec3<f32>(0.1f);
            sample.object_type = 0.0f;
        }
    }

    if ((flags & RF_SHOW_FIELD) != 0u && slice_height > 0.0001f && has_model_bounds()) {
        let field_distance = field_overlay(position, slice_height);
        if (field_distance < sample.distance) {
            sample.distance = field_distance;
            sample.color = vec3<f32>(
                select(0.0f, 0.5f + 0.5f * fract(abs(model.w)), model.w < 0.0f),
                fract(abs(model.w) * 0.01f),
                fract(abs(model.w) * 0.1f));
            if (abs(model.w) < 0.05f) {
                sample.color = sample.color + vec3<f32>(abs(0.05f - model.w) * 10.0f);
            }
            sample.object_type = 3.0f;
        }
    }

    if ((flags & RF_SHOW_STACK) != 0u && slice_height > 0.0001f && has_model_bounds()) {
        let stack_distance = stack_overlay(position, slice_height);
        if (stack_distance < sample.distance) {
            sample.distance = stack_distance;
            sample.color = vec3<f32>(0.5f + model.w * 0.05f);
            sample.object_type = 3.0f;
        }
    }

    if ((flags & RF_SHOW_COORDINATE_SYSTEM) != 0u) {
        let axis_radius = 0.1f;
        let x_axis = signed_cylinder_from_to(position, vec3<f32>(0.0), vec3<f32>(400.0f, 0.0, 0.0), axis_radius);
        let y_axis = signed_cylinder_from_to(position, vec3<f32>(0.0), vec3<f32>(0.0, 400.0f, 0.0), axis_radius);
        let z_axis = signed_cylinder_from_to(position, vec3<f32>(0.0), vec3<f32>(0.0, 0.0, 400.0f), axis_radius);
        if (x_axis < sample.distance) {
            sample.distance = x_axis;
            sample.color = vec3<f32>(1.0, 0.0, 0.0);
            sample.object_type = 0.0f;
        }
        if (y_axis < sample.distance) {
            sample.distance = y_axis;
            sample.color = vec3<f32>(0.0, 1.0, 0.0);
            sample.object_type = 0.0f;
        }
        if (z_axis < sample.distance) {
            sample.distance = z_axis;
            sample.color = vec3<f32>(0.0, 0.0, 1.0);
            sample.object_type = 0.0f;
        }
    }

    return sample;
}

fn estimate_normal(position: vec3<f32>) -> vec3<f32> {
    let epsilon = 0.0001f;
    var normal = vec3<f32>(0.0);
    for (var sample = 0u; sample < 4u; sample++) {
        let bits = vec3<u32>((sample + 3u) >> 1u, sample >> 1u, sample);
        let offset = 0.5773f * (2.0f * vec3<f32>(vec3<u32>(bits & vec3<u32>(1u))) - vec3<f32>(1.0));
        normal = normal + offset * evaluate_surface(position + offset * epsilon).distance;
    }
    return normalize(normal);
}

fn calc_soft_shadow(position: vec3<f32>, direction: vec3<f32>) -> f32 {
    var maximum_distance = 25.0f;
    if (abs(direction.y) > 0.000001f) {
        let plane_distance = (300.0f - position.y) / direction.y;
        if (plane_distance > 0.0f) {
            maximum_distance = min(maximum_distance, plane_distance);
        }
    }

    var result = 20.0f;
    var distance = 0.002f;
    for (var step = 0u; step < 32u; step++) {
        let sample_position = position + direction * distance;
        let sample_distance = evaluate_surface(sample_position).distance;
        result = min(result, 8.0f * sample_distance / distance);
        distance = distance + clamp(sample_distance, 0.5f, 1.0f);
        if (result < 0.005f || distance > maximum_distance) {
            break;
        }
    }
    return clamp(result, 0.0f, 1.0f);
}

fn calc_ambient_occlusion(position: vec3<f32>, normal: vec3<f32>) -> f32 {
    var occlusion = 0.0f;
    var scale = 1.0f;
    for (var step = 0u; step < 8u; step++) {
        let distance = 0.01f + 0.03f * f32(step);
        let sample_distance = evaluate_surface(position + normal * distance).distance;
        occlusion = occlusion - (sample_distance - distance) * scale;
        scale = scale * 0.95f;
    }
    return clamp(1.0f - 3.0f * occlusion, 0.0f, 1.0f) * (0.5f + 0.5f * normal.y);
}

fn reflect_vector(in_vector: vec3<f32>, normal: vec3<f32>) -> vec3<f32> {
    return in_vector - 2.0f * dot(normal, in_vector) * normal;
}

fn shade_surface(position: vec3<f32>, ray_direction: vec3<f32>, base_color: vec3<f32>) -> vec3<f32> {
    let flags = frame.flags_mode_reserved.x;
    let normal = estimate_normal(position);
    let grid_distance = 5.0f;
    let grid_line_width = vec2<f32>(0.2f);
    let grid_remainder = position.xy - grid_distance * trunc(position.xy / grid_distance);
    let grid = grid_line_width - clamp(grid_remainder - grid_line_width, vec2<f32>(0.0), vec2<f32>(1.0));
    let grid_highlight = smoothstep(0.1f, 1.0f, max(grid.x, grid.y));
    var color = base_color + vec3<f32>(grid_highlight);

    let reflected = reflect_vector(ray_direction, normal);
    let occlusion = select(0.5f + 0.5f * normal.y,
                           calc_ambient_occlusion(position, normal),
                           (flags & RF_DISABLE_AO) == 0u);
    let light_direction = normalize(vec3<f32>(-0.4f, -0.7f, 1.0f));
    let half_direction = normalize(light_direction - ray_direction);
    let ambient = clamp(0.7f - 0.3f * normal.y, 0.0f, 1.0f);
    var diffuse = clamp(dot(normal, light_direction), 0.0f, 1.0f);
    let back = clamp(dot(normal, normalize(vec3<f32>(-light_direction.x, 0.0f, -light_direction.z))), 0.0f, 1.0f) *
               clamp(1.0f - position.y, 0.0f, 1.0f);
    var dominant = smoothstep(-0.2f, 0.2f, reflected.y);
    let fresnel = pow(clamp(1.0f + dot(normal, ray_direction), 0.0f, 1.0f), 2.0f);

    let shadows_enabled = (flags & RF_DISABLE_SHADOWS) == 0u &&
                          frame.flags_mode_reserved.y != 2u;
    if (shadows_enabled) {
        diffuse = diffuse * calc_soft_shadow(position, light_direction);
        dominant = dominant * calc_soft_shadow(position, reflected);
    }

    let specular = pow(clamp(dot(normal, half_direction), 0.0f, 1.0f), 16.0f) * diffuse *
                   (0.08f + 0.92f * pow(clamp(1.0f + dot(half_direction, ray_direction), 0.0f, 1.0f), 5.0f));
    var light = vec3<f32>(0.0);
    light = light + 1.30f * diffuse * vec3<f32>(1.0);
    light = light + 0.80f * ambient * vec3<f32>(0.40f, 0.60f, 1.00f) * occlusion;
    light = light + 0.40f * dominant * vec3<f32>(0.40f, 0.60f, 1.00f) * occlusion;
    light = light + 0.50f * back * vec3<f32>(0.25f) * occlusion;
    light = light + 1.25f * fresnel * vec3<f32>(1.0) * occlusion;
    color = color * light;
    color = color + 9.0f * specular * vec3<f32>(1.0, 0.90f, 0.70f);
    return color;
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
        frame.up_and_height.xyz * (screen.y * frame.vertical_scale_and_max_steps.x));
    let eye = frame.eye_and_max_distance.xyz;
    let max_distance = frame.eye_and_max_distance.w;

    let initial_close_enough = select(0.001f, 0.01f, frame.flags_mode_reserved.y == 2u);
    let min_step_size = initial_close_enough * 0.01f;
    var traveled = initial_close_enough;
    var previous_signed_distance = 3.402823466e+38f;
    var previous_absolute_distance = 3.402823466e+38f;
    var previous_delta_distance = 0.0f;
    var previous_step_size = 0.0f;
    var next_step = min_step_size;
    var near_range_factor = 1.0f;
    var omega = 1.6f;
    var consecutive_small_steps = 0u;
    var did_backtrack = false;
    var hit = false;
    var model = SceneSample(0.0f, vec3<f32>(0.0f), 1.0f);
    let max_ray_steps = u32(frame.vertical_scale_and_max_steps.y);
    for (var step = 0u; step < max_ray_steps; step++) {
        var position = eye + ray_direction * traveled;
        model = evaluate_surface(position);

        var signed_distance = model.distance;
        var absolute_distance = abs(signed_distance);
        let close_enough = initial_close_enough + traveled * 1.0e-6f;
        let adaptive_enabled = (frame.flags_mode_reserved.x & RF_DISABLE_ADAPTIVE_OMEGA) == 0u;

        let should_backtrack = adaptive_enabled && step > 0u && !did_backtrack &&
                               previous_absolute_distance + absolute_distance < previous_step_size;
        if (should_backtrack) {
            traveled = traveled - previous_step_size;
            let conservative_step = max(previous_absolute_distance, min_step_size);
            traveled = traveled + conservative_step;
            previous_step_size = conservative_step;
            did_backtrack = true;
            position = eye + ray_direction * traveled;
            model = evaluate_surface(position);
            signed_distance = model.distance;
            absolute_distance = abs(signed_distance);
            omega = 1.0f;
        } else {
            did_backtrack = false;
            if (!adaptive_enabled) {
                omega = 1.0f;
            } else if (absolute_distance > close_enough * 10.0f) {
                omega = 1.6f;
            } else {
                omega = 1.0f;
            }
        }

        if (adaptive_enabled && absolute_distance < close_enough * 10.0f) {
            consecutive_small_steps = consecutive_small_steps + 1u;
            if (consecutive_small_steps >= 5u) {
                omega = 1.0f;
            }
        } else {
            consecutive_small_steps = 0u;
        }

        let distance_sign_changed = step > 0u &&
                                    ((previous_signed_distance < 0.0f) != (signed_distance < 0.0f));
        let delta_distance = select(0.0f, signed_distance - previous_signed_distance, step > 0u);
        let slope_sign_changed = step > 1u && previous_delta_distance != 0.0f &&
                                 ((delta_distance < 0.0f) != (previous_delta_distance < 0.0f));
        let close_to_surface = absolute_distance < next_step;

        if (distance_sign_changed || slope_sign_changed || close_to_surface) {
            var last_good_distance = traveled - abs(previous_signed_distance);
            var bad_distance = traveled;
            for (var refinement = 0u; refinement < 6u; refinement++) {
                let middle_distance = (last_good_distance + bad_distance) * 0.5f;
                let middle_sample = evaluate_surface(eye + ray_direction * middle_distance);
                if ((previous_signed_distance < 0.0f) != (middle_sample.distance < 0.0f)) {
                    bad_distance = middle_distance;
                } else {
                    last_good_distance = middle_distance;
                }
            }
            traveled = last_good_distance;
            position = eye + ray_direction * traveled;
            model = evaluate_surface(position);
            signed_distance = model.distance;
            absolute_distance = abs(signed_distance);
            near_range_factor = 0.1f;
            omega = 1.0f;
            next_step = max(absolute_distance * near_range_factor * omega, min_step_size);
        } else {
            near_range_factor = min(near_range_factor * 1.05f, 1.0f);
            next_step = max(absolute_distance * near_range_factor * omega, min_step_size);
        }

        previous_signed_distance = signed_distance;
        previous_absolute_distance = absolute_distance;
        previous_delta_distance = delta_distance;
        previous_step_size = next_step;
        traveled = traveled + next_step;

        if (absolute_distance < close_enough) {
            traveled = traveled - signed_distance;
            hit = true;
            break;
        }
        if (traveled > max_distance) {
            break;
        }
    }

    let pixel_index = global_id.y * width + global_id.x;
    if (!hit) {
        output_pixels[pixel_index] = pack_rgba8(vec4<f32>(vec3<f32>(0.1), 1.0));
        return;
    }

    let hit_position = eye + ray_direction * traveled;
    let hit_sample = evaluate_surface(hit_position);
    let use_model_color = hit_sample.object_type == 1.0f && frame.flags_mode_reserved.y != 2u;
    let hit_color = select(hit_sample.color, evaluateModel(hit_position).xyz, use_model_color);
    let shaded = shade_surface(hit_position, ray_direction, hit_color);
    let background = vec3<f32>(0.1f);
    let fog = 1.0f - exp(-1.0e-10f * traveled * traveled * traveled);
    output_pixels[pixel_index] = pack_rgba8(vec4<f32>(mix(shaded, background, fog), 1.0));
}