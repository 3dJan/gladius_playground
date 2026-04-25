/// @file mesh_sdf.cl
/// @brief OpenCL kernel functions for spatial mesh SDF queries
/// @details Implements BVH traversal for closest-point queries on triangle meshes
///          with weighted pseudo-normal sign determination.
///
/// @see MeshBVH.h for host-side data structures
/// @see sdf.cl for integration with the main SDF evaluation pipeline
/// @see research.md for algorithm decisions (Bærentzen & Aanæs, 2005)

#ifndef MESH_SDF_CL
#define MESH_SDF_CL

// ============================================================================
// Data Structure Definitions (must match host-side MeshBVH.h)
// ============================================================================

/// BVH node for mesh traversal (48 bytes)
struct MeshBVHNodeGPU
{
    float4 bboxMin;     // xyz = min bounds, w unused
    float4 bboxMax;     // xyz = max bounds, w unused
    int leftChild;      // -1 if leaf
    int rightChild;     // -1 if leaf
    int primStart;      // First triangle index in this node's subtree
    int primCount;      // Number of triangles in this node's subtree
};

/// Triangle data (64 bytes - extended for precomputed face normal)
struct MeshTriangleGPU
{
    float4 v0;          // First vertex (xyz, w unused)
    float4 v1;          // Second vertex (xyz, w unused)
    float4 v2;          // Third vertex (xyz, w unused)
    float4 faceNormal;  // Precomputed face normal (xyz normalized, w unused)
};

/// Vertex normal for sign determination (16 bytes)
struct MeshVertexNormalGPU
{
    float4 normal;      // xyz = angle-weighted normal, w = vertex index
};

/// Result of closest point query (not stored, computed per query)
struct ClosestPointResult
{
    float sqDistance;       // Squared distance to closest point
    float3 closestPoint;    // Position on mesh surface
    int featureType;        // 0=face, 1=edge, 2=vertex
    int triangleIndex;      // Index of closest triangle
    float baryU;            // Barycentric coordinate u
    float baryV;            // Barycentric coordinate v
    int edgeIndex;          // Which edge (0, 1, 2) for edge features
    int vertexIndex;        // Which vertex (0, 1, 2) for vertex features
};

// ============================================================================
// Helper Functions
// ============================================================================

/// Compute squared distance from point to AABB
/// @param pos Query point
/// @param bboxMin Box minimum
/// @param bboxMax Box maximum
/// @return Squared distance (0 if inside)
inline float sqDistanceToAABB(float3 pos, float3 bboxMin, float3 bboxMax)
{
    float3 d = fmax(bboxMin - pos, fmax(pos - bboxMax, (float3)(0.0f)));
    return dot(d, d);
}

/// Compute closest point on triangle and return squared distance
/// Extended from sqTriangle() to also return closest point info
/// @param pos Query point
/// @param v0, v1, v2 Triangle vertices
/// @param result Output: closest point details
/// @return Squared distance
inline float sqTriangleWithClosestPoint(float3 pos, 
                                        float3 v0, float3 v1, float3 v2,
                                        struct ClosestPointResult* result)
{
    // Epsilon for degenerate triangle detection
    float const epsilon = 1e-10f;
    
    // Edge vectors
    float3 e0 = v1 - v0;
    float3 e1 = v2 - v0;
    float3 v = v0 - pos;
    
    float a = dot(e0, e0);
    float b = dot(e0, e1);
    float c = dot(e1, e1);
    float d = dot(e0, v);
    float e = dot(e1, v);
    
    float det = a * c - b * b;
    float s = b * e - c * d;
    float t = b * d - a * e;
    
    // Handle degenerate triangles (zero area or line/point)
    if (det < epsilon && a < epsilon && c < epsilon)
    {
        // Triangle is a point - return distance to v0
        result->sqDistance = dot(v, v);
        result->closestPoint = v0;
        result->featureType = 2;
        result->baryU = 0.0f;
        result->baryV = 0.0f;
        result->edgeIndex = -1;
        result->vertexIndex = 0;
        return result->sqDistance;
    }
    
    // Voronoi region classification
    int featureType = 0;  // 0=face, 1=edge, 2=vertex
    int edgeIndex = -1;
    int vertexIndex = -1;
    
    if (s + t <= det)
    {
        if (s < 0.0f)
        {
            if (t < 0.0f)
            {
                // Region 4: closest to v0
                if (d < 0.0f)
                {
                    t = 0.0f;
                    s = clamp(-d / a, 0.0f, 1.0f);
                    featureType = (s == 0.0f || s == 1.0f) ? 2 : 1;
                    edgeIndex = 0;
                    vertexIndex = (s == 0.0f) ? 0 : 1;
                }
                else
                {
                    s = 0.0f;
                    t = clamp(-e / c, 0.0f, 1.0f);
                    featureType = (t == 0.0f || t == 1.0f) ? 2 : 1;
                    edgeIndex = 2;
                    vertexIndex = (t == 0.0f) ? 0 : 2;
                }
            }
            else
            {
                // Region 3: closest to edge v0-v2
                s = 0.0f;
                t = clamp(-e / c, 0.0f, 1.0f);
                featureType = (t == 0.0f || t == 1.0f) ? 2 : 1;
                edgeIndex = 2;
                vertexIndex = (t == 0.0f) ? 0 : 2;
            }
        }
        else if (t < 0.0f)
        {
            // Region 5: closest to edge v0-v1
            t = 0.0f;
            s = clamp(-d / a, 0.0f, 1.0f);
            featureType = (s == 0.0f || s == 1.0f) ? 2 : 1;
            edgeIndex = 0;
            vertexIndex = (s == 0.0f) ? 0 : 1;
        }
        else
        {
            // Region 0: inside triangle (face)
            // Guard against degenerate triangles
            float invDet = (det > epsilon) ? (1.0f / det) : 0.0f;
            s *= invDet;
            t *= invDet;
            featureType = 0;
        }
    }
    else
    {
        if (s < 0.0f)
        {
            // Region 2
            float tmp0 = b + d;
            float tmp1 = c + e;
            if (tmp1 > tmp0)
            {
                float numer = tmp1 - tmp0;
                float denom = a - 2.0f * b + c;
                s = clamp(numer / denom, 0.0f, 1.0f);
                t = 1.0f - s;
                featureType = (s == 0.0f || s == 1.0f) ? 2 : 1;
                edgeIndex = 1;
                vertexIndex = (s == 0.0f) ? 2 : 1;
            }
            else
            {
                s = 0.0f;
                t = clamp(-e / c, 0.0f, 1.0f);
                featureType = (t == 0.0f || t == 1.0f) ? 2 : 1;
                edgeIndex = 2;
                vertexIndex = (t == 0.0f) ? 0 : 2;
            }
        }
        else if (t < 0.0f)
        {
            // Region 6
            float tmp0 = b + e;
            float tmp1 = a + d;
            if (tmp1 > tmp0)
            {
                float numer = tmp1 - tmp0;
                float denom = a - 2.0f * b + c;
                t = clamp(numer / denom, 0.0f, 1.0f);
                s = 1.0f - t;
                featureType = (t == 0.0f || t == 1.0f) ? 2 : 1;
                edgeIndex = 1;
                vertexIndex = (t == 0.0f) ? 1 : 2;
            }
            else
            {
                t = 0.0f;
                s = clamp(-d / a, 0.0f, 1.0f);
                featureType = (s == 0.0f || s == 1.0f) ? 2 : 1;
                edgeIndex = 0;
                vertexIndex = (s == 0.0f) ? 0 : 1;
            }
        }
        else
        {
            // Region 1: closest to edge v1-v2
            float numer = (c + e) - (b + d);
            if (numer <= 0.0f)
            {
                s = 0.0f;
                t = 1.0f;
                featureType = 2;
                vertexIndex = 2;
            }
            else
            {
                float denom = a - 2.0f * b + c;
                s = clamp(numer / denom, 0.0f, 1.0f);
                t = 1.0f - s;
                featureType = (s == 0.0f || s == 1.0f) ? 2 : 1;
                edgeIndex = 1;
                vertexIndex = (s == 0.0f) ? 2 : 1;
            }
        }
    }
    
    // Compute closest point
    float3 closestPoint = v0 + s * e0 + t * e1;
    float3 diff = pos - closestPoint;
    float sqDist = dot(diff, diff);
    
    // Store result
    result->sqDistance = sqDist;
    result->closestPoint = closestPoint;
    result->featureType = featureType;
    result->baryU = s;
    result->baryV = t;
    result->edgeIndex = edgeIndex;
    result->vertexIndex = vertexIndex;
    
    return sqDist;
}

