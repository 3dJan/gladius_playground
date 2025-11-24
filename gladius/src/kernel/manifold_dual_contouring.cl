// Structures matching the host side
typedef struct __attribute__((packed))
{
    ulong mortonCode;
    uint edgeMask;
    uint internalMask;
    uint vertexStartIndex;
    uchar vertexCount;
    uchar padding[3];
} OctreeNode;

typedef struct __attribute__((packed))
{
    float4 position; // w is padding
    float4 normal;   // w is padding
} Vertex;

// Helper functions for Morton codes

/// Spreads bits of a 21-bit number to every third bit position
ulong expandBits(ulong v)
{
    v = (v | (v << 32)) & 0x1f00000000ffffUL;
    v = (v | (v << 16)) & 0x1f0000ff0000ffUL;
    v = (v | (v << 8))  & 0x100f00f00f00f00fUL;
    v = (v | (v << 4))  & 0x10c30c30c30c30c3UL;
    v = (v | (v << 2))  & 0x1249249249249249UL;
    return v;
}

/// Encodes 3D coordinates into Morton code (Z-order curve)
ulong encodeMorton3(ulong x, ulong y, ulong z)
{
    return (expandBits(z) << 2) | (expandBits(y) << 1) | expandBits(x);
}

/// Compacts every third bit
ulong compactBits(ulong v)
{
    v &= 0x1249249249249249UL;
    v = (v | (v >> 2))  & 0x10c30c30c30c30c3UL;
    v = (v | (v >> 4))  & 0x100f00f00f00f00fUL;
    v = (v | (v >> 8))  & 0x1f0000ff0000ffUL;
    v = (v | (v >> 16)) & 0x1f00000000ffffUL;
    v = (v | (v >> 32)) & 0x1fffffUL;
    return v;
}

/// Decodes Morton code back to 3D coordinates
ulong3 decodeMorton3(ulong m)
{
    ulong x = compactBits(m);
    ulong y = compactBits(m >> 1);
    ulong z = compactBits(m >> 2);
    return (ulong3)(x, y, z);
}

/// Get the depth level from a Morton code (number of subdivisions)
uint getMortonDepth(ulong mortonCode)
{
    // Find the highest set bit and divide by 3 (since we use 3 bits per level)
    uint depth = 0;
    ulong temp = mortonCode;
    while (temp > 0) {
        temp >>= 3;
        depth++;
    }
    return depth > 0 ? depth - 1 : 0;
}

/// Get the cell size at a given depth
float getCellSize(float rootSize, uint depth)
{
    return rootSize / (float)(1 << depth);
}

/// Convert Morton code to world-space position (cell center)
float3 mortonToWorldPos(ulong mortonCode, float3 bboxMin, float rootSize)
{
    ulong3 coords = decodeMorton3(mortonCode);
    uint depth = getMortonDepth(mortonCode);
    float cellSize = getCellSize(rootSize, depth);
    
    // Cell corner position
    float3 pos = bboxMin + (float3)((float)coords.x, (float)coords.y, (float)coords.z) * cellSize;
    // Return cell center
    return pos + cellSize * 0.5f;
}

// Kernel to count vertices per cell
// For manifold DC, each cell typically generates 1 vertex per connected component
// For now, we'll use a simple approach: 1 vertex if the cell contains the surface
__kernel void count_vertices(
    __global OctreeNode* nodes,
    __global int* countBuffer,
    const int numNodes)
{
    int i = get_global_id(0);
    if (i >= numNodes) return;
    
    OctreeNode node = nodes[i];
    
    // If edgeMask is 0, no edges cross the surface -> no vertex
    // If edgeMask != 0, we have surface crossing -> generate 1 vertex
    // (In full manifold DC, we'd analyze topology for multiple vertices)
    countBuffer[i] = (node.edgeMask != 0) ? 1 : 0;
}

// QEF (Quadratic Error Function) Solver
// Simplified version using mass point (average of edge intersections)
// For production, consider SVD-based solver

typedef struct
{
    float3 position;
    float3 normal;
    float error;
} QefResult;

