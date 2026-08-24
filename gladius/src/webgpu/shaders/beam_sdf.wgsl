// GLADIUS_BEAM_SDF_MODULE
//
// WebGPU (WGSL) port of the beam lattice evaluation in src/kernel/sdf.cl
// (evaluateBeamLatticeBVH + sdToBeam) — pure-BVH signed distance to a beam
// lattice with tapered beams (hemisphere/sphere/butt caps) and vertex balls.
//
// Data layout matches BeamPayloadSerializer:
//   Header (8 floats):
//     [0] bvhNodesOffset  [1] primitiveIndicesOffset
//     [2] beamsOffset     [3] ballsOffset
//     [4] bvhNodeCount    [5] beamCount
//     [6] ballCount       [7] reserved
//   BVH node: 10 floats — bbMin.xyz(3), bbMax.xyz(3), leftChild, rightChild,
//             primStart, primCount (ints stored as plain float values)
//   Primitive index entry: 3 floats (type 0=beam/1=ball, index, unused)
//   Beam: 11 floats — startPos.xyz, endPos.xyz, startRadius, endRadius,
//         startCapStyle, endCapStyle, materialId
//   Ball: 4 floats — position.xyz, radius

const BEAM_INVALID_DISTANCE: f32 = 3.402823466e+38f;

@group(0) @binding(6)
var<storage, read> beam_payload_data: array<f32>;

@group(0) @binding(7)
var<storage, read> beam_offset_table: array<vec2<u32>>;

fn beam_resource_start(resource_id: u32) -> u32 {
    if (resource_id >= arrayLength(&beam_offset_table)) {
        return 0u;
    }
    return beam_offset_table[resource_id].x;
}

fn beam_resource_count(resource_id: u32) -> u32 {
    if (resource_id >= arrayLength(&beam_offset_table)) {
        return 0u;
    }
    return beam_offset_table[resource_id].y;
}

fn beam_load_f32(payload_base: u32, index: u32) -> f32 {
    return beam_payload_data[payload_base + index];
}

fn beam_load_vec3(payload_base: u32, index: u32) -> vec3<f32> {
    return vec3<f32>(beam_load_f32(payload_base, index),
                     beam_load_f32(payload_base, index + 1u),
                     beam_load_f32(payload_base, index + 2u));
}

/// Signed distance to a single tapered beam with cap styles.
/// Cap styles: 0 = hemisphere, 1 = sphere, 2 = butt.
fn beam_sd_to_beam(position: vec3<f32>,
                   start_pos: vec3<f32>,
                   end_pos: vec3<f32>,
                   start_radius: f32,
                   end_radius: f32,
                   start_cap_style: i32,
                   end_cap_style: i32) -> f32 {
    let axis_raw = end_pos - start_pos;
    let length_sq = dot(axis_raw, axis_raw);
    let length_val = sqrt(length_sq);

    // Degenerate beam: treat as sphere.
    if (length_val < 1.0e-6) {
        let radius = max(start_radius, end_radius);
        return length(position - start_pos) - radius;
    }

    let inv_length = 1.0 / length_val;
    let axis = axis_raw * inv_length;

    let to_point = position - start_pos;
    let t_unclamped = dot(to_point, axis);
    let t = clamp(t_unclamped, 0.0, length_val);

    // Radius interpolated at the projection point.
    let alpha = t * inv_length;
    let radius = mix(start_radius, end_radius, alpha);

    let projection = start_pos + axis * t;
    let dist_to_axis = length(position - projection);
    let surface_dist = dist_to_axis - radius;

    if (t_unclamped <= 0.0) {
        let sphere_dist = length(position - start_pos) - start_radius;
        let butt_dist = max(surface_dist, -t_unclamped);
        return select(sphere_dist, butt_dist, start_cap_style == 2i);
    }

    if (t_unclamped >= length_val) {
        let sphere_dist = length(position - end_pos) - end_radius;
        let overrun = t_unclamped - length_val;
        let butt_dist = max(surface_dist, overrun);
        return select(sphere_dist, butt_dist, end_cap_style == 2i);
    }

    return surface_dist;
}

const BEAM_BVH_STACK_SIZE: u32 = 32u;

