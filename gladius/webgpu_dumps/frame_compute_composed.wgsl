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


//
// WebGPU (WGSL) port of src/kernel/mesh_sdf.cl — pure-BVH signed distance to
// triangle meshes with pseudo-normal sign determination (Bærentzen & Aanæs 2005).
//
// Data layout matches SpatialMeshResource / MeshPayloadSerializer:
//   Header (34 floats):
//     [0-7]   bbox min.xyzw, max.xyzw
//     [8-11]  nodeCount, triCount, vertexNormalCount, reserved
//     [12-15] nodesOffset, trianglesOffset, normalsOffset, indicesOffset
//     [16-25] voxel grid header (unused in pure-BVH mode)
//     [26-27] voxelDataOffset, voxelCount (0 = pure BVH)
//     [28]    edgeNeighborsOffset
//     [29-33] FWN/sign-cache/NanoVDB slots (0 = not available)
//   BVH node: 12 floats — bboxMin(4), bboxMax(4), leftChild, rightChild,
//             primStart, primCount (ints stored as float bit patterns)
//   Triangle: 16 floats — v0(4), v1(4), v2(4), faceNormal(4)
//   Vertex normal: 4 floats
//   Triangle indices: 4 floats per triangle (i0, i1, i2, padding)
//   Edge neighbors: 3 x 4 floats per triangle

const MESH_PAYLOAD_HEADER_FLOATS: u32 = 34u;
const MESH_INVALID_DISTANCE: f32 = 3.402823466e+38f;

// Flat payload storage: all mesh payloads concatenated. Per-resource start
// offsets (in floats) and total float counts live in the offset table below.
// WGSL forbids runtime-sized arrays nested inside array elements, so a flat
// buffer plus an offset table is used instead of an array of structs.
@group(0) @binding(4)
var<storage, read> mesh_payload_data: array<f32>;

@group(0) @binding(5)
var<storage, read> mesh_offset_table: array<vec2<u32>>;

fn mesh_resource_start(resource_id: u32) -> u32 {
    if (resource_id >= arrayLength(&mesh_offset_table)) {
        return 0u;
    }
    return mesh_offset_table[resource_id].x;
}

fn mesh_resource_count(resource_id: u32) -> u32 {
    if (resource_id >= arrayLength(&mesh_offset_table)) {
        return 0u;
    }
    return mesh_offset_table[resource_id].y;
}

fn mesh_load_f32(payload_base: u32, index: u32) -> f32 {
    return mesh_payload_data[payload_base + index];
}

fn mesh_load_vec3(payload_base: u32, index: u32) -> vec3<f32> {
    return vec3<f32>(mesh_load_f32(payload_base, index),
                     mesh_load_f32(payload_base, index + 1u),
                     mesh_load_f32(payload_base, index + 2u));
}

fn mesh_load_int(payload_base: u32, index: u32) -> i32 {
    return bitcast<i32>(mesh_load_f32(payload_base, index));
}

/// Read a header slot that stores a non-negative integer as a plain float
/// value (serializer writes static_cast<float>(value), not a bit pattern).
fn mesh_load_header_uint(payload_base: u32, index: u32) -> u32 {
    return u32(mesh_load_f32(payload_base, index));
}

struct MeshBvhNode {
    bbox_min: vec3<f32>,
    bbox_max: vec3<f32>,
    left_child: i32,
    right_child: i32,
    prim_start: i32,
    prim_count: i32,
};

fn mesh_load_node(payload_base: u32, base: u32, node_index: u32) -> MeshBvhNode {
    let offset = base + node_index * 12u;
    var node: MeshBvhNode;
    node.bbox_min = mesh_load_vec3(payload_base, offset);
    node.bbox_max = mesh_load_vec3(payload_base, offset + 4u);
    node.left_child = mesh_load_int(payload_base, offset + 8u);
    node.right_child = mesh_load_int(payload_base, offset + 9u);
    node.prim_start = mesh_load_int(payload_base, offset + 10u);
    node.prim_count = mesh_load_int(payload_base, offset + 11u);
    return node;
}