/// Find intersection point on an edge using linear interpolation
float3 findEdgeIntersection(float3 p0, float3 p1, float v0, float v1)
{
    // Linear interpolation to find zero crossing
    if (fabs(v1 - v0) < 1e-6f) {
        return (p0 + p1) * 0.5f;
    }
    float t = -v0 / (v1 - v0);
    t = clamp(t, 0.0f, 1.0f);
    return p0 + t * (p1 - p0);
}

/// Compute gradient (normal) at a point using finite differences
float3 computeGradient(float3 pos, PAYLOAD_ARGS)
{
    // Central difference gradient using model evaluation
    const float h = 0.001f;
    const float h2 = 2.0f * h;
    
    const float3 posXp = pos + (float3)(h, 0.0f, 0.0f);
    const float3 posXn = pos - (float3)(h, 0.0f, 0.0f);
    const float3 posYp = pos + (float3)(0.0f, h, 0.0f);
    const float3 posYn = pos - (float3)(0.0f, h, 0.0f);
    const float3 posZp = pos + (float3)(0.0f, 0.0f, h);
    const float3 posZn = pos - (float3)(0.0f, 0.0f, h);
    
    const float sdfXp = model(posXp, PASS_PAYLOAD_ARGS).w;
    const float sdfXn = model(posXn, PASS_PAYLOAD_ARGS).w;
    const float sdfYp = model(posYp, PASS_PAYLOAD_ARGS).w;
    const float sdfYn = model(posYn, PASS_PAYLOAD_ARGS).w;
    const float sdfZp = model(posZp, PASS_PAYLOAD_ARGS).w;
    const float sdfZn = model(posZn, PASS_PAYLOAD_ARGS).w;
    
    float3 gradient;
    gradient.x = (sdfXp - sdfXn) / h2;
    gradient.y = (sdfYp - sdfYn) / h2;
    gradient.z = (sdfZp - sdfZn) / h2;
    
    const float gradLengthSq = dot(gradient, gradient);
    if (gradLengthSq > 1e-8f) {
        gradient /= sqrt(gradLengthSq);
    } else {
        gradient = (float3)(0.0f, 1.0f, 0.0f);
    }
    
    return gradient;
}

/// Solve QEF using mass point method (average of intersections)
QefResult solveQefMassPoint(float3* intersections, float3* normals, int count, float3 cellMin, float3 cellMax)
{
    QefResult result;
    
    if (count == 0) {
        // No intersections, return cell center
        result.position = (cellMin + cellMax) * 0.5f;
        result.normal = (float3)(0.0f, 0.0f, 1.0f);
        result.error = 0.0f;
        return result;
    }
    
    // Mass point: average of all intersection points
    float3 avgPos = (float3)(0.0f, 0.0f, 0.0f);
    float3 avgNormal = (float3)(0.0f, 0.0f, 0.0f);
    
    for (int i = 0; i < count; i++) {
        avgPos += intersections[i];
        avgNormal += normals[i];
    }
    
    avgPos /= (float)count;
    avgNormal = normalize(avgNormal);
    
    // Clamp to cell bounds
    result.position = clamp(avgPos, cellMin, cellMax);
    result.normal = avgNormal;
    result.error = 0.0f; // Could compute actual QEF error here
    
    return result;
}

