// Structures matching the host side
typedef struct __attribute__((packed))
{
    ulong mortonCode;
    uint edgeMask;
    uint internalMask;
    uint vertexStartIndex;
    uchar vertexCount;
    uchar depth;
    uchar padding[2];
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

/// Get the per-axis cell extents at a given depth
float3 getCellExtent(float3 bboxMin, float3 bboxMax, uint depth)
{
    float3 extent = bboxMax - bboxMin;
    float scale = 1.0f / (float)(1u << depth);
    return extent * scale;
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
    uint depth = node.depth;
    float3 cellExtent = getCellExtent(bboxMin, bboxMax, depth);
    
    float3 cellMin = bboxMin + (float3)((float)coords.x, (float)coords.y, (float)coords.z) * cellExtent;
    float3 cellMax = cellMin + cellExtent;
    float3 cellCenter = (cellMin + cellMax) * 0.5f;
    
    // Sample SDF at 8 corners
    float cornerValues[8];
    for (int corner = 0; corner < 8; corner++) {
        int cx = (corner >> 0) & 1;
        int cy = (corner >> 1) & 1;
        int cz = (corner >> 2) & 1;
        
        float3 cornerPos = cellMin + (float3)((float)cx, (float)cy, (float)cz) * cellExtent;
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
        
        float3 p0 = cellMin + (float3)((float)cx0, (float)cy0, (float)cz0) * cellExtent;
        float3 p1 = cellMin + (float3)((float)cx1, (float)cy1, (float)cz1) * cellExtent;
        
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
    const uint currentDepth,
    const uint maxDepth,
    const uint initialDepth,
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
    float3 cellExtent = getCellExtent(bboxMin, bboxMax, currentDepth);
    float3 parentMin = bboxMin + (float3)((float)parentCoords.x, (float)parentCoords.y, (float)parentCoords.z) * cellExtent;
    
    // Subdivide into 8 children
    float3 childExtent = cellExtent * 0.5f;
    
    for (int childIdx = 0; childIdx < 8; childIdx++) {
        // Compute child offset (0-7 maps to 3D grid)
        int dx = (childIdx >> 0) & 1;
        int dy = (childIdx >> 1) & 1;
        int dz = (childIdx >> 2) & 1;
        
        ulong childX = (parentCoords.x << 1) | dx;
        ulong childY = (parentCoords.y << 1) | dy;
        ulong childZ = (parentCoords.z << 1) | dz;
        
        ulong childMorton = encodeMorton3(childX, childY, childZ);
        
        float3 childMin = parentMin + (float3)((float)dx, (float)dy, (float)dz) * childExtent;
        float3 childMax = childMin + childExtent;
        
        // Sample SDF at 8 corners of child cell using model evaluation
        float cornerValues[8];
        uint signMask = 0;
        
        for (int corner = 0; corner < 8; corner++) {
            int cx = (corner >> 0) & 1;
            int cy = (corner >> 1) & 1;
            int cz = (corner >> 2) & 1;
            
            float3 cornerPos = childMin + (float3)((float)cx, (float)cy, (float)cz) * childExtent;
            
            // Evaluate SDF using the model function
            float4 sdfResult = model(cornerPos, PASS_PAYLOAD_ARGS);
            float sdfValue = sdfResult.w - isoValue;
            cornerValues[corner] = sdfValue;
            
            if (sdfValue < 0.0f) {
                signMask |= (1 << corner);
            }
        }
        
        bool const containsSurface = (signMask != 0) && (signMask != 0xFF);
        bool const forceSubdivision = currentDepth < initialDepth;

        // Check if child contains surface (sign changes) or needs forced subdivision
        if (forceSubdivision || containsSurface) {
            // Child contains surface or is part of forced refinement, add to output
            int outputIdx = atomic_inc(outputCount);
            
            OctreeNode child;
            child.mortonCode = childMorton;
            child.edgeMask = 0; // Will be computed below
            child.internalMask = signMask; // Store which corners are inside (negative SDF)
            child.vertexStartIndex = 0;
            child.vertexCount = 0;
            child.depth = (uchar)min((uint)255, currentDepth + 1U);
            child.padding[0] = 0;
            child.padding[1] = 0;
            
            // Compute edge mask (which of 12 edges cross surface)
            // Edge numbering: 0-3 bottom face (X,Y,X,Y), 4-7 top face, 8-11 vertical
            const int edgeCorners[12][2] = {
                {0,1}, {1,3}, {3,2}, {2,0},  // Bottom face
                {4,5}, {5,7}, {7,6}, {6,4},  // Top face
                {0,4}, {1,5}, {3,7}, {2,6}   // Vertical edges
            };
            
            if (containsSurface) {
                for (int e = 0; e < 12; e++) {
                    int c0 = edgeCorners[e][0];
                    int c1 = edgeCorners[e][1];
                    
                    // Check for sign change
                    if ((cornerValues[c0] < 0.0f) != (cornerValues[c1] < 0.0f)) {
                        child.edgeMask |= (1 << e);
                    }
                }
            }
            
            outputNodes[outputIdx] = child;
        }
    }
}

// ============================================================================
// GPU-Based Watertight Index Generation
// ============================================================================
//
// Strategy for watertightness:
// 1. Each edge with a sign change generates exactly one quad (2 triangles)
// 2. A quad is formed by 4 cells sharing that edge
// 3. To avoid duplicates, only the cell with the smallest Morton code among
//    the 4 neighbors "owns" the edge and emits the quad
// 4. Cells are sorted by Morton code, enabling binary search for neighbors
//
// For uniform grids (all cells at same depth):
// - Edge 0 (X-axis at min Y, min Z) shared by: (x,y,z), (x,y-1,z), (x,y,z-1), (x,y-1,z-1)
// - Edge 3 (Y-axis at min X, min Z) shared by: (x,y,z), (x-1,y,z), (x,y,z-1), (x-1,y,z-1)
// - Edge 8 (Z-axis at min X, min Y) shared by: (x,y,z), (x-1,y,z), (x,y-1,z), (x-1,y-1,z)
// ============================================================================

/// Binary search to find a node by Morton code (for uniform-depth octrees)
/// Returns the index of the node, or -1 if not found
int findNodeByMorton(__global const OctreeNode* nodes, int numNodes, ulong targetMorton)
{
    int left = 0;
    int right = numNodes - 1;
    
    while (left <= right)
    {
        int mid = (left + right) / 2;
        ulong midMorton = nodes[mid].mortonCode;
        
        if (midMorton == targetMorton)
        {
            return mid;
        }
        else if (midMorton < targetMorton)
        {
            left = mid + 1;
        }
        else
        {
            right = mid - 1;
        }
    }
    
    return -1;
}

/// Count quads per cell (first pass for prefix sum)
/// We use edges at the MAX corner (6, 5, 10) because for these edges,
/// the current cell has the smallest Morton code among the 4 cells sharing it.
/// This is because the neighbors are at +Y, +Z, or both, which have larger Morton codes.
///
/// Edge ownership rule: emit a quad if:
/// 1. The edge crosses the surface (edgeMask bit set)
/// 2. All 4 cells around this edge exist (have surface)
__kernel void count_quads(
    __global const OctreeNode* nodes,
    __global int* quadCounts,
    const int numNodes,
    const uint maxCoord)  // Maximum valid coordinate (2^depth - 1)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;
    
    OctreeNode node = nodes[id];
    
    // Skip cells without surface
    if (node.edgeMask == 0)
    {
        quadCounts[id] = 0;
        return;
    }
    
    ulong3 coords = decodeMorton3(node.mortonCode);
    
    int count = 0;
    
    // Edge 6: X-axis at (y=max, z=max), corners 7-6: (1,1,1)-(0,1,1)
    // Shared by: (x,y,z), (x,y+1,z), (x,y,z+1), (x,y+1,z+1)
    // Current cell has smallest Morton code since neighbors have larger y and/or z
    if (node.edgeMask & (1 << 6))
    {
        // Check if neighbors exist within grid bounds
        if (coords.y < maxCoord && coords.z < maxCoord)
        {
            ulong nMorton1 = encodeMorton3(coords.x, coords.y + 1, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y, coords.z + 1);
            ulong nMorton3 = encodeMorton3(coords.x, coords.y + 1, coords.z + 1);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            // All 3 neighbors must exist (contain surface)
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                count++;
            }
        }
    }
    
    // Edge 5: Y-axis at (x=max, z=max), corners 5-7: (1,0,1)-(1,1,1)
    // Shared by: (x,y,z), (x+1,y,z), (x,y,z+1), (x+1,y,z+1)
    if (node.edgeMask & (1 << 5))
    {
        if (coords.x < maxCoord && coords.z < maxCoord)
        {
            ulong nMorton1 = encodeMorton3(coords.x + 1, coords.y, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y, coords.z + 1);
            ulong nMorton3 = encodeMorton3(coords.x + 1, coords.y, coords.z + 1);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                count++;
            }
        }
    }
    
    // Edge 10: Z-axis at (x=max, y=max), corners 3-7: (1,1,0)-(1,1,1)
    // Shared by: (x,y,z), (x+1,y,z), (x,y+1,z), (x+1,y+1,z)
    if (node.edgeMask & (1 << 10))
    {
        if (coords.x < maxCoord && coords.y < maxCoord)
        {
            ulong nMorton1 = encodeMorton3(coords.x + 1, coords.y, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y + 1, coords.z);
            ulong nMorton3 = encodeMorton3(coords.x + 1, coords.y + 1, coords.z);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                count++;
            }
        }
    }
    
    // Each quad = 2 triangles = 6 indices
    quadCounts[id] = count * 6;
}