/// Compute squared distance from point to triangle (fast, distance only)
/// Optimized version that skips closest point tracking for deferred sign computation
/// Uses Ericson's algorithm from "Real-Time Collision Detection"
/// @param pos Query point
/// @param v0, v1, v2 Triangle vertices
/// @return Squared distance
inline float sqTriangleFast(float3 pos, float3 v0, float3 v1, float3 v2)
{
    // Edge vectors
    float3 ab = v1 - v0;
    float3 ac = v2 - v0;
    float3 ap = pos - v0;

    float d1 = dot(ab, ap);
    float d2 = dot(ac, ap);

    // Check if pos is in vertex region outside v0
    if (d1 <= 0.0f && d2 <= 0.0f)
    {
        float3 diff = pos - v0;
        return dot(diff, diff);
    }

    float3 bp = pos - v1;
    float d3 = dot(ab, bp);
    float d4 = dot(ac, bp);

    // Check if pos is in vertex region outside v1
    if (d3 >= 0.0f && d4 <= d3)
    {
        float3 diff = pos - v1;
        return dot(diff, diff);
    }

    float vc = d1 * d4 - d3 * d2;

    // Check if pos is in edge region of v0-v1
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    {
        float v = d1 / (d1 - d3);
        float3 nearestPoint = v0 + v * ab;
        float3 diff = pos - nearestPoint;
        return dot(diff, diff);
    }

    float3 cp = pos - v2;
    float d5 = dot(ab, cp);
    float d6 = dot(ac, cp);

    // Check if pos is in vertex region outside v2
    if (d6 >= 0.0f && d5 <= d6)
    {
        float3 diff = pos - v2;
        return dot(diff, diff);
    }

    float vb = d5 * d2 - d1 * d6;

    // Check if pos is in edge region of v0-v2
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    {
        float w = d2 / (d2 - d6);
        float3 nearestPoint = v0 + w * ac;
        float3 diff = pos - nearestPoint;
        return dot(diff, diff);
    }

    float va = d3 * d6 - d5 * d4;

    // Check if pos is in edge region of v1-v2
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        float3 nearestPoint = v1 + w * (v2 - v1);
        float3 diff = pos - nearestPoint;
        return dot(diff, diff);
    }

    // pos is inside the triangle - compute distance to plane
    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    float3 nearestPoint = v0 + ab * v + ac * w;
    float3 diff = pos - nearestPoint;
    return dot(diff, diff);
}

// ============================================================================
// Vectorized Data Load Helpers (Option C - Phase 4)
// ============================================================================

/// Load BVH node bounding box using vectorized loads
/// BVH node layout: float4 bboxMin (0), float4 bboxMax (4), 4 ints (8-11)
/// Total: 12 floats = 48 bytes per node
/// @param nodes Base pointer to node array (as floats)
/// @param nodeIdx Node index to load
/// @param outBboxMin Output: bounding box minimum xyz
/// @param outBboxMax Output: bounding box maximum xyz
/// @param outLeftChild Output: left child index (-1 if leaf)
/// @param outRightChild Output: right child index (-1 if leaf)  
/// @param outPrimStart Output: first primitive index (leaf only)
/// @param outPrimCount Output: primitive count (leaf only)
inline void loadBVHNodeVectorized(__global float const* nodes,
                                  int nodeIdx,
                                  float3* outBboxMin,
                                  float3* outBboxMax,
                                  int* outLeftChild,
                                  int* outRightChild,
                                  int* outPrimStart,
                                  int* outPrimCount)
{
    // Each node is 12 floats (48 bytes)
    int const baseOffset = nodeIdx * 12;
    
    // Load bounding box using vload4 for coalesced memory access
    float4 bboxMin = vload4(0, nodes + baseOffset);      // floats 0-3
    float4 bboxMax = vload4(0, nodes + baseOffset + 4);  // floats 4-7
    
    // Load integer fields (stored as floats in the buffer)
    // Note: Using as_int to reinterpret float bits as int
    float4 intData = vload4(0, nodes + baseOffset + 8);  // floats 8-11
    
    *outBboxMin = bboxMin.xyz;
    *outBboxMax = bboxMax.xyz;
    *outLeftChild = as_int(intData.x);
    *outRightChild = as_int(intData.y);
    *outPrimStart = as_int(intData.z);
    *outPrimCount = as_int(intData.w);
}

/// Load triangle vertices using vectorized loads
/// Triangle layout: float4 v0 (0), float4 v1 (4), float4 v2 (8), float4 faceNormal (12)
/// Total: 16 floats = 64 bytes per triangle
/// @param triangles Base pointer to triangle array (as floats)
/// @param triIdx Triangle index to load
/// @param outV0 Output: first vertex xyz
/// @param outV1 Output: second vertex xyz
/// @param outV2 Output: third vertex xyz
inline void loadTriangleVectorized(__global float const* triangles,
                                   int triIdx,
                                   float3* outV0,
                                   float3* outV1,
                                   float3* outV2)
{
    // Each triangle is 16 floats (64 bytes)
    int const baseOffset = triIdx * 16;
    
    // Load vertices using vload4 for coalesced memory access
    float4 v0 = vload4(0, triangles + baseOffset);       // floats 0-3
    float4 v1 = vload4(0, triangles + baseOffset + 4);   // floats 4-7
    float4 v2 = vload4(0, triangles + baseOffset + 8);   // floats 8-11
    // faceNormal at floats 12-15, not loaded here
    
    *outV0 = v0.xyz;
    *outV1 = v1.xyz;
    *outV2 = v2.xyz;
}

/// Load triangle vertices and face normal using vectorized loads
/// @param triangles Base pointer to triangle array (as floats)
/// @param triIdx Triangle index to load
/// @param outV0 Output: first vertex xyz
/// @param outV1 Output: second vertex xyz
/// @param outV2 Output: third vertex xyz
/// @param outFaceNormal Output: precomputed face normal xyz
inline void loadTriangleWithNormalVectorized(__global float const* triangles,
                                             int triIdx,
                                             float3* outV0,
                                             float3* outV1,
                                             float3* outV2,
                                             float3* outFaceNormal)
{
    // Each triangle is 16 floats (64 bytes)
    int const baseOffset = triIdx * 16;
    
    // Load all 4 float4s for maximum throughput
    float4 v0 = vload4(0, triangles + baseOffset);       // floats 0-3
    float4 v1 = vload4(0, triangles + baseOffset + 4);   // floats 4-7
    float4 v2 = vload4(0, triangles + baseOffset + 8);   // floats 8-11
    float4 fn = vload4(0, triangles + baseOffset + 12);  // floats 12-15
    
    *outV0 = v0.xyz;
    *outV1 = v1.xyz;
    *outV2 = v2.xyz;
    *outFaceNormal = fn.xyz;
}

/// Compute AABB distance using vectorized node load
/// Combines loadBVHNodeVectorized with sqDistanceToAABB for common pattern
/// @param nodes Base pointer to node array (as floats)
/// @param nodeIdx Node index to check
/// @param pos Query point
/// @return Squared distance to node's bounding box
inline float sqDistanceToNodeAABB(__global float const* nodes, int nodeIdx, float3 pos)
{
    int const baseOffset = nodeIdx * 12;
    float4 bboxMin = vload4(0, nodes + baseOffset);
    float4 bboxMax = vload4(0, nodes + baseOffset + 4);
    return sqDistanceToAABB(pos, bboxMin.xyz, bboxMax.xyz);
}