struct MeshTriangle {
    v0: vec3<f32>,
    v1: vec3<f32>,
    v2: vec3<f32>,
    face_normal: vec3<f32>,
};

fn mesh_load_triangle(payload_base: u32, base: u32, tri_index: u32) -> MeshTriangle {
    let offset = base + tri_index * 16u;
    var tri: MeshTriangle;
    tri.v0 = mesh_load_vec3(payload_base, offset);
    tri.v1 = mesh_load_vec3(payload_base, offset + 4u);
    tri.v2 = mesh_load_vec3(payload_base, offset + 8u);
    tri.face_normal = mesh_load_vec3(payload_base, offset + 12u);
    return tri;
}

fn mesh_sq_distance_to_aabb(position: vec3<f32>, minimum: vec3<f32>, maximum: vec3<f32>) -> f32 {
    let d = max(minimum - position, max(position - maximum, vec3<f32>(0.0)));
    return dot(d, d);
}

/// Squared distance from point to triangle (Ericson, Real-Time Collision Detection).
fn mesh_sq_triangle_fast(position: vec3<f32>, v0: vec3<f32>, v1: vec3<f32>, v2: vec3<f32>) -> f32 {
    let ab = v1 - v0;
    let ac = v2 - v0;
    let ap = position - v0;

    let d1 = dot(ab, ap);
    let d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return dot(position - v0, position - v0);
    }

    let bp = position - v1;
    let d3 = dot(ab, bp);
    let d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return dot(bp, bp);
    }

    let vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        let v = d1 / (d1 - d3);
        return dot(position - (v0 + v * ab), position - (v0 + v * ab));
    }

    let cp = position - v2;
    let d5 = dot(ab, cp);
    let d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return dot(cp, cp);
    }

    let vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        let w = d2 / (d2 - d6);
        return dot(position - (v0 + w * ac), position - (v0 + w * ac));
    }

    let va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        let w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return dot(position - (v1 + w * (v2 - v1)), position - (v1 + w * (v2 - v1)));
    }

    // Inside the triangle: distance to the plane.
    let denom = 1.0 / (va + vb + vc);
    let v = vb * denom;
    let w = vc * denom;
    let nearest = v0 + ab * v + ac * w;
    return dot(position - nearest, position - nearest);
}

/// Closest point on triangle with feature classification (for sign computation).
/// Returns squared distance; outputs closest point and feature info via inout struct.
struct MeshClosestPointResult {
    sq_distance: f32,
    closest_point: vec3<f32>,
    feature_type: i32,   // 0 = face, 1 = edge, 2 = vertex
    bary_u: f32,
    bary_v: f32,
    edge_index: i32,
    vertex_index: i32,
};