/// Emit indices for quads (second pass after prefix sum)
/// Must use the same edges as count_quads (6, 5, 10)
/// Winding order is determined by the sign of corner 7 (the max corner)
__kernel void emit_indices(
    __global const OctreeNode* nodes,
    __global const int* vertexOffsets,
    __global const int* indexOffsets,
    __global uint* outputIndices,
    const int numNodes,
    const uint maxCoord)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;

    OctreeNode node = nodes[id];
    
    // Skip cells without surface
    if (node.edgeMask == 0) return;
    
    ulong3 coords = decodeMorton3(node.mortonCode);
    
    int writeOffset = indexOffsets[id];
    
    // Corner 7 is at (1,1,1), bit 7 in internalMask
    bool corner7Inside = (node.internalMask & (1 << 7)) != 0;
    
    // Edge 6: X-axis at (y=max, z=max), corners 7-6: (1,1,1)-(0,1,1)
    // Shared by: (x,y,z), (x,y+1,z), (x,y,z+1), (x,y+1,z+1)
    if (node.edgeMask & (1 << 6))
    {
        if (coords.y < maxCoord && coords.z < maxCoord)
        {
            ulong nMorton1 = encodeMorton3(coords.x, coords.y + 1, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y, coords.z + 1);
            ulong nMorton3 = encodeMorton3(coords.x, coords.y + 1, coords.z + 1);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                uint v0 = (uint)vertexOffsets[id];
                uint v1 = (uint)vertexOffsets[nIdx1];
                uint v2 = (uint)vertexOffsets[nIdx2];
                uint v3 = (uint)vertexOffsets[nIdx3];
                
                // Emit 2 triangles for quad
                // Winding depends on whether corner 7 is inside
                if (corner7Inside)
                {
                    // Corner 7 inside: wind CCW when viewed from +X
                    outputIndices[writeOffset + 0] = v0;
                    outputIndices[writeOffset + 1] = v2;
                    outputIndices[writeOffset + 2] = v1;
                    
                    outputIndices[writeOffset + 3] = v1;
                    outputIndices[writeOffset + 4] = v2;
                    outputIndices[writeOffset + 5] = v3;
                }
                else
                {
                    // Corner 7 outside: wind CW when viewed from +X
                    outputIndices[writeOffset + 0] = v0;
                    outputIndices[writeOffset + 1] = v1;
                    outputIndices[writeOffset + 2] = v2;
                    
                    outputIndices[writeOffset + 3] = v1;
                    outputIndices[writeOffset + 4] = v3;
                    outputIndices[writeOffset + 5] = v2;
                }
                
                writeOffset += 6;
            }
        }
    }
    
    // Edge 5: Y-axis at (x=max, z=max), corners 5-7: (1,0,1)-(1,1,1)
    // Shared by: (x,y,z), (x+1,y,z), (x,y,z+1), (x+1,y,z+1)
    // DIFFERENT from edges 6 and 10:
    // - Base winding (v0,v1,v2) with v1=+X, v2=+Z gives (+X)×(+Z) = -Y
    // - When corner7Inside, we want -Y normal (away from corner7 at max Y)
    // - So use base winding (v0,v1,v2) when corner7Inside (NO swap)
    // - When !corner7Inside, swap to get +Y normal
    if (node.edgeMask & (1 << 5))
    {
        if (coords.x < maxCoord && coords.z < maxCoord)
        {
            ulong nMorton1 = encodeMorton3(coords.x + 1, coords.y, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y, coords.z + 1);
            ulong nMorton3 = encodeMorton3(coords.x + 1, coords.y, coords.z + 1);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                uint v0 = (uint)vertexOffsets[id];
                uint v1 = (uint)vertexOffsets[nIdx1];
                uint v2 = (uint)vertexOffsets[nIdx2];
                uint v3 = (uint)vertexOffsets[nIdx3];
                
                // INVERTED logic compared to edges 6 and 10
                if (corner7Inside)
                {
                    // Need -Y: base winding gives -Y, so NO swap
                    outputIndices[writeOffset + 0] = v0;
                    outputIndices[writeOffset + 1] = v1;
                    outputIndices[writeOffset + 2] = v2;
                    
                    outputIndices[writeOffset + 3] = v1;
                    outputIndices[writeOffset + 4] = v3;
                    outputIndices[writeOffset + 5] = v2;
                }
                else
                {
                    // Need +Y: swap to get +Y
                    outputIndices[writeOffset + 0] = v0;
                    outputIndices[writeOffset + 1] = v2;
                    outputIndices[writeOffset + 2] = v1;
                    
                    outputIndices[writeOffset + 3] = v1;
                    outputIndices[writeOffset + 4] = v2;
                    outputIndices[writeOffset + 5] = v3;
                }
                
                writeOffset += 6;
            }
        }
    }
    
    // Edge 10: Z-axis at (x=max, y=max), corners 3-7: (1,1,0)-(1,1,1)
    // Shared by: (x,y,z), (x+1,y,z), (x,y+1,z), (x+1,y+1,z)
    if (node.edgeMask & (1 << 10))
    {
        if (coords.x < maxCoord && coords.y < maxCoord)
        {
            ulong nMorton1 = encodeMorton3(coords.x + 1, coords.y, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y + 1, coords.z);
            ulong nMorton3 = encodeMorton3(coords.x + 1, coords.y + 1, coords.z);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                uint v0 = (uint)vertexOffsets[id];
                uint v1 = (uint)vertexOffsets[nIdx1];
                uint v2 = (uint)vertexOffsets[nIdx2];
                uint v3 = (uint)vertexOffsets[nIdx3];
                
                if (corner7Inside)
                {
                    outputIndices[writeOffset + 0] = v0;
                    outputIndices[writeOffset + 1] = v2;
                    outputIndices[writeOffset + 2] = v1;
                    
                    outputIndices[writeOffset + 3] = v1;
                    outputIndices[writeOffset + 4] = v2;
                    outputIndices[writeOffset + 5] = v3;
                }
                else
                {
                    outputIndices[writeOffset + 0] = v0;
                    outputIndices[writeOffset + 1] = v1;
                    outputIndices[writeOffset + 2] = v2;
                    
                    outputIndices[writeOffset + 3] = v1;
                    outputIndices[writeOffset + 4] = v3;
                    outputIndices[writeOffset + 5] = v2;
                }
                
                writeOffset += 6;
            }
        }
    }
}