/// Compute pseudo-normal for sign determination (Bærentzen & Aanæs 2005)
/// @param result Closest point query result
/// @param v0, v1, v2 Triangle vertices
/// @param precomputedFaceNormal Precomputed unit face normal of the host triangle
/// @param vertexNormals Array of angle-weighted vertex normals
/// @param vertexNormalCount Number of vertex normals (for bounds checking)
/// @param idx0, idx1, idx2 Vertex indices for this triangle
/// @param edgeNeighbors Per-edge adjacent face normals; xyz = unit normal of opposite face,
///                     w = 1 if edge is shared, 0 if boundary/non-manifold
/// @param triIdx Index of the host triangle (used to address edgeNeighbors)
/// @return Pseudo-normal at closest point. Always returned in the host triangle's outward
///         hemisphere — i.e. dot(result, precomputedFaceNormal) >= 0.
inline float3 computePseudoNormalFast(struct ClosestPointResult const* result,
                                      float3 v0, float3 v1, float3 v2,
                                      float3 precomputedFaceNormal,
                                      __global struct MeshVertexNormalGPU const* vertexNormals,
                                      int vertexNormalCount,
                                      int idx0, int idx1, int idx2,
                                      __global float4 const* edgeNeighbors,
                                      int triIdx)
{
    float3 pseudoNormal;

    if (result->featureType == 0)
    {
        // Face: use precomputed unit face normal directly.
        pseudoNormal = precomputedFaceNormal;
    }
    else if (result->featureType == 2)
    {
        // Vertex: use precomputed angle-weighted normal.
        int vIdx;
        if (result->vertexIndex == 0) vIdx = idx0;
        else if (result->vertexIndex == 1) vIdx = idx1;
        else vIdx = idx2;

        if (vIdx >= 0 && vIdx < vertexNormalCount)
        {
            pseudoNormal = vertexNormals[vIdx].normal.xyz;
        }
        else
        {
            pseudoNormal = precomputedFaceNormal;
        }
    }
    else
    {
        // Edge: per Bærentzen & Aanæs the edge pseudo-normal is the *sum of the two
        // adjacent face normals*. This is independent of the position along the edge
        // and is the only edge-formulation that yields a correct sign for arbitrary
        // dihedral angles. The previous version interpolated the angle-weighted vertex
        // normals along the edge, which collapses near sharp creases and produces sign
        // artifacts streaking away from the larger adjacent face's normal.
        float4 const neighbor = edgeNeighbors[triIdx * 3 + result->edgeIndex];
        if (neighbor.w > 0.5f)
        {
            pseudoNormal = precomputedFaceNormal + neighbor.xyz;
        }
        else
        {
            // Boundary edge: only one face contributes — use it.
            pseudoNormal = precomputedFaceNormal;
        }
    }

    // Robustness: if pseudo-normal is degenerate (e.g. exactly opposing faces),
    // fall back to the host triangle's face normal so the sign reflects the front
    // we actually hit.
    float lenSq = dot(pseudoNormal, pseudoNormal);
    if (lenSq < 1e-10f)
    {
        pseudoNormal = precomputedFaceNormal;
    }

    return pseudoNormal;
}

/// Compute pseudo-normal for sign determination (legacy version without precomputed normal)
/// @param result Closest point query result
/// @param tri Triangle data
/// @param vertexNormals Array of vertex normals
/// @param vertexNormalCount Number of vertex normals (for bounds checking)
/// @param triVertexIndices Vertex indices for this triangle (3 ints)
/// @return Pseudo-normal at closest point (may need normalization check)
inline float3 computePseudoNormal(struct ClosestPointResult const* result,
                                  float3 v0, float3 v1, float3 v2,
                                  __global struct MeshVertexNormalGPU const* vertexNormals,
                                  int vertexNormalCount,
                                  int idx0, int idx1, int idx2)
{
    float3 pseudoNormal;
    
    if (result->featureType == 0)
    {
        // Face: use face normal directly
        float3 e0 = v1 - v0;
        float3 e1 = v2 - v0;
        pseudoNormal = cross(e0, e1);
    }
    else if (result->featureType == 2)
    {
        // Vertex: use precomputed angle-weighted normal
        int vIdx;
        if (result->vertexIndex == 0) vIdx = idx0;
        else if (result->vertexIndex == 1) vIdx = idx1;
        else vIdx = idx2;
        
        // Bounds check for safety
        if (vIdx >= 0 && vIdx < vertexNormalCount)
        {
            pseudoNormal = vertexNormals[vIdx].normal.xyz;
        }
        else
        {
            // Fallback to face normal if index is out of bounds
            float3 e0 = v1 - v0;
            float3 e1 = v2 - v0;
            pseudoNormal = cross(e0, e1);
        }
    }
    else
    {
        // Edge: interpolate vertex normals at edge endpoints
        int vIdxA, vIdxB;
        float t;
        
        if (result->edgeIndex == 0)
        {
            // Edge v0-v1
            vIdxA = idx0;
            vIdxB = idx1;
            t = result->baryU;
        }
        else if (result->edgeIndex == 1)
        {
            // Edge v1-v2
            vIdxA = idx1;
            vIdxB = idx2;
            // Parameter along edge: s goes from v1 (s=1,t=0) to v2 (s=0,t=1)
            t = result->baryV;
        }
        else
        {
            // Edge v0-v2
            vIdxA = idx0;
            vIdxB = idx2;
            t = result->baryV;
        }
        
        // Bounds check for safety
        if (vIdxA >= 0 && vIdxA < vertexNormalCount && vIdxB >= 0 && vIdxB < vertexNormalCount)
        {
            float3 nA = vertexNormals[vIdxA].normal.xyz;
            float3 nB = vertexNormals[vIdxB].normal.xyz;
            pseudoNormal = (1.0f - t) * nA + t * nB;
        }
        else
        {
            // Fallback to face normal if indices are out of bounds
            float3 e0 = v1 - v0;
            float3 e1 = v2 - v0;
            pseudoNormal = cross(e0, e1);
        }
    }
    
    // Robustness: if pseudo-normal is near-zero, fall back to face normal
    float lenSq = dot(pseudoNormal, pseudoNormal);
    if (lenSq < 1e-10f)
    {
        float3 e0 = v1 - v0;
        float3 e1 = v2 - v0;
        pseudoNormal = cross(e0, e1);
    }
    
    return pseudoNormal;
}

// ============================================================================
// Main SDF Query Functions
// ============================================================================