fn mesh_sq_triangle_with_closest_point(position: vec3<f32>,
                                       v0: vec3<f32>,
                                       v1: vec3<f32>,
                                       v2: vec3<f32>,
                                       result: ptr<function, MeshClosestPointResult>) -> f32 {
    let epsilon = 1.0e-10;

    let e0 = v1 - v0;
    let e1 = v2 - v0;
    let v = v0 - position;

    let a = dot(e0, e0);
    let b = dot(e0, e1);
    let c = dot(e1, e1);
    let d = dot(e0, v);
    let e = dot(e1, v);

    let det = a * c - b * b;
    var s = b * e - c * d;
    var t = b * d - a * e;

    if (det < epsilon && a < epsilon && c < epsilon) {
        (*result).sq_distance = dot(v, v);
        (*result).closest_point = v0;
        (*result).feature_type = 2;
        (*result).bary_u = 0.0;
        (*result).bary_v = 0.0;
        (*result).edge_index = -1;
        (*result).vertex_index = 0;
        return (*result).sq_distance;
    }

    var feature_type = 0i;
    var edge_index = -1i;
    var vertex_index = -1i;

    if (s + t <= det) {
        if (s < 0.0) {
            if (t < 0.0) {
                // Region 4: closest to v0.
                if (d < 0.0) {
                    t = 0.0;
                    s = clamp(-d / a, 0.0, 1.0);
                    feature_type = select(1i, 2i, s == 0.0 || s == 1.0);
                    edge_index = 0i;
                    vertex_index = select(1i, 0i, s == 0.0);
                } else {
                    s = 0.0;
                    t = clamp(-e / c, 0.0, 1.0);
                    feature_type = select(1i, 2i, t == 0.0 || t == 1.0);
                    edge_index = 2i;
                    vertex_index = select(2i, 0i, t == 0.0);
                }
            } else {
                // Region 3: edge v0-v2.
                s = 0.0;
                t = clamp(-e / c, 0.0, 1.0);
                feature_type = select(1i, 2i, t == 0.0 || t == 1.0);
                edge_index = 2i;
                vertex_index = select(2i, 0i, t == 0.0);
            }
        } else if (t < 0.0) {
            // Region 5: edge v0-v1.
            t = 0.0;
            s = clamp(-d / a, 0.0, 1.0);
            feature_type = select(1i, 2i, s == 0.0 || s == 1.0);
            edge_index = 0i;
            vertex_index = select(1i, 0i, s == 0.0);
        } else {
            // Region 0: face interior.
            let inv_det = select(0.0, 1.0 / det, det > epsilon);
            s = s * inv_det;
            t = t * inv_det;
            feature_type = 0i;
        }
    } else {
        if (s < 0.0) {
            // Region 2.
            let tmp0 = b + d;
            let tmp1 = c + e;
            if (tmp1 > tmp0) {
                let numer = tmp1 - tmp0;
                let denom = a - 2.0 * b + c;
                s = clamp(numer / denom, 0.0, 1.0);
                t = 1.0 - s;
                feature_type = select(1i, 2i, s == 0.0 || s == 1.0);
                edge_index = 1i;
                vertex_index = select(1i, 2i, s == 0.0);
            } else {
                s = 0.0;
                t = clamp(-e / c, 0.0, 1.0);
                feature_type = select(1i, 2i, t == 0.0 || t == 1.0);
                edge_index = 2i;
                vertex_index = select(2i, 0i, t == 0.0);
            }
        } else if (t < 0.0) {
            // Region 6.
            let tmp0 = b + e;
            let tmp1 = a + d;
            if (tmp1 > tmp0) {
                let numer = tmp1 - tmp0;
                let denom = a - 2.0 * b + c;
                t = clamp(numer / denom, 0.0, 1.0);
                s = 1.0 - t;
                feature_type = select(1i, 2i, t == 0.0 || t == 1.0);
                edge_index = 1i;
                vertex_index = select(2i, 1i, t == 0.0);
            } else {
                t = 0.0;
                s = clamp(-d / a, 0.0, 1.0);
                feature_type = select(1i, 2i, s == 0.0 || s == 1.0);
                edge_index = 0i;
                vertex_index = select(1i, 0i, s == 0.0);
            }
        } else {
            // Region 1: edge v1-v2.
            let numer = (c + e) - (b + d);
            if (numer <= 0.0) {
                s = 0.0;
                t = 1.0;
                feature_type = 2i;
                vertex_index = 2i;
            } else {
                let denom = a - 2.0 * b + c;
                s = clamp(numer / denom, 0.0, 1.0);
                t = 1.0 - s;
                feature_type = select(1i, 2i, s == 0.0 || s == 1.0);
                edge_index = 1i;
                vertex_index = select(1i, 2i, s == 0.0);
            }
        }
    }

    let closest_point = v0 + s * e0 + t * e1;
    let diff = position - closest_point;
    let sq_dist = dot(diff, diff);

    (*result).sq_distance = sq_dist;
    (*result).closest_point = closest_point;
    (*result).feature_type = feature_type;
    (*result).bary_u = s;
    (*result).bary_v = t;
    (*result).edge_index = edge_index;
    (*result).vertex_index = vertex_index;
    return sq_dist;
}