// Kernel to emit vertices
// Each cell with surface intersection generates vertices based on QEF solving
__kernel void emit_vertices(
    __global OctreeNode* nodes,
    __global int* offsets,
    __global Vertex* outputVertices,
    const int numNodes,
    const float3 bboxMin,
    const float3 bboxMax,
    const float rootSize,
    PAYLOAD_ARGS,
    const float isoValue)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;

    OctreeNode node = nodes[id];
    int startIndex = offsets[id];
    
    // Skip cells with no surface intersection
    if (node.edgeMask == 0) return;
    
    // Get cell bounds
    ulong3 coords = decodeMorton3(node.mortonCode);
    uint depth = getMortonDepth(node.mortonCode);
    float cellSize = getCellSize(rootSize, depth);
    
    float3 cellMin = bboxMin + (float3)((float)coords.x, (float)coords.y, (float)coords.z) * cellSize;
    float3 cellMax = cellMin + cellSize;
    float3 cellCenter = (cellMin + cellMax) * 0.5f;
    
    // Sample SDF at 8 corners
    float cornerValues[8];
    for (int corner = 0; corner < 8; corner++) {
        int cx = (corner >> 0) & 1;
        int cy = (corner >> 1) & 1;
        int cz = (corner >> 2) & 1;
        
        float3 cornerPos = cellMin + (float3)((float)cx, (float)cy, (float)cz) * cellSize;
        float4 sdfResult = model(cornerPos, PASS_PAYLOAD_ARGS);
        cornerValues[corner] = sdfResult.w - isoValue;
    }
    
    // Find edge intersections and compute QEF data
    // Edge numbering: 0-3 bottom face, 4-7 top face, 8-11 vertical
    const int edgeCorners[12][2] = {
        {0,1}, {1,3}, {3,2}, {2,0},  // Bottom face
        {4,5}, {5,7}, {7,6}, {6,4},  // Top face
        {0,4}, {1,5}, {3,7}, {2,6}   // Vertical edges
    };
    
    float3 intersections[12];
    float3 normals[12];
    int intersectionCount = 0;
    
    for (int e = 0; e < 12; e++) {
        if ((node.edgeMask & (1 << e)) == 0) continue;
        
        int c0 = edgeCorners[e][0];
        int c1 = edgeCorners[e][1];
        
        // Get corner positions
        int cx0 = (c0 >> 0) & 1, cy0 = (c0 >> 1) & 1, cz0 = (c0 >> 2) & 1;
        int cx1 = (c1 >> 0) & 1, cy1 = (c1 >> 1) & 1, cz1 = (c1 >> 2) & 1;
        
        float3 p0 = cellMin + (float3)((float)cx0, (float)cy0, (float)cz0) * cellSize;
        float3 p1 = cellMin + (float3)((float)cx1, (float)cy1, (float)cz1) * cellSize;
        
        // Find intersection point
        float3 intersection = findEdgeIntersection(p0, p1, cornerValues[c0], cornerValues[c1]);
        intersections[intersectionCount] = intersection;
        
        // Compute normal at intersection
        normals[intersectionCount] = computeGradient(intersection, PASS_PAYLOAD_ARGS);
        intersectionCount++;
    }
    
    // Solve QEF using mass point method
    QefResult qef = solveQefMassPoint(intersections, normals, intersectionCount, cellMin, cellMax);
    
    // Output vertex
    Vertex v;
    v.position = (float4)(qef.position.x, qef.position.y, qef.position.z, 1.0f);
    v.normal = (float4)(qef.normal.x, qef.normal.y, qef.normal.z, 0.0f);
    
    outputVertices[startIndex] = v;
}