/// Core implementation: Query signed distance to mesh using BVH traversal
/// Returns float2 where x = signed distance, y = nearest triangle index (as float)
/// This is the shared implementation used by all SDF query variants.
///
/// Performance optimizations (T024-T026 verification):
/// - T024 (FR-010) Early-out: AABB distance check skips subtrees farther than current best
/// - T025: Implicit depth limit via stack[64] and maxIterations = nodeCount*2+100
/// - T026: Vectorized loads (float4) for cache-friendly 16-byte aligned access
/// - Ordered traversal: near child first reduces average traversal depth
/// - Deferred sign: single final triangle test instead of per-triangle sign computation
///
/// @param pos Query point in world coordinates
/// @param nodesOffset Offset to BVH nodes in data array
/// @param trianglesOffset Offset to triangles in data array
/// @param normalsOffset Offset to vertex normals in data array
/// @param indicesOffset Offset to vertex indices in data array
/// @param nodeCount Number of BVH nodes
/// @param triCount Number of triangles
/// @param vertexNormalCount Number of vertex normals (for bounds checking)
/// @param data Global primitive data array
/// @param earlyExitDistanceSq Early termination threshold (squared distance).
///                            If > 0, traversal stops when minSqDist < earlyExitDistanceSq.
///                            Set to 0.0 to disable (find exact minimum distance).
/// @return float2(signedDistance, nearestTriangleIndex)
inline float2 spatialMeshSDF_Core(float3 pos,
                                  int nodesOffset,
                                  int trianglesOffset,
                                  int normalsOffset,
                                  int indicesOffset,
                                  int edgeNeighborsOffset,
                                  int nodeCount,
                                  int triCount,
                                  int vertexNormalCount,
                                  __global float const* data,
                                  float earlyExitDistanceSq)
{
    if (nodeCount == 0 || triCount == 0)
    {
        return (float2)(FLT_MAX, -1.0f);
    }
    
    // Cast data pointers - use raw float pointer for vectorized loads
    __global float const* nodesData = data + nodesOffset;
    __global float const* trianglesData = data + trianglesOffset;
    __global struct MeshVertexNormalGPU const* vertexNormals = 
        (__global struct MeshVertexNormalGPU const*)(data + normalsOffset);
    __global int const* vertexIndices = 
        (__global int const*)(data + indicesOffset);
    __global float4 const* edgeNeighbors =
        (__global float4 const*)(data + edgeNeighborsOffset);
    
    // Stack-based BVH traversal with safety limits
    int stack[64];
    int stackPtr = 0;
    stack[stackPtr++] = 0;  // Start with root node
    
    float minSqDist = FLT_MAX;
    // Deferred sign computation (Option B): only track triangle index during traversal
    // We'll recompute closest point details only for the winning triangle at the end
    int bestTriIdx = -1;
    
    // Safety: limit iterations to prevent infinite loops from corrupted data
    int const maxIterations = nodeCount * 2 + 100;
    int iterations = 0;
    
    while (stackPtr > 0 && iterations < maxIterations)
    {
        ++iterations;
        int nodeIdx = stack[--stackPtr];
        
        // Safety: bounds check on node index
        if (nodeIdx < 0 || nodeIdx >= nodeCount)
        {
            continue;
        }
        
        // Load node data using vectorized loads (Option C)
        float3 nodeBboxMin, nodeBboxMax;
        int nodeLeftChild, nodeRightChild, nodePrimStart, nodePrimCount;
        loadBVHNodeVectorized(nodesData, nodeIdx,
                              &nodeBboxMin, &nodeBboxMax,
                              &nodeLeftChild, &nodeRightChild,
                              &nodePrimStart, &nodePrimCount);
        
        // Early exit if bounding box is farther than current best
        float boxSqDist = sqDistanceToAABB(pos, nodeBboxMin, nodeBboxMax);
        if (boxSqDist >= minSqDist)
        {
            continue;
        }
        
        if (nodeLeftChild == -1)  // Leaf node
        {
            // Test all triangles in leaf
            int primEnd = nodePrimStart + nodePrimCount;
            // Safety: clamp to valid triangle range
            if (nodePrimStart < 0 || primEnd > triCount)
            {
                continue;
            }
            
            // Deferred sign computation (Option B): use fast distance-only function
            for (int i = 0; i < nodePrimCount; ++i)
            {
                int triIdx = nodePrimStart + i;
                
                // Load triangle using vectorized loads (Option C)
                float3 triV0, triV1, triV2;
                loadTriangleVectorized(trianglesData, triIdx, &triV0, &triV1, &triV2);
                
                float sqDist = sqTriangleFast(pos, triV0, triV1, triV2);
                
                if (sqDist < minSqDist)
                {
                    minSqDist = sqDist;
                    bestTriIdx = triIdx;
                    
                    // Early termination (Option F): stop if we're close enough for rendering
                    if (earlyExitDistanceSq > 0.0f && minSqDist < earlyExitDistanceSq)
                    {
                        // Break out of triangle loop and traversal loop
                        stackPtr = 0;  // Clear stack to exit main loop
                        break;
                    }
                }
            }
        }
        else  // Internal node
        {
            // Safety: check stack overflow and child validity before pushing
            // Ordered traversal (Option A): push far child first so near child is processed first
            int leftValid = (nodeLeftChild >= 0 && nodeLeftChild < nodeCount) ? 1 : 0;
            int rightValid = (nodeRightChild >= 0 && nodeRightChild < nodeCount) ? 1 : 0;
            
            if (leftValid && rightValid && stackPtr < 62)
            {
                // Compute AABB distances to both children using vectorized loads (Option C)
                float leftDist = sqDistanceToNodeAABB(nodesData, nodeLeftChild, pos);
                float rightDist = sqDistanceToNodeAABB(nodesData, nodeRightChild, pos);
                
                // Push far child first, near child last (LIFO: near processed first)
                if (leftDist < rightDist)
                {
                    stack[stackPtr++] = nodeRightChild;  // Far child first
                    stack[stackPtr++] = nodeLeftChild;   // Near child last (processed first)
                }
                else
                {
                    stack[stackPtr++] = nodeLeftChild;   // Far child first
                    stack[stackPtr++] = nodeRightChild;  // Near child last (processed first)
                }
            }
            else
            {
                // Fallback: push whichever children are valid
                if (stackPtr < 62 && rightValid)
                {
                    stack[stackPtr++] = nodeRightChild;
                }
                if (stackPtr < 63 && leftValid)
                {
                    stack[stackPtr++] = nodeLeftChild;
                }
            }
        }
    }
    
    if (bestTriIdx < 0)
    {
        return (float2)(FLT_MAX, -1.0f);
    }
    
    // Deferred sign computation (Option B): recompute closest point details for winner only
    // This single triangle test is cheap compared to full traversal
    // Use vectorized load with precomputed normal (Options C + D)
    float3 bestV0, bestV1, bestV2, bestFaceNormal;
    loadTriangleWithNormalVectorized(trianglesData, bestTriIdx, 
                                     &bestV0, &bestV1, &bestV2, &bestFaceNormal);
    
    struct ClosestPointResult bestResult;
    sqTriangleWithClosestPoint(pos, bestV0, bestV1, bestV2, &bestResult);
    
    // Compute sign using pseudo-normal with precomputed face normal (Option D)
    // Note: vertex indices are stored with stride 4 (3 indices + 1 padding per triangle)
    int idx0 = vertexIndices[bestTriIdx * 4 + 0];
    int idx1 = vertexIndices[bestTriIdx * 4 + 1];
    int idx2 = vertexIndices[bestTriIdx * 4 + 2];
    
    float3 pseudoNormal = computePseudoNormalFast(&bestResult,
        bestV0, bestV1, bestV2, bestFaceNormal,
        vertexNormals, vertexNormalCount, idx0, idx1, idx2,
        edgeNeighbors, bestTriIdx);
    
    float3 toQuery = pos - bestResult.closestPoint;
    float sign = (dot(toQuery, pseudoNormal) < 0.0f) ? -1.0f : 1.0f;
    
    float signedDist = sign * sqrt(minSqDist);
    return (float2)(signedDist, (float)bestTriIdx);
}

/// Query signed distance to mesh using BVH traversal with optional early termination
/// Thin wrapper around spatialMeshSDF_Core that returns only the signed distance.
/// @param pos Query point in world coordinates
/// @param nodesOffset Offset to BVH nodes in data array
/// @param trianglesOffset Offset to triangles in data array
/// @param normalsOffset Offset to vertex normals in data array
/// @param indicesOffset Offset to vertex indices in data array
/// @param nodeCount Number of BVH nodes
/// @param triCount Number of triangles
/// @param vertexNormalCount Number of vertex normals (for bounds checking)
/// @param data Global primitive data array
/// @param earlyExitDistanceSq Early termination threshold (squared distance).
///                            If > 0, traversal stops when minSqDist < earlyExitDistanceSq.
///                            Set to 0.0 to disable (find exact minimum distance).
/// @return Signed distance (negative inside, positive outside)
inline float spatialMeshSDFWithEarlyExit(float3 pos,
                                         int nodesOffset,
                                         int trianglesOffset,
                                         int normalsOffset,
                                         int indicesOffset,
                                         int edgeNeighborsOffset,
                                         int nodeCount,
                                         int triCount,
                                         int vertexNormalCount,
                                         __global float const* data,
                                         float earlyExitDistanceSq)
{
    return spatialMeshSDF_Core(pos, nodesOffset, trianglesOffset, normalsOffset,
                               indicesOffset, edgeNeighborsOffset,
                               nodeCount, triCount, vertexNormalCount,
                               data, earlyExitDistanceSq).x;
}

/// Query signed distance to mesh with nearest triangle index
/// Thin wrapper around spatialMeshSDF_Core for use by voxel grid build kernel.
/// @param pos Query point in world coordinates
/// @param nodesOffset Offset to BVH nodes in data array
/// @param trianglesOffset Offset to triangles in data array
/// @param normalsOffset Offset to vertex normals in data array
/// @param indicesOffset Offset to vertex indices in data array
/// @param nodeCount Number of BVH nodes
/// @param triCount Number of triangles
/// @param vertexNormalCount Number of vertex normals (for bounds checking)
/// @param data Global primitive data array
/// @return float2(signedDistance, nearestTriangleIndex)
inline float2 spatialMeshSDF_WithTriangleIndex(float3 pos,
                                                int nodesOffset,
                                                int trianglesOffset,
                                                int normalsOffset,
                                                int indicesOffset,
                                                int edgeNeighborsOffset,
                                                int nodeCount,
                                                int triCount,
                                                int vertexNormalCount,
                                                __global float const* data)
{
    // Call core with early exit disabled (0.0 = find exact minimum)
    return spatialMeshSDF_Core(pos, nodesOffset, trianglesOffset, normalsOffset,
                               indicesOffset, edgeNeighborsOffset,
                               nodeCount, triCount, vertexNormalCount,
                               data, 0.0f);
}

/// Query signed distance to mesh using BVH traversal (backward-compatible wrapper)
/// @param pos Query point in world coordinates
/// @param nodesOffset Offset to BVH nodes in data array
/// @param trianglesOffset Offset to triangles in data array
/// @param normalsOffset Offset to vertex normals in data array
/// @param indicesOffset Offset to vertex indices in data array
/// @param nodeCount Number of BVH nodes
/// @param triCount Number of triangles
/// @param vertexNormalCount Number of vertex normals (for bounds checking)
/// @param data Global primitive data array
/// @return Signed distance (negative inside, positive outside)
inline float spatialMeshSDF(float3 pos,
                            int nodesOffset,
                            int trianglesOffset,
                            int normalsOffset,
                            int indicesOffset,
                            int edgeNeighborsOffset,
                            int nodeCount,
                            int triCount,
                            int vertexNormalCount,
                            __global float const* data)
{
    // Call with early exit disabled (0.0 = find exact minimum distance)
    return spatialMeshSDFWithEarlyExit(pos,
                                       nodesOffset, trianglesOffset,
                                       normalsOffset, indicesOffset,
                                       edgeNeighborsOffset,
                                       nodeCount, triCount, vertexNormalCount,
                                       data, 0.0f);
}