/// Core BVH traversal over the beam lattice payload; returns min signed distance.
fn gladius_beam_sdf_core(payload_base: u32, position: vec3<f32>) -> f32 {
    let node_count = u32(beam_load_f32(payload_base, 4u));
    if (node_count == 0u) {
        return BEAM_INVALID_DISTANCE;
    }

    let bvh_offset = u32(beam_load_f32(payload_base, 0u));
    let indices_offset = u32(beam_load_f32(payload_base, 1u));
    let beams_offset = u32(beam_load_f32(payload_base, 2u));
    let balls_offset = u32(beam_load_f32(payload_base, 3u));
    let beam_count = u32(beam_load_f32(payload_base, 5u));
    let ball_count = u32(beam_load_f32(payload_base, 6u));

    var stack: array<u32, BEAM_BVH_STACK_SIZE>;
    var stack_ptr = 0u;
    stack[stack_ptr] = 0u;
    stack_ptr = stack_ptr + 1u;

    var min_dist = BEAM_INVALID_DISTANCE;

    loop {
        if (stack_ptr == 0u) {
            break;
        }
        stack_ptr = stack_ptr - 1u;
        let node_index = stack[stack_ptr];

        if (node_index >= node_count) {
            continue;
        }

        let node_base = bvh_offset + node_index * 10u;
        let bb_min = beam_load_vec3(payload_base, node_base);
        let bb_max = beam_load_vec3(payload_base, node_base + 3u);

        // Distance to AABB (matches OpenCL bbBox helper).
        let d = max(bb_min - position, max(position - bb_max, vec3<f32>(0.0)));
        let bb_dist = length(d);
        if (bb_dist > min_dist) {
            continue;
        }

        let left_child = i32(beam_load_f32(payload_base, node_base + 6u));
        let right_child = i32(beam_load_f32(payload_base, node_base + 7u));
        let prim_start = i32(beam_load_f32(payload_base, node_base + 8u));
        let prim_count = i32(beam_load_f32(payload_base, node_base + 9u));

        let is_leaf = (left_child == -1i && right_child == -1i);
        if (is_leaf) {
            var i = 0u;
            loop {
                if (i >= u32(prim_count)) {
                    break;
                }
                let entry_base = indices_offset + (u32(prim_start) + i) * 3u;
                let primitive_type = i32(beam_load_f32(payload_base, entry_base));
                let primitive_index = u32(beam_load_f32(payload_base, entry_base + 1u));

                var dist = BEAM_INVALID_DISTANCE;
                if (primitive_type == 0i && primitive_index < beam_count) {
                    let beam_base = beams_offset + primitive_index * 11u;
                    let sd = beam_sd_to_beam(position,
                                             beam_load_vec3(payload_base, beam_base),
                                             beam_load_vec3(payload_base, beam_base + 3u),
                                             beam_load_f32(payload_base, beam_base + 6u),
                                             beam_load_f32(payload_base, beam_base + 7u),
                                             i32(beam_load_f32(payload_base, beam_base + 8u)),
                                             i32(beam_load_f32(payload_base, beam_base + 9u)));
                    dist = sd;
                } else if (primitive_type == 1i && primitive_index < ball_count) {
                    let ball_base = balls_offset + primitive_index * 4u;
                    let ball_pos = beam_load_vec3(payload_base, ball_base);
                    let ball_radius = beam_load_f32(payload_base, ball_base + 3u);
                    dist = length(position - ball_pos) - ball_radius;
                }

                min_dist = min(min_dist, dist);
                i = i + 1u;
            }
        } else {
            // Push right first so left is processed first (depth-first).
            if (right_child >= 0i && u32(right_child) < node_count &&
                stack_ptr < BEAM_BVH_STACK_SIZE - 1u) {
                stack[stack_ptr] = u32(right_child);
                stack_ptr = stack_ptr + 1u;
            }
            if (left_child >= 0i && u32(left_child) < node_count &&
                stack_ptr < BEAM_BVH_STACK_SIZE) {
                stack[stack_ptr] = u32(left_child);
                stack_ptr = stack_ptr + 1u;
            }
        }
    }

    return min_dist;
}

/// Signed distance to the beam lattice resource with the given resource id.
/// Returns a large positive value when the resource is unavailable or empty.
fn gladiusSignedDistanceToBeamLattice(position: vec3<f32>, resource_id: u32) -> f32 {
    let start = beam_resource_start(resource_id);
    let count = beam_resource_count(resource_id);
    if (count < 8u) {
        return BEAM_INVALID_DISTANCE;
    }
    return gladius_beam_sdf_core(start, position);
}
