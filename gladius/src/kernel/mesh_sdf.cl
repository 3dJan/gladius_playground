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
    int primStart;      // First triangle index (leaf only)
    int primCount;      // Number of triangles (leaf only)
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

#endif // MESH_SDF_CL