/// Query unsigned distance to mesh using BVH traversal
/// @param pos Query point in world coordinates
/// @param nodesOffset Offset to BVH nodes in data array
/// @param trianglesOffset Offset to triangles in data array
/// @param nodeCount Number of BVH nodes
/// @param triCount Number of triangles
/// @param data Global primitive data array
/// @return Unsigned (absolute) distance to mesh surface
inline float spatialMeshUnsignedDistance(float3 pos,
                                         int nodesOffset,
                                         int trianglesOffset,
                                         int nodeCount,
                                         int triCount,
                                         __global float const* data)
{
    if (nodeCount == 0 || triCount == 0)
    {
        return FLT_MAX;
    }
    
    // Use raw float pointers for vectorized loads (Option C)
    __global float const* nodesData = data + nodesOffset;
    __global float const* trianglesData = data + trianglesOffset;
    
    // Stack-based BVH traversal with safety limits
    int stack[64];
    int stackPtr = 0;
    stack[stackPtr++] = 0;
    
    float minSqDist = FLT_MAX;
    
    // Safety: limit iterations to prevent infinite loops
    int const maxIterations = nodeCount * 2 + 100;
    int iterations = 0;
    
    while (stackPtr > 0 && iterations < maxIterations)
    {
        ++iterations;
        int nodeIdx = stack[--stackPtr];
        
        // Safety: bounds check
        if (nodeIdx < 0 || nodeIdx >= nodeCount)
        {
            continue;
        }
        
        // Load node data using vectorized loads (Option C)
        float3 nodeBboxMin, nodeBboxMax;
        int nodeLeftChild, nodeRightChild, nodePrimStart, nodePrimCount;
        loadBVHNodeVectorized(nodesData, nodeIdx,
                              &nodeBboxMin, &nodeBboxMax,
                              &nodeLeftChild, &nodeRightChild,
                              &nodePrimStart, &nodePrimCount);
        
        float boxSqDist = sqDistanceToAABB(pos, nodeBboxMin, nodeBboxMax);
        if (boxSqDist >= minSqDist)
        {
            continue;
        }
        
        if (nodeLeftChild == -1)
        {
            int primEnd = nodePrimStart + nodePrimCount;
            if (nodePrimStart < 0 || primEnd > triCount)
            {
                continue;
            }
            
            // Use fast distance-only function (no closest point details needed)
            for (int i = 0; i < nodePrimCount; ++i)
            {
                int triIdx = nodePrimStart + i;
                
                // Load triangle using vectorized loads (Option C)
                float3 triV0, triV1, triV2;
                loadTriangleVectorized(trianglesData, triIdx, &triV0, &triV1, &triV2);
                
                float sqDist = sqTriangleFast(pos, triV0, triV1, triV2);
                
                minSqDist = fmin(minSqDist, sqDist);
            }
        }
        else
        {
            // Ordered traversal (Option A): push far child first so near child is processed first
            int leftValid = (nodeLeftChild >= 0 && nodeLeftChild < nodeCount) ? 1 : 0;
            int rightValid = (nodeRightChild >= 0 && nodeRightChild < nodeCount) ? 1 : 0;
            
            if (leftValid && rightValid && stackPtr < 62)
            {
                // Compute AABB distances to both children using vectorized loads (Option C)
                float leftDist = sqDistanceToNodeAABB(nodesData, nodeLeftChild, pos);
                float rightDist = sqDistanceToNodeAABB(nodesData, nodeRightChild, pos);
                
                // Push far child first, near child last (LIFO: near processed first)
                if (leftDist < rightDist)
                {
                    stack[stackPtr++] = nodeRightChild;
                    stack[stackPtr++] = nodeLeftChild;
                }
                else
                {
                    stack[stackPtr++] = nodeLeftChild;
                    stack[stackPtr++] = nodeRightChild;
                }
            }
            else
            {
                // Fallback: push whichever children are valid
                if (stackPtr < 62 && rightValid)
                {
                    stack[stackPtr++] = nodeRightChild;
                }
                if (stackPtr < 63 && leftValid)
                {
                    stack[stackPtr++] = nodeLeftChild;
                }
            }
        }
    }
    
    return sqrt(minSqDist);
}

// ============================================================================
// Voxel Acceleration Grid (Option G)
// ============================================================================

/// Voxel grid header structure (mirrors MeshVoxelGridHeader in MeshVoxelGrid.h)
/// Header layout: 10 floats
///   [0-2]: origin (x, y, z)
///   [3-5]: dimensions (x, y, z) as floats
///   [6]: voxelSize
///   [7]: invVoxelSize
///   [8]: threshold
///   [9]: padding

/// Lookup voxel data for a position
/// @param pos Query position
/// @param header Pointer to voxel grid header (10 floats)
/// @param voxelData Pointer to voxel data array
/// @param voxelCount Total number of voxels
/// @return float2(nearestTriIndex, approxSignedDist), or (-1, FLT_MAX) if out of bounds
inline float2 lookupMeshVoxel(float3 pos,
                               __global float const* header,
                               __global float const* voxelData,
                               int voxelCount)
{
    // Extract header values
    float3 origin = (float3)(header[0], header[1], header[2]);
    int3 dims = (int3)((int)header[3], (int)header[4], (int)header[5]);
    float invVoxelSize = header[7];
    
    // Compute voxel coordinates
    float3 localPos = (pos - origin) * invVoxelSize;
    int3 coord = (int3)((int)floor(localPos.x), (int)floor(localPos.y), (int)floor(localPos.z));
    
    // Bounds check
    if (coord.x < 0 || coord.x >= dims.x ||
        coord.y < 0 || coord.y >= dims.y ||
        coord.z < 0 || coord.z >= dims.z)
    {
        return (float2)(-1.0f, FLT_MAX);
    }
    
    // Compute linear index
    int voxelIdx = coord.z * dims.y * dims.x + coord.y * dims.x + coord.x;
    if (voxelIdx < 0 || voxelIdx >= voxelCount)
    {
        return (float2)(-1.0f, FLT_MAX);
    }
    
    // Read voxel data (2 floats per voxel)
    float nearestTriIndex = voxelData[voxelIdx * 2 + 0];
    float approxSignedDist = voxelData[voxelIdx * 2 + 1];
    
    return (float2)(nearestTriIndex, approxSignedDist);
}

/// Query mesh SDF using voxel acceleration with 2x2x2 stencil lookup
/// For positions far from the surface, uses cached triangle only.
/// Near the surface, falls back to full BVH traversal for accuracy.
/// @param pos Query position
/// @param headerOffset Offset to voxel grid header in data array
/// @param voxelDataOffset Offset to voxel data in data array
/// @param nodesOffset BVH nodes offset
/// @param trianglesOffset Triangles offset
/// @param normalsOffset Vertex normals offset
/// @param indicesOffset Vertex indices offset
/// @param nodeCount Number of BVH nodes
/// @param triCount Number of triangles
/// @param vertexNormalCount Number of vertex normals
/// @param data Global data array
/// @return Signed distance
inline float spatialMeshSDF_VoxelAccelerated(float3 pos,
                                              int headerOffset,
                                              int voxelDataOffset,
                                              int nodesOffset,
                                              int trianglesOffset,
                                              int normalsOffset,
                                              int indicesOffset,
                                              int edgeNeighborsOffset,
                                              int nodeCount,
                                              int triCount,
                                              int vertexNormalCount,
                                              __global float const* data)
{
    __global float const* header = data + headerOffset;
    __global float const* voxelData = data + voxelDataOffset;
    
    // Extract header values
    int3 dims = (int3)((int)header[3], (int)header[4], (int)header[5]);
    float threshold = header[8];
    int voxelCount = dims.x * dims.y * dims.z;
    
    // O(1) voxel lookup
    float2 voxelResult = lookupMeshVoxel(pos, header, voxelData, voxelCount);
    int nearestTriIdx = (int)voxelResult.x;
    float approxDist = voxelResult.y;
    
    // If out of bounds or no valid triangle, fall back to BVH
    if (nearestTriIdx < 0 || nearestTriIdx >= triCount)
    {
        return spatialMeshSDF(pos, nodesOffset, trianglesOffset, normalsOffset,
                              indicesOffset, edgeNeighborsOffset,
                              nodeCount, triCount, vertexNormalCount, data);
    }
    
    // If far from surface, use approximate distance with sign from cached value
    // This covers ~80% of raymarching queries
    if (fabs(approxDist) > threshold)
    {
        // Compute exact distance to cached triangle only
        __global struct MeshTriangleGPU const* triangles = 
            (__global struct MeshTriangleGPU const*)(data + trianglesOffset);
        
        struct MeshTriangleGPU tri = triangles[nearestTriIdx];
        float sqDist = sqTriangleFast(pos, tri.v0.xyz, tri.v1.xyz, tri.v2.xyz);
        
        // Use sign from precomputed approximate distance
        float sign = (approxDist < 0.0f) ? -1.0f : 1.0f;
        return sign * sqrt(sqDist);
    }
    
    // Near surface - fall back to full BVH traversal for accuracy
    return spatialMeshSDF(pos, nodesOffset, trianglesOffset, normalsOffset,
                          indicesOffset, edgeNeighborsOffset,
                          nodeCount, triCount, vertexNormalCount, data);
}

