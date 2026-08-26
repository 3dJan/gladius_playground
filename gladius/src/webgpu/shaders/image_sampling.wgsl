// GLADIUS_IMAGE_SAMPLING_MODULE
//
// Manual image-stack sampling matching src/kernel/sdf.cl. Payloads are indexed
// by resource ID and use the layout from ImagePayloadSerializer.h.

@group(0) @binding(8)
var<storage, read> image_payload_data: array<f32>;

@group(0) @binding(9)
var<storage, read> image_offset_table: array<vec2<u32>>;

fn image_resource_start(resource_id: u32) -> u32 {
    if (resource_id >= arrayLength(&image_offset_table)) {
        return 0u;
    }
    return image_offset_table[resource_id].x;
}

fn image_resource_count(resource_id: u32) -> u32 {
    if (resource_id >= arrayLength(&image_offset_table)) {
        return 0u;
    }
    return image_offset_table[resource_id].y;
}

fn image_wrap(value: f32) -> f32 {
    return fract(value);
}

fn image_mirror_repeated(value: f32) -> f32 {
    let adjusted = abs(value + select(0.0f, 1.0f, value < 0.0f));
    let repeated = adjusted - 2.0f * trunc(adjusted / 2.0f);
    return select(repeated, 1.0f - repeated, repeated >= 1.0f);
}

fn image_apply_tile_style(value: f32, tile_style: u32) -> f32 {
    if (tile_style == 0u) {
        return image_wrap(value);
    }
    if (tile_style == 1u) {
        return image_mirror_repeated(value);
    }
    if (tile_style == 2u) {
        return clamp(value, 0.0f, 1.0f);
    }
    return value;
}

fn image_apply_tile_styles(position: vec3<f32>, tile_styles: vec3<u32>) -> vec3<f32> {
    return vec3<f32>(image_apply_tile_style(position.x, tile_styles.x),
                     image_apply_tile_style(position.y, tile_styles.y),
                     image_apply_tile_style(position.z, tile_styles.z));
}

fn image_get_value(payload_base: u32,
                   payload_count: u32,
                   position: vec3<i32>,
                   dimensions: vec3<i32>) -> vec4<f32> {
    let voxel_index = position.x + position.y * dimensions.x +
                      position.z * dimensions.x * dimensions.y;
    let unclamped_index = 4i + voxel_index * 4i;
    let max_index = max(4i, i32(payload_count) - 4i);
    let index = u32(clamp(unclamped_index, 4i, max_index));
    return vec4<f32>(image_payload_data[payload_base + index],
                     image_payload_data[payload_base + index + 1u],
                     image_payload_data[payload_base + index + 2u],
                     image_payload_data[payload_base + index + 3u]);
}

fn image_sample_nearest(payload_base: u32,
                        payload_count: u32,
                        uvw: vec3<f32>,
                        dimensions: vec3<i32>,
                        tile_styles: vec3<u32>) -> vec4<f32> {
    let mapped = image_apply_tile_styles(uvw, tile_styles);
    let texel = vec3<i32>(mapped * vec3<f32>(dimensions));
    return image_get_value(payload_base, payload_count, texel, dimensions);
}

fn image_sample_linear(payload_base: u32,
                       payload_count: u32,
                       uvw: vec3<f32>,
                       dimensions: vec3<i32>,
                       tile_styles: vec3<u32>) -> vec4<f32> {
    let mapped = image_apply_tile_styles(uvw, tile_styles);
    let texel = mapped * vec3<f32>(dimensions);
    let coordinate = vec3<i32>(floor(texel));
    let relative = texel - floor(texel);

    let c000 = image_get_value(payload_base, payload_count, coordinate, dimensions);
    let c100 = image_get_value(payload_base, payload_count, coordinate + vec3<i32>(1, 0, 0), dimensions);
    let c010 = image_get_value(payload_base, payload_count, coordinate + vec3<i32>(0, 1, 0), dimensions);
    let c110 = image_get_value(payload_base, payload_count, coordinate + vec3<i32>(1, 1, 0), dimensions);
    let c001 = image_get_value(payload_base, payload_count, coordinate + vec3<i32>(0, 0, 1), dimensions);
    let c101 = image_get_value(payload_base, payload_count, coordinate + vec3<i32>(1, 0, 1), dimensions);
    let c011 = image_get_value(payload_base, payload_count, coordinate + vec3<i32>(0, 1, 1), dimensions);
    let c111 = image_get_value(payload_base, payload_count, coordinate + vec3<i32>(1, 1, 1), dimensions);

    let c00 = mix(c000, c100, relative.x);
    let c01 = mix(c001, c101, relative.x);
    let c10 = mix(c010, c110, relative.x);
    let c11 = mix(c011, c111, relative.x);
    let c0 = mix(c00, c10, relative.y);
    let c1 = mix(c01, c11, relative.y);
    return mix(c0, c1, relative.z);
}

fn gladiusSampleImage(uvw: vec3<f32>,
                      resource_id: u32,
                      tile_styles: vec3<u32>,
                      sampling_filter: u32) -> vec4<f32> {
    let payload_base = image_resource_start(resource_id);
    let payload_count = image_resource_count(resource_id);
    if (payload_count < 8u) {
        return vec4<f32>(0.0f);
    }

    let dimensions = vec3<i32>(i32(image_payload_data[payload_base]),
                                i32(image_payload_data[payload_base + 1u]),
                                i32(image_payload_data[payload_base + 2u]));
    if (any(dimensions <= vec3<i32>(0))) {
        return vec4<f32>(0.0f);
    }

    if (sampling_filter == 0u) {
        return image_sample_nearest(payload_base, payload_count, uvw, dimensions, tile_styles);
    }
    return image_sample_linear(payload_base, payload_count, uvw, dimensions, tile_styles);
}