/// Pseudo-normal sign determination at the closest point.
fn mesh_pseudo_normal(payload_base: u32,
                      result: MeshClosestPointResult,
                      face_normal: vec3<f32>,
                      idx0: u32,
                      idx1: u32,
                      idx2: u32,
                      tri_index: u32) -> vec3<f32> {
    var pseudo_normal: vec3<f32>;

    if (result.feature_type == 0i) {
        pseudo_normal = face_normal;
    } else if (result.feature_type == 2i) {
        // Vertex: angle-weighted normal of the winning vertex.
        let vertex_normals_offset = mesh_load_header_uint(payload_base, 14u);
        let vertex_normal_count = u32(mesh_load_f32(payload_base, 10u));
        var v_idx = idx0;
        if (result.vertex_index == 1i) {
            v_idx = idx1;
        } else if (result.vertex_index == 2i) {
            v_idx = idx2;
        }
        if (v_idx < vertex_normal_count) {
            pseudo_normal = mesh_load_vec3(payload_base, vertex_normals_offset + v_idx * 4u);
        } else {
            pseudo_normal = face_normal;
        }
    } else {
        // Edge: sum of the two adjacent face normals (Bærentzen & Aanæs).
        let edge_neighbors_offset = mesh_load_header_uint(payload_base, 28u);
        let neighbor_slot = edge_neighbors_offset + (tri_index * 3u + u32(result.edge_index)) * 4u;
        let neighbor = vec4<f32>(mesh_load_f32(payload_base, neighbor_slot),
                                 mesh_load_f32(payload_base, neighbor_slot + 1u),
                                 mesh_load_f32(payload_base, neighbor_slot + 2u),
                                 mesh_load_f32(payload_base, neighbor_slot + 3u));
        if (neighbor.w > 0.5) {
            pseudo_normal = face_normal + neighbor.xyz;
        } else {
            pseudo_normal = face_normal;
        }
    }

    if (dot(pseudo_normal, pseudo_normal) < 1.0e-10) {
        pseudo_normal = face_normal;
    }
    return pseudo_normal;
}

const MESH_BVH_STACK_SIZE: u32 = 64u;