// ============================================================================
// Fast Winding Number (Barill et al., SIGGRAPH 2018)
// ============================================================================
//
// Aggregate buffer layout (8 floats per BVH node, indexed by node index):
//   [0..2] weightedNormalSum.xyz  ( = Σ 2·area·n over the subtree )
//   [3]    bounding radius about the area-weighted centroid
//   [4..6] areaCentroid.xyz       ( = Σ area·centroid )
//   [7]    totalArea              ( = Σ area )
//
// The Barnes-Hut acceptance criterion is `dist > beta * radius`; when met the
// subtree contributes via the dipole formula
//     w_dipole(p) = (1 / (4π)) · ( (c - p) · N̂ ) / |c - p|^3
// where c is the area-weighted centroid and N̂ = Σ 2·area·n  is the doubled
// area-weighted normal sum stored above.  When the criterion is not met and
// the node is internal, traversal recurses; at a leaf the exact per-triangle
// solid-angle formula (Van Oosterom-Strang) is used.

/// Solid angle subtended at the origin by a triangle (a, b, c).
/// @return Signed solid angle in (-2π, 2π].
inline float fwnSolidAngleAtOrigin(float3 a, float3 b, float3 c)
{
    float const la = length(a);
    float const lb = length(b);
    float const lc = length(c);
    float const numerator = dot(a, cross(b, c));
    float const denominator = la * lb * lc + dot(a, b) * lc + dot(b, c) * la + dot(c, a) * lb;
    return 2.0f * atan2(numerator, denominator);
}

/// Compute the generalised winding number at `pos` using a Barnes-Hut
/// hierarchical traversal of the BVH augmented with per-node multipole
/// aggregates.
///
/// **Experimental distance-aware pruning** (when @p unsignedDist > 0): skips
/// subtrees whose nearest-AABB distance exceeds @c FWN_PRUNE_FACTOR *
/// unsignedDist. This is fast but approximate; near a surface, winding is a
/// global quantity and aggressive pruning can expose local triangle/BVH
/// partitioning as visible sign artifacts. Pass @p unsignedDist <= 0 to
/// disable (safe/full Barnes-Hut behaviour).
///
/// **Front-to-back traversal**: when both children are pushed, the closer
/// one (by AABB distance) is pushed last so it is popped first. Closer
/// subtrees contribute more winding mass earlier for inside points.
///
/// @param beta Barnes-Hut acceptance threshold (typical 1.5 – 4.0).
/// @param unsignedDist Unsigned distance from @p pos to the surface (for
///                     distance-aware pruning); pass 0 to disable.
/// @return winding number (~1 inside, ~0 outside; signed for non-closed input).
inline float fwnHierarchical(float3 pos,
                             int nodesOffset,
                             int trianglesOffset,
                             int fwnAggregatesOffset,
                             int nodeCount,
                             __global float const * data,
                             float beta,
                             float unsignedDist)
{
    if (nodeCount == 0)
    {
        return 0.0f;
    }

    __global float const * nodes = data + nodesOffset;
    __global float const * triangles = data + trianglesOffset;
    __global float const * aggregates = data + fwnAggregatesOffset;

    // Experimental distance-aware pruning radius (squared). Disabled when
    // unsignedDist <= 0. Runtime FWN keeps this disabled because near-surface
    // global winding can be visibly affected by dropping far subtrees.
    float const FWN_PRUNE_FACTOR = 8.0f;
    float const pruneDistSq = (unsignedDist > 0.0f)
        ? (FWN_PRUNE_FACTOR * unsignedDist) * (FWN_PRUNE_FACTOR * unsignedDist)
        : -1.0f;

    // Iterative DFS with explicit stack. Depth 128 leaves headroom for very
    // unbalanced BVHs (depth 64 covers 2^64 tris in the balanced case but
    // long chains can exceed that). When the stack is full we drop the child
    // pushes; this biases the winding estimate but never crashes.
    int stack[128];
    int stackTop = 0;
    stack[stackTop++] = 0; // root

    float windingSum = 0.0f;
    float const betaSq = beta * beta;
    float const invFourPi = 1.0f / (4.0f * M_PI_F);

    while (stackTop > 0)
    {
        int const nodeIdx = stack[--stackTop];

        // Load BVH node AABB first (cheap, used for distance-aware prune
        // before we touch the aggregate buffer).
        int const baseOffset = nodeIdx * 12;
        float4 const bboxMin = vload4(0, nodes + baseOffset);      // floats 0-3
        float4 const bboxMax = vload4(0, nodes + baseOffset + 4);  // floats 4-7
        float const aabbDistSq = sqDistanceToAABB(pos, bboxMin.xyz, bboxMax.xyz);

        // Distance-aware prune: subtree is too far to influence the sign.
        if (pruneDistSq > 0.0f && aabbDistSq > pruneDistSq)
        {
            continue;
        }

        // Load aggregate (8 floats = 2 × float4).
        int const agBase = nodeIdx * 8;
        float4 const wnRadius = vload4(0, aggregates + agBase);     // weightedSum.xyz, radius
        float4 const acTotal = vload4(0, aggregates + agBase + 4);  // centroid.xyz, totalArea

        if (acTotal.w <= 0.0f)
        {
            continue; // empty subtree
        }

        float3 const centroid = acTotal.xyz / acTotal.w;
        float3 const diff = centroid - pos;
        float const distSq = dot(diff, diff);
        float const radius = wnRadius.w;

        // Barnes-Hut acceptance: dist > beta * radius
        // (compare squared to avoid a sqrt)
        if (distSq > betaSq * radius * radius && distSq > 0.0f)
        {
            // Dipole approximation: contribution is (diff · N) / (4π · |diff|^3)
            // where N here is wnRadius.xyz (already includes the 2·area factor).
            float const invDist = rsqrt(distSq);
            float const invDistCubed = invDist * invDist * invDist;
            // Note: wnRadius.xyz = Σ 2·area·n  → divide by 2 for dipole formula.
            float3 const N = 0.5f * wnRadius.xyz;
            windingSum += invFourPi * dot(diff, N) * invDistCubed;

            continue;
        }

        // Recurse / evaluate exactly. Children indices live in the int data
        // block of the BVH node we already loaded.
        float4 const intData = vload4(0, nodes + baseOffset + 8);
        int const leftChild = as_int(intData.x);
        int const rightChild = as_int(intData.y);
        int const primStart = as_int(intData.z);
        int const primCount = as_int(intData.w);

        bool const isLeaf = (leftChild == -1 && rightChild == -1);
        if (isLeaf)
        {
            int const end = primStart + primCount;
            for (int i = primStart; i < end; ++i)
            {
                int const triBase = i * 16;
                float3 const v0 = vload4(0, triangles + triBase).xyz - pos;
                float3 const v1 = vload4(0, triangles + triBase + 4).xyz - pos;
                float3 const v2 = vload4(0, triangles + triBase + 8).xyz - pos;
                windingSum += invFourPi * fwnSolidAngleAtOrigin(v0, v1, v2);
            }
        }
        else
        {
            // Front-to-back: push the farther child first so the closer one
            // is popped first. Use AABB squared-distance for the comparison.
            float leftDistSq = FLT_MAX;
            float rightDistSq = FLT_MAX;
            if (leftChild >= 0)
            {
                int const leftBase = leftChild * 12;
                float4 const lMin = vload4(0, nodes + leftBase);
                float4 const lMax = vload4(0, nodes + leftBase + 4);
                leftDistSq = sqDistanceToAABB(pos, lMin.xyz, lMax.xyz);
            }
            if (rightChild >= 0)
            {
                int const rightBase = rightChild * 12;
                float4 const rMin = vload4(0, nodes + rightBase);
                float4 const rMax = vload4(0, nodes + rightBase + 4);
                rightDistSq = sqDistanceToAABB(pos, rMin.xyz, rMax.xyz);
            }

            int const nearChild = (leftDistSq <= rightDistSq) ? leftChild : rightChild;
            int const farChild  = (leftDistSq <= rightDistSq) ? rightChild : leftChild;

            if (farChild >= 0 && stackTop < 128)
            {
                stack[stackTop++] = farChild;
            }
            if (nearChild >= 0 && stackTop < 128)
            {
                stack[stackTop++] = nearChild;
            }
        }
    }

    return windingSum;
}

