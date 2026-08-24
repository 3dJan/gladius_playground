// GLADIUS_MESH_SDF_MODULE
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