// Octree construction kernel
// Build octree by subdividing cells that contain the surface
__kernel void construct_octree_level(
    __global OctreeNode* inputNodes,
    __global OctreeNode* outputNodes,
    __global int* outputCount,
    const int numInputNodes,
    const float3 bboxMin,
    const float3 bboxMax,
    const float rootSize,
    const uint currentDepth,
    const uint maxDepth,
    PAYLOAD_ARGS,
    const float isoValue)
{
    int id = get_global_id(0);
    if (id >= numInputNodes) return;
    
    OctreeNode parent = inputNodes[id];
    
    // Don't subdivide if we're at max depth
    if (currentDepth >= maxDepth) return;
    
    // Get parent cell bounds
    ulong3 parentCoords = decodeMorton3(parent.mortonCode);
    float cellSize = getCellSize(rootSize, currentDepth);
    float3 parentMin = bboxMin + (float3)((float)parentCoords.x, (float)parentCoords.y, (float)parentCoords.z) * cellSize;
    
    // Subdivide into 8 children
    float childSize = cellSize * 0.5f;
    
    for (int childIdx = 0; childIdx < 8; childIdx++) {
        // Compute child offset (0-7 maps to 3D grid)
        int dx = (childIdx >> 0) & 1;
        int dy = (childIdx >> 1) & 1;
        int dz = (childIdx >> 2) & 1;
        
        ulong childX = (parentCoords.x << 1) | dx;
        ulong childY = (parentCoords.y << 1) | dy;
        ulong childZ = (parentCoords.z << 1) | dz;
        
        ulong childMorton = encodeMorton3(childX, childY, childZ);
        
        float3 childMin = parentMin + (float3)((float)dx, (float)dy, (float)dz) * childSize;
        float3 childMax = childMin + childSize;
        
        // Sample SDF at 8 corners of child cell using model evaluation
        float cornerValues[8];
        uint signMask = 0;
        
        for (int corner = 0; corner < 8; corner++) {
            int cx = (corner >> 0) & 1;
            int cy = (corner >> 1) & 1;
            int cz = (corner >> 2) & 1;
            
            float3 cornerPos = childMin + (float3)((float)cx, (float)cy, (float)cz) * childSize;
            
            // Evaluate SDF using the model function
            float4 sdfResult = model(cornerPos, PASS_PAYLOAD_ARGS);
            float sdfValue = sdfResult.w - isoValue;
            cornerValues[corner] = sdfValue;
            
            if (sdfValue < 0.0f) {
                signMask |= (1 << corner);
            }
        }
        
        // Check if child contains surface (sign changes)
        // If all corners have same sign, no surface intersection
        if (signMask != 0 && signMask != 0xFF) {
            // Child contains surface, add to output
            int outputIdx = atomic_inc(outputCount);
            
            OctreeNode child;
            child.mortonCode = childMorton;
            child.edgeMask = 0; // Will be computed later
            child.internalMask = 0;
            child.vertexStartIndex = 0;
            child.vertexCount = 0;
            
            // Compute edge mask (which of 12 edges cross surface)
            // Edge numbering: 0-3 bottom face (X,Y,X,Y), 4-7 top face, 8-11 vertical
            const int edgeCorners[12][2] = {
                {0,1}, {1,3}, {3,2}, {2,0},  // Bottom face
                {4,5}, {5,7}, {7,6}, {6,4},  // Top face
                {0,4}, {1,5}, {3,7}, {2,6}   // Vertical edges
            };
            
            for (int e = 0; e < 12; e++) {
                int c0 = edgeCorners[e][0];
                int c1 = edgeCorners[e][1];
                
                // Check for sign change
                if ((cornerValues[c0] < 0.0f) != (cornerValues[c1] < 0.0f)) {
                    child.edgeMask |= (1 << e);
                }
            }
            
            outputNodes[outputIdx] = child;
        }
    }
}

// Kernel to emit indices (triangles/quads)
// Generate mesh topology by connecting vertices from adjacent cells
__kernel void emit_indices(
    __global OctreeNode* nodes,
    __global int* vertexOffsets,
    __global uint* outputIndices,
    __global int* indexCount,
    const int numNodes,
    const float3 bboxMin,
    const float rootSize)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;

    OctreeNode node = nodes[id];
    
    // Skip cells without surface
    if (node.edgeMask == 0) return;
    
    // For each face of the cell, check if we need to emit a quad
    // A face generates a quad if:
    // 1. The face is on the boundary of the octree, OR
    // 2. The adjacent cell across this face has a different size (T-junction handling)
    // 3. Both cells have vertices
    
    // Simplified approach: emit quads for faces with surface-crossing edges
    // Face numbering: 0=X-, 1=X+, 2=Y-, 3=Y+, 4=Z-, 5=Z+
    
    ulong3 coords = decodeMorton3(node.mortonCode);
    uint depth = getMortonDepth(node.mortonCode);
    
    // Get this cell's vertex index
    int v0 = vertexOffsets[id];
    
    // TODO: For each face, find the neighbor cell and its vertex
    // Then emit a quad (2 triangles) connecting the 4 vertices
    // This requires a neighbor-finding function in the octree
    
    // Placeholder: just reserve space for potential indices
    // In a complete implementation, we'd:
    // 1. For each of 6 faces, check the 4 edges
    // 2. Find the 4 neighboring cells sharing those edges
    // 3. Get their vertex indices
    // 4. Emit 6 indices (2 triangles) forming a quad
    
    // For now, do nothing (indices will be empty)
}