/// Lookup the coarse FWN sign cache.
/// @return 1 for inside, -1 for outside, 0 when the cache cannot be used.
inline int lookupFwnSignCache(float3 pos,
                              int nodesOffset,
                              int signCacheDataOffset,
                              int signCacheResolution,
                              float signCacheBeta,
                              float unsignedDist,
                              float runtimeBeta,
                              __global float const * data)
{
    if (signCacheDataOffset <= 0 || signCacheResolution <= 0 || unsignedDist <= 0.0f ||
        fabs(signCacheBeta - runtimeBeta) > 1.0e-4f)
    {
        return 0;
    }

    __global float const * nodes = data + nodesOffset;
    float4 const bboxMin4 = vload4(0, nodes);
    float4 const bboxMax4 = vload4(0, nodes + 4);
    float3 const bboxMin = bboxMin4.xyz;
    float3 const bboxMax = bboxMax4.xyz;
    float3 const extent = bboxMax - bboxMin;

    if (extent.x <= 0.0f || extent.y <= 0.0f || extent.z <= 0.0f)
    {
        return 0;
    }

    if (pos.x < bboxMin.x || pos.y < bboxMin.y || pos.z < bboxMin.z ||
        pos.x > bboxMax.x || pos.y > bboxMax.y || pos.z > bboxMax.z)
    {
        return 0;
    }

    float3 const cellSize = extent / (float)signCacheResolution;
    float const cellDiag = length(cellSize);
    if (unsignedDist <= 0.5f * cellDiag)
    {
        return 0;
    }

    int3 cell = convert_int3(floor(((pos - bboxMin) / extent) * (float)signCacheResolution));
    cell = clamp(cell, (int3)(0), (int3)(signCacheResolution - 1));

    int const cellIndex = cell.x + signCacheResolution * (cell.y + signCacheResolution * cell.z);
    int const wordIndex = cellIndex >> 4;
    int const bitShift = (cellIndex & 15) * 2;
    __global uint const * signCacheWords = (__global uint const *)(data + signCacheDataOffset);
    uint const word = signCacheWords[wordIndex];
    uint const state = (word >> bitShift) & 3u;
    if (state == 0u)
    {
        return 0;
    }
    return (state == 2u) ? 1 : -1;
}

/// Signed distance via Fast Winding Number sign + closest-point magnitude.
/// Robust to open/non-manifold/self-intersecting meshes.
///
/// When a populated voxel acceleration grid is available
/// (@p voxelHeaderOffset > 0 and @p voxelDataOffset > 0), the magnitude is
/// taken from the voxel-accelerated path which only falls back to a full BVH
/// walk near the surface. This avoids the duplicate full BVH traversal that
/// would otherwise dominate FWN cost on large meshes.
///
/// **Far-field skip** (@p farFieldDistance > 0): when the unsigned distance
/// returned by the magnitude pass exceeds this threshold, the query point is
/// far from any feature and we trust the cheap pseudo-normal / voxel sign
/// instead of running the hierarchical winding traversal. For a typical
/// render (most pixels far from the surface) this drops FWN cost to nearly
/// the cost of the magnitude path on the bulk of the image.
inline float spatialMeshSDF_FastWindingNumber(float3 pos,
                                              int nodesOffset,
                                              int trianglesOffset,
                                              int normalsOffset,
                                              int indicesOffset,
                                              int edgeNeighborsOffset,
                                              int fwnAggregatesOffset,
                                              int voxelHeaderOffset,
                                              int voxelDataOffset,
                                              int signCacheDataOffset,
                                              int signCacheResolution,
                                              float signCacheBeta,
                                              int nodeCount,
                                              int triCount,
                                              int vertexNormalCount,
                                              __global float const * data,
                                              float beta,
                                              float farFieldDistance)
{
    // Magnitude (signed): prefer the voxel-accelerated path when available.
    float signedMagnitude;
    if (voxelHeaderOffset > 0 && voxelDataOffset > 0)
    {
        signedMagnitude = spatialMeshSDF_VoxelAccelerated(pos,
                                                          voxelHeaderOffset,
                                                          voxelDataOffset,
                                                          nodesOffset,
                                                          trianglesOffset,
                                                          normalsOffset,
                                                          indicesOffset,
                                                          edgeNeighborsOffset,
                                                          nodeCount,
                                                          triCount,
                                                          vertexNormalCount,
                                                          data);
    }
    else
    {
        float2 const closest =
            spatialMeshSDF_Core(pos, nodesOffset, trianglesOffset, normalsOffset,
                                indicesOffset, edgeNeighborsOffset,
                                nodeCount, triCount, vertexNormalCount, data, 0.0f);
        signedMagnitude = closest.x;
    }

    float const unsignedDist = fabs(signedMagnitude);

    // Far-field skip: when the closest triangle is far away the magnitude
    // path's sign is reliable (no nearby features to confuse the
    // pseudo-normal / voxel sign). Skip the expensive winding traversal
    // entirely. Covers the majority of pixels on a typical render.
    if (farFieldDistance > 0.0f && unsignedDist > farFieldDistance)
    {
        return signedMagnitude;
    }

    // Coarse sign cache: once populated, this skips the winding traversal for
    // points that are far enough from the surface not to share a cache cell
    // with it. The lookup itself applies a conservative half-cell-diagonal gate.
    int const cachedSign = lookupFwnSignCache(pos,
                                              nodesOffset,
                                              signCacheDataOffset,
                                              signCacheResolution,
                                              signCacheBeta,
                                              unsignedDist,
                                              beta,
                                              data);
    if (cachedSign != 0)
    {
        return (cachedSign > 0) ? -unsignedDist : unsignedDist;
    }

    // Sign: hierarchical winding number. Do not pass unsignedDist for the
    // distance-aware prune here: near the surface, winding is a global
    // quantity and pruning far subtrees makes the result depend on the local
    // triangle fan / BVH partitioning, which shows up as triangle-edge
    // artifacts on otherwise smooth faces. Keep the safer front-to-back
    // traversal order, but evaluate the full Barnes-Hut tree for sign.
    float const winding = fwnHierarchical(pos, nodesOffset, trianglesOffset,
                                          fwnAggregatesOffset, nodeCount, data, beta,
                                          0.0f);
    return (winding > 0.5f) ? -unsignedDist : unsignedDist;
}

// ============================================================================
// Voxel Grid Build Kernel
// ============================================================================

/// Build voxel acceleration grid on GPU (in-place in primitive buffer)
/// Each work item computes the SDF at one voxel center and stores the result.
/// Launch with global size = total voxel count (dims.x * dims.y * dims.z)
/// @param primitiveData Primitive data buffer (read BVH, write voxels)
/// @param headerStart Start offset of the spatial mesh header in primitiveData
/// @param voxelDataOffset Offset where voxel data should be written
/// @param nodesOffset BVH nodes offset in primitiveData
/// @param trianglesOffset Triangles offset in primitiveData
/// @param normalsOffset Vertex normals offset in primitiveData
/// @param indicesOffset Vertex indices offset in primitiveData
/// @param nodeCount Number of BVH nodes
/// @param triCount Number of triangles
/// @param vertexNormalCount Number of vertex normals
__kernel void buildMeshVoxelGrid(
    __global float* primitiveData,
    int headerStart,
    int voxelDataOffset,
    int nodesOffset,
    int trianglesOffset,
    int normalsOffset,
    int indicesOffset,
    int edgeNeighborsOffset,
    int nodeCount,
    int triCount,
    int vertexNormalCount)
{
    int const voxelIdx = get_global_id(0);
    
    // Voxel grid header is at headerStart + 16 (after bbox, counts, BVH offsets)
    int const voxelHeaderOffset = headerStart + 16;
    
    // Extract header values from voxel grid header
    float3 const origin = (float3)(primitiveData[voxelHeaderOffset + 0],
                                    primitiveData[voxelHeaderOffset + 1],
                                    primitiveData[voxelHeaderOffset + 2]);
    int3 const dims = (int3)((int)primitiveData[voxelHeaderOffset + 3],
                              (int)primitiveData[voxelHeaderOffset + 4],
                              (int)primitiveData[voxelHeaderOffset + 5]);
    float const voxelSize = primitiveData[voxelHeaderOffset + 6];
    
    // Bounds check
    int const totalVoxels = dims.x * dims.y * dims.z;
    if (voxelIdx >= totalVoxels)
    {
        return;
    }
    
    // Compute 3D voxel coordinates from linear index
    int const z = voxelIdx / (dims.x * dims.y);
    int const remainder = voxelIdx % (dims.x * dims.y);
    int const y = remainder / dims.x;
    int const x = remainder % dims.x;
    
    // Compute voxel center in world space
    float3 const center = origin + (convert_float3((int3)(x, y, z)) + 0.5f) * voxelSize;
    
    // Query signed distance and nearest triangle using BVH traversal
    // This returns float2(signedDist, nearestTriangleIndex)
    float2 const result = spatialMeshSDF_WithTriangleIndex(center,
                                                            nodesOffset,
                                                            trianglesOffset,
                                                            normalsOffset,
                                                            indicesOffset,
                                                            edgeNeighborsOffset,
                                                            nodeCount,
                                                            triCount,
                                                            vertexNormalCount,
                                                            primitiveData);
    
    float const signedDist = result.x;
    float const nearestTriIdx = result.y;
    
    // Store results: 2 floats per voxel at voxelDataOffset
    // [0] = nearest triangle index (enables O(1) lookup for queries far from surface)
    // [1] = signed distance at voxel center (for threshold-based fast path)
    int const outputIdx = voxelDataOffset + voxelIdx * 2;
    primitiveData[outputIdx + 0] = nearestTriIdx;
    primitiveData[outputIdx + 1] = signedDist;
}