/// Core BVH traversal: returns signed distance (negative inside).
fn gladius_mesh_sdf_core(payload_base: u32,
                         position: vec3<f32>) -> f32 {
    let node_count = u32(mesh_load_f32(payload_base, 8u));
    let tri_count = u32(mesh_load_f32(payload_base, 9u));
    if (node_count == 0u || tri_count == 0u) {
        return -2.0e9;
    }

    let nodes_offset = mesh_load_header_uint(payload_base, 12u);
    let triangles_offset = mesh_load_header_uint(payload_base, 13u);
    let indices_offset = mesh_load_header_uint(payload_base, 15u);

    var stack: array<u32, MESH_BVH_STACK_SIZE>;
    var stack_ptr = 0u;
    stack[stack_ptr] = 0u;
    stack_ptr = stack_ptr + 1u;

    var min_sq_dist = MESH_INVALID_DISTANCE;
    var best_tri: i32 = -1i;

    let max_iterations = node_count * 2u + 100u;
    var iterations = 0u;

    loop {
        if (stack_ptr == 0u || iterations >= max_iterations) {
            break;
        }
        iterations = iterations + 1u;
        stack_ptr = stack_ptr - 1u;
        let node_index = stack[stack_ptr];

        if (node_index >= node_count) {
            continue;
        }

        let node = mesh_load_node(payload_base, nodes_offset, node_index);
        let box_sq_dist = mesh_sq_distance_to_aabb(position, node.bbox_min, node.bbox_max);
        if (box_sq_dist >= min_sq_dist) {
            continue;
        }

        if (node.left_child == -1i) {
            // Leaf: test all triangles with the fast distance-only variant.
            let prim_end = node.prim_start + node.prim_count;
            if (node.prim_start < 0i || u32(prim_end) > tri_count) {
                continue;
            }
            var i = 0u;
            loop {
                if (i >= u32(node.prim_count)) {
                    break;
                }
                let tri_index = u32(node.prim_start) + i;
                let tri = mesh_load_triangle(payload_base, triangles_offset, tri_index);
                let sq_dist = mesh_sq_triangle_fast(position, tri.v0, tri.v1, tri.v2);
                if (sq_dist < min_sq_dist) {
                    min_sq_dist = sq_dist;
                    best_tri = i32(tri_index);
                }
                i = i + 1u;
            }
        } else {
            // Internal node: ordered traversal (near child processed first).
            let left_valid = node.left_child >= 0i && u32(node.left_child) < node_count;
            let right_valid = node.right_child >= 0i && u32(node.right_child) < node_count;
            if (left_valid && right_valid && stack_ptr < MESH_BVH_STACK_SIZE - 2u) {
                let left_node = mesh_load_node(payload_base, nodes_offset, u32(node.left_child));
                let right_node = mesh_load_node(payload_base, nodes_offset, u32(node.right_child));
                let left_dist = mesh_sq_distance_to_aabb(position, left_node.bbox_min, left_node.bbox_max);
                let right_dist = mesh_sq_distance_to_aabb(position, right_node.bbox_min, right_node.bbox_max);
                if (left_dist < right_dist) {
                    stack[stack_ptr] = u32(node.right_child);
                    stack_ptr = stack_ptr + 1u;
                    stack[stack_ptr] = u32(node.left_child);
                    stack_ptr = stack_ptr + 1u;
                } else {
                    stack[stack_ptr] = u32(node.left_child);
                    stack_ptr = stack_ptr + 1u;
                    stack[stack_ptr] = u32(node.right_child);
                    stack_ptr = stack_ptr + 1u;
                }
            } else {
                if (right_valid && stack_ptr < MESH_BVH_STACK_SIZE - 1u) {
                    stack[stack_ptr] = u32(node.right_child);
                    stack_ptr = stack_ptr + 1u;
                }
                if (left_valid && stack_ptr < MESH_BVH_STACK_SIZE) {
                    stack[stack_ptr] = u32(node.left_child);
                    stack_ptr = stack_ptr + 1u;
                }
            }
        }
    }

    if (best_tri < 0i) {
        return -3.0e9;
    }

    // Deferred sign computation for the winning triangle only.
    let best_index = u32(best_tri);
    let tri = mesh_load_triangle(payload_base, triangles_offset, best_index);

    var best_result: MeshClosestPointResult;
    _ = mesh_sq_triangle_with_closest_point(position, tri.v0, tri.v1, tri.v2, &best_result);

    let indices_base = indices_offset + best_index * 4u;
    let idx0 = u32(mesh_load_int(payload_base, indices_base));
    let idx1 = u32(mesh_load_int(payload_base, indices_base + 1u));
    let idx2 = u32(mesh_load_int(payload_base, indices_base + 2u));

    let pseudo_normal = mesh_pseudo_normal(payload_base,
                                           best_result,
                                           tri.face_normal,
                                           idx0,
                                           idx1,
                                           idx2,
                                           best_index);

    let to_query = position - best_result.closest_point;
    let sign_value = select(1.0, -1.0, dot(to_query, pseudo_normal) < 0.0);
    return sign_value * sqrt(min_sq_dist);
}

/// Signed distance to the mesh resource with the given resource id.
/// Returns a large positive value when the mesh is unavailable or empty.
fn gladiusSignedDistanceToMesh(position: vec3<f32>, resource_id: u32) -> f32 {
    let start = mesh_resource_start(resource_id);
    let count = mesh_resource_count(resource_id);
    if (count < MESH_PAYLOAD_HEADER_FLOATS) {
        return MESH_INVALID_DISTANCE;
    }
    return gladius_mesh_sdf_core(start, position);
}

/// Unsigned distance to the mesh resource with the given resource id.
fn gladiusUnsignedDistanceToMesh(position: vec3<f32>, resource_id: u32) -> f32 {
    return abs(gladiusSignedDistanceToMesh(position, resource_id));
}

fn evaluateModel(position: vec3<f32>) -> vec4<f32> {
let Input_1_pos: vec3<f32> = position;
return vec4<f32>(vec3<f32>(parameters.values[1u], parameters.values[2u], parameters.values[3u]), parameters.values[0u]);
}


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