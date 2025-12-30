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

/// Triangle data (48 bytes)
struct MeshTriangleGPU
{
    float4 v0;          // First vertex (xyz, w unused)
    float4 v1;          // Second vertex (xyz, w unused)
    float4 v2;          // Third vertex (xyz, w unused)
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
            float invDet = 1.0f / det;
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

/// Compute pseudo-normal for sign determination
/// @param result Closest point query result
/// @param tri Triangle data
/// @param vertexNormals Array of vertex normals
/// @param triVertexIndices Vertex indices for this triangle (3 ints)
/// @return Pseudo-normal at closest point (may need normalization check)
inline float3 computePseudoNormal(struct ClosestPointResult const* result,
                                  float3 v0, float3 v1, float3 v2,
                                  __global struct MeshVertexNormalGPU const* vertexNormals,
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
        
        pseudoNormal = vertexNormals[vIdx].normal.xyz;
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
        
        float3 nA = vertexNormals[vIdxA].normal.xyz;
        float3 nB = vertexNormals[vIdxB].normal.xyz;
        pseudoNormal = (1.0f - t) * nA + t * nB;
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

/// Query signed distance to mesh using BVH traversal
/// @param pos Query point in world coordinates
/// @param nodesOffset Offset to BVH nodes in data array
/// @param trianglesOffset Offset to triangles in data array
/// @param normalsOffset Offset to vertex normals in data array
/// @param indicesOffset Offset to vertex indices in data array
/// @param nodeCount Number of BVH nodes
/// @param triCount Number of triangles
/// @param data Global primitive data array
/// @return Signed distance (negative inside, positive outside)
inline float spatialMeshSDF(float3 pos,
                            int nodesOffset,
                            int trianglesOffset,
                            int normalsOffset,
                            int indicesOffset,
                            int nodeCount,
                            int triCount,
                            __global float const* data)
{
    if (nodeCount == 0 || triCount == 0)
    {
        return FLT_MAX;
    }
    
    // Cast data pointers
    __global struct MeshBVHNodeGPU const* nodes = 
        (__global struct MeshBVHNodeGPU const*)(data + nodesOffset);
    __global struct MeshTriangleGPU const* triangles = 
        (__global struct MeshTriangleGPU const*)(data + trianglesOffset);
    __global struct MeshVertexNormalGPU const* vertexNormals = 
        (__global struct MeshVertexNormalGPU const*)(data + normalsOffset);
    __global int const* vertexIndices = 
        (__global int const*)(data + indicesOffset);
    
    // Stack-based BVH traversal with safety limits
    int stack[64];
    int stackPtr = 0;
    stack[stackPtr++] = 0;  // Start with root node
    
    float minSqDist = FLT_MAX;
    struct ClosestPointResult bestResult;
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
        
        struct MeshBVHNodeGPU node = nodes[nodeIdx];
        
        // Early exit if bounding box is farther than current best
        float boxSqDist = sqDistanceToAABB(pos, node.bboxMin.xyz, node.bboxMax.xyz);
        if (boxSqDist >= minSqDist)
        {
            continue;
        }
        
        if (node.leftChild == -1)  // Leaf node
        {
            // Test all triangles in leaf
            int primEnd = node.primStart + node.primCount;
            // Safety: clamp to valid triangle range
            if (node.primStart < 0 || primEnd > triCount)
            {
                continue;
            }
            
            for (int i = 0; i < node.primCount; ++i)
            {
                int triIdx = node.primStart + i;
                struct MeshTriangleGPU tri = triangles[triIdx];
                
                struct ClosestPointResult result;
                float sqDist = sqTriangleWithClosestPoint(pos, 
                    tri.v0.xyz, tri.v1.xyz, tri.v2.xyz, &result);
                
                if (sqDist < minSqDist)
                {
                    minSqDist = sqDist;
                    bestResult = result;
                    bestTriIdx = triIdx;
                }
            }
        }
        else  // Internal node
        {
            // Safety: check stack overflow and child validity before pushing
            if (stackPtr < 62 && node.rightChild >= 0 && node.rightChild < nodeCount)
            {
                stack[stackPtr++] = node.rightChild;
            }
            if (stackPtr < 63 && node.leftChild >= 0 && node.leftChild < nodeCount)
            {
                stack[stackPtr++] = node.leftChild;
            }
        }
    }
    
    if (bestTriIdx < 0)
    {
        return FLT_MAX;
    }
    
    // Compute sign using pseudo-normal
    struct MeshTriangleGPU bestTri = triangles[bestTriIdx];
    // Note: vertex indices are stored with stride 4 (3 indices + 1 padding per triangle)
    int idx0 = vertexIndices[bestTriIdx * 4 + 0];
    int idx1 = vertexIndices[bestTriIdx * 4 + 1];
    int idx2 = vertexIndices[bestTriIdx * 4 + 2];
    
    float3 pseudoNormal = computePseudoNormal(&bestResult,
        bestTri.v0.xyz, bestTri.v1.xyz, bestTri.v2.xyz,
        vertexNormals, idx0, idx1, idx2);
    
    float3 toQuery = pos - bestResult.closestPoint;
    float sign = (dot(toQuery, pseudoNormal) < 0.0f) ? -1.0f : 1.0f;
    
    return sign * sqrt(minSqDist);
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
    
    __global struct MeshBVHNodeGPU const* nodes = 
        (__global struct MeshBVHNodeGPU const*)(data + nodesOffset);
    __global struct MeshTriangleGPU const* triangles = 
        (__global struct MeshTriangleGPU const*)(data + trianglesOffset);
    
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
        
        struct MeshBVHNodeGPU node = nodes[nodeIdx];
        
        float boxSqDist = sqDistanceToAABB(pos, node.bboxMin.xyz, node.bboxMax.xyz);
        if (boxSqDist >= minSqDist)
        {
            continue;
        }
        
        if (node.leftChild == -1)
        {
            int primEnd = node.primStart + node.primCount;
            if (node.primStart < 0 || primEnd > triCount)
            {
                continue;
            }
            
            for (int i = 0; i < node.primCount; ++i)
            {
                int triIdx = node.primStart + i;
                struct MeshTriangleGPU tri = triangles[triIdx];
                
                struct ClosestPointResult result;
                float sqDist = sqTriangleWithClosestPoint(pos, 
                    tri.v0.xyz, tri.v1.xyz, tri.v2.xyz, &result);
                
                minSqDist = fmin(minSqDist, sqDist);
            }
        }
        else
        {
            if (stackPtr < 62 && node.rightChild >= 0 && node.rightChild < nodeCount)
            {
                stack[stackPtr++] = node.rightChild;
            }
            if (stackPtr < 63 && node.leftChild >= 0 && node.leftChild < nodeCount)
            {
                stack[stackPtr++] = node.leftChild;
            }
        }
    }
    
    return sqrt(minSqDist);
}

#endif // MESH_SDF_CL