/// Build Fast-Winding-Number per-node aggregate records in-place.
/// Each work item handles one BVH node. Internal nodes use the host-provided
/// contiguous subtree triangle range, so this avoids CPU-side bottom-up FWN
/// prep while keeping the query-time aggregate layout unchanged.
__kernel void buildMeshFwnAggregates(__global float * primitiveData,
                                     int nodesOffset,
                                     int trianglesOffset,
                                     int fwnAggregatesOffset,
                                     int nodeCount,
                                     int triCount)
{
    int const nodeIdx = (int)get_global_id(0);
    if (nodeIdx >= nodeCount || nodeCount <= 0 || triCount <= 0 || fwnAggregatesOffset <= 0)
    {
        return;
    }

    __global float const * nodes = primitiveData + nodesOffset;
    __global float const * triangles = primitiveData + trianglesOffset;
    __global float * aggregates = primitiveData + fwnAggregatesOffset;

    int const nodeBase = nodeIdx * 12;
    float4 const intData = vload4(0, nodes + nodeBase + 8);
    int const primStart = as_int(intData.z);
    int const primCount = as_int(intData.w);
    int const primEnd = primStart + primCount;

    int const agBase = nodeIdx * 8;
    if (primStart < 0 || primCount <= 0 || primStart >= triCount || primEnd > triCount)
    {
        vstore4((float4)(0.0f, 0.0f, 0.0f, 0.0f), 0, aggregates + agBase);
        vstore4((float4)(0.0f, 0.0f, 0.0f, 0.0f), 0, aggregates + agBase + 4);
        return;
    }

    float3 weightedNormalSum = (float3)(0.0f, 0.0f, 0.0f);
    float3 areaCentroid = (float3)(0.0f, 0.0f, 0.0f);
    float totalArea = 0.0f;

    for (int i = primStart; i < primEnd; ++i)
    {
        int const triBase = i * 16;
        float3 const v0 = vload4(0, triangles + triBase).xyz;
        float3 const v1 = vload4(0, triangles + triBase + 4).xyz;
        float3 const v2 = vload4(0, triangles + triBase + 8).xyz;
        float3 const faceNormal = vload4(0, triangles + triBase + 12).xyz;

        float3 const e = v1 - v0;
        float3 const f = v2 - v0;
        float const area = 0.5f * length(cross(e, f));
        float3 const centroid = (v0 + v1 + v2) * (1.0f / 3.0f);

        weightedNormalSum += (2.0f * area) * faceNormal;
        areaCentroid += area * centroid;
        totalArea += area;
    }

    float radius = 0.0f;
    if (totalArea > 0.0f)
    {
        float3 const centroid = areaCentroid / totalArea;
        float radiusSq = 0.0f;
        for (int i = primStart; i < primEnd; ++i)
        {
            int const triBase = i * 16;
            float3 const v0 = vload4(0, triangles + triBase).xyz;
            float3 const v1 = vload4(0, triangles + triBase + 4).xyz;
            float3 const v2 = vload4(0, triangles + triBase + 8).xyz;
            float3 const d0 = v0 - centroid;
            float3 const d1 = v1 - centroid;
            float3 const d2 = v2 - centroid;
            radiusSq = fmax(radiusSq, dot(d0, d0));
            radiusSq = fmax(radiusSq, dot(d1, d1));
            radiusSq = fmax(radiusSq, dot(d2, d2));
        }
        radius = sqrt(radiusSq);
    }

    vstore4((float4)(weightedNormalSum.x, weightedNormalSum.y, weightedNormalSum.z, radius),
            0,
            aggregates + agBase);
    vstore4((float4)(areaCentroid.x, areaCentroid.y, areaCentroid.z, totalArea),
            0,
            aggregates + agBase + 4);
}

/// Build a coarse conservative FWN sign cache.
/// Each work item writes one 32-bit word (16 two-bit cache cells) to avoid atomics.
/// The host queues this kernel in small word batches to avoid long-running
/// kernels on display GPUs.
__kernel void buildMeshSignCache(__global float * primitiveData,
                                 int headerStart,
                                 int signCacheDataOffset,
                                 int nodesOffset,
                                 int trianglesOffset,
                                 int normalsOffset,
                                 int indicesOffset,
                                 int edgeNeighborsOffset,
                                 int fwnAggregatesOffset,
                                 int nodeCount,
                                 int triCount,
                                 int vertexNormalCount,
                                 int resolution,
                                 int baseWord,
                                 int wordCount,
                                 float beta)
{
    int const wordIndex = baseWord + (int)get_global_id(0);
    if (wordIndex >= wordCount || resolution <= 0 || nodeCount <= 0)
    {
        return;
    }

    float3 const bboxMin = (float3)(primitiveData[headerStart + 0],
                                    primitiveData[headerStart + 1],
                                    primitiveData[headerStart + 2]);
    float3 const bboxMax = (float3)(primitiveData[headerStart + 4],
                                    primitiveData[headerStart + 5],
                                    primitiveData[headerStart + 6]);
    float3 const extent = bboxMax - bboxMin;
    __global uint * signCacheWords = (__global uint *)(primitiveData + signCacheDataOffset);

    if (extent.x <= 0.0f || extent.y <= 0.0f || extent.z <= 0.0f)
    {
        signCacheWords[wordIndex] = 0u;
        return;
    }

    int const cellsPerSlice = resolution * resolution;
    int const cellCount = cellsPerSlice * resolution;
    int const firstCell = wordIndex * 16;
    float3 const cellSize = extent / (float)resolution;
    float const cellDiag = length(cellSize);
    uint bits = 0u;

    for (int cellInWord = 0; cellInWord < 16; ++cellInWord)
    {
        int const cellIndex = firstCell + cellInWord;
        if (cellIndex >= cellCount)
        {
            break;
        }

        int const z = cellIndex / cellsPerSlice;
        int const rem = cellIndex - z * cellsPerSlice;
        int const y = rem / resolution;
        int const x = rem - y * resolution;

        float3 const pos = bboxMin + ((float3)((float)x + 0.5f,
                                               (float)y + 0.5f,
                                               (float)z + 0.5f) * cellSize);
        float2 const closest = spatialMeshSDF_Core(pos,
                                                   nodesOffset,
                                                   trianglesOffset,
                                                   normalsOffset,
                                                   indicesOffset,
                                                   edgeNeighborsOffset,
                                                   nodeCount,
                                                   triCount,
                                                   vertexNormalCount,
                                                   primitiveData,
                                                   0.0f);
        if (fabs(closest.x) <= 0.5f * cellDiag)
        {
            continue; // Unknown: cell may contain or touch the surface.
        }

        float const winding = fwnHierarchical(pos,
                                              nodesOffset,
                                              trianglesOffset,
                                              fwnAggregatesOffset,
                                              nodeCount,
                                              primitiveData,
                                              beta,
                                              0.0f);
        uint const state = (winding > 0.5f) ? 2u : 1u;
        bits |= state << (cellInWord * 2);
    }

    signCacheWords[wordIndex] = bits;
}

/// Mark a queued sign-cache build as ready. This kernel is queued after all
/// buildMeshSignCache chunks, so render kernels see a non-zero offset only
/// after the bitmap has been fully populated.
__kernel void markMeshSignCacheReady(__global float * primitiveData,
                                     int signCacheReadyOffset,
                                     int signCacheDataOffset,
                                     int signCacheBetaOffset,
                                     float beta)
{
    primitiveData[signCacheBetaOffset] = beta;
    primitiveData[signCacheReadyOffset] = (float)signCacheDataOffset;
}

#endif // MESH_SDF_CL
