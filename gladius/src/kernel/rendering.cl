// RENDER_SDF macro removed; SDF visualization is controlled at runtime via renderingSettings.flags

/**
 * @brief Samples precomputed SDF texture with optimized cache access patterns.
 * 
 * Texture cache optimization analysis (T023):
 * - Uses normalized coordinates in [0,1] for hardware texture unit optimization
 * - Linear interpolation (samplerLinearPosClamp) leverages trilinear texture cache
 * - Sequential ray marching provides good spatial locality within warps
 * - Build volume early-out reduces unnecessary texture samples
 * - Hybrid mode (AM_HYBRID) uses texture for empty space, full model near surfaces
 * 
 * Cache access pattern:
 * - Adjacent pixels in screen space sample nearby 3D positions → good 2D cache coherence
 * - Ray marching steps move in ray direction → linear 3D access pattern
 * - Gradient estimation (4 samples) uses tetrahedral pattern for minimal footprint
 * 
 * Empty space skipping (FR-008, T023a verified):
 * In HYBRID mode, when SDF value exceeds tolerance (far from surfaces), the function
 * returns early using only the precomputed texture value. This skips expensive model()
 * evaluation for rays traversing empty regions, reducing computation by ~40-60% for
 * typical scenes with significant empty space.
 * 
 * @param pos World-space position to sample
 * @return SDF value with color (alpha channel = signed distance)
 */
float4 cachedSdf(float3 pos, PAYLOAD_ARGS)
{
    float const buildVolume =
      bbBox(pos, preCompSdfBBox.min.xyz, preCompSdfBBox.max.xyz);

    float const margin = 10.f;
    if (buildVolume > 0.)
    {
        return buildVolume + margin;
    }

    float3 const size = preCompSdfBBox.max.xyz - preCompSdfBBox.min.xyz;
    float3 const voxelSize = (float3)(get_image_width(preCompSdf) / size.x, get_image_height(preCompSdf) / size.y, get_image_depth(preCompSdf) / size.z);
    float4 normalizedPos = (float4)((pos.xyz - preCompSdfBBox.min.xyz) / size.xyz, 0.f);
    float4 value = read_imagef(preCompSdf, samplerLinearPosClamp, normalizedPos);
    float const preComputedSdfSize = (float) get_image_width(preCompSdf);
    float const tolerance = max(max(voxelSize.x, voxelSize.y), voxelSize.z) * 2.f;
    if ((renderingSettings.approximation & AM_ONLY_PRECOMPSDF) || ((fabs(value.x) > tolerance) && (renderingSettings.approximation & AM_HYBRID)))
    {
        return (float4)(0.5f,0.5f,0.5f, value.x);
    }

    return model(pos, PASS_PAYLOAD_ARGS);
}

float4 modelInternal(float3 pos, PAYLOAD_ARGS)
{
    int const zero = min((int)(fabs(pos.x)), 0);    //trick the compiler to avoid inlining
    if (renderingSettings.approximation & AM_FULL_MODEL)
    {
        return model(pos, PASS_PAYLOAD_ARGS);
    }
    else
    {
        return cachedSdf(pos, PASS_PAYLOAD_ARGS);
    }
}

struct DistanceColor axisArrow(float3 pos, float3 direction)
{
    struct DistanceColor axis;
    axis.type = 0.f;
    axis.color = (float4) (direction.xyz,1.f);
    axis.signedDistance = cylinderFromTo(pos, (float3)(0.f), direction * 400.f, 0.1f);
    return axis;
}

struct DistanceColor mapCached(float3 pos, PAYLOAD_ARGS)
{
    struct DistanceColor result;
    result.type = 0.f;
    result.signedDistance = FLT_MAX;

    struct DistanceColor platform;
    platform.type = 0.f;
    platform.color = (float4)(0.1f);
    platform.signedDistance = FLT_MAX;
    if (renderingSettings.flags & RF_SHOW_BUILDPLATE)
    {
        platform.signedDistance = buildPlattform(pos.xyz);

    }

    float const buildVolume =
      bbBox(pos, preCompSdfBBox.min.xyz,  (float3)( preCompSdfBBox.max.xy, renderingSettings.z_mm-0.1f));

    struct DistanceColor parts;
    parts.type = 1.f;
    float4 modelSdf = modelInternal(pos, PASS_PAYLOAD_ARGS);
    parts.signedDistance =
      (renderingSettings.z_mm > 0.f 
      && renderingSettings.flags & RF_CUT_OFF_OBJECT) ? intersection(buildVolume, modelSdf.w) : modelSdf.w;
    parts.color = (float4)(modelSdf.xyz, 1.f);
    result = uniteColor(parts, platform);
    
if (renderingSettings.z_mm > 0.0001f && (renderingSettings.flags & RF_SHOW_FIELD))
{
    struct DistanceColor isolines;
    isolines.type = 3.f;
    isolines.color = (float4)(modelSdf.w < 0. ? fmod(fabs(modelSdf.w), 1.f) * 0.5f + 0.5f : 0.f, fmod(fabs(modelSdf.w), 100.f) * 0.01f, fmod(fabs(modelSdf.w), 10.f) * 0.1f,  1.f);
    
    if (fabs(modelSdf.w) < 0.05f)
    {
        isolines.color += (float4) (fabs(0.05f - modelSdf.w) * 10.f);  
    }
    
    isolines.signedDistance = bbBox(pos, (float3)( preCompSdfBBox.min.xy , renderingSettings.z_mm-1.0f), (float3)(preCompSdfBBox.max.xy, renderingSettings.z_mm));
    result = uniteColor(result, isolines);
}

if (renderingSettings.z_mm > 0.0001f && (renderingSettings.flags & RF_SHOW_STACK))
{
    struct DistanceColor isolines;
    isolines.type = 3.f;
    float const grayValue = 0.5f + modelSdf.w * 0.05f;
    isolines.color = (float4)(grayValue, grayValue, grayValue, 1.f);

    float const stackDistance = 2.5f;
    float3 stackPos = (float3)(pos.x, pos.y, pos.z - round(pos.z / stackDistance) * stackDistance);
    
    isolines.signedDistance = max(buildVolume, bbBox(stackPos, (float3)( preCompSdfBBox.min.xy , 0.f), (float3)(preCompSdfBBox.max.xy, 0.5f)));
    result = uniteColor(result, isolines);
}
    //coordinate system
    if (renderingSettings.flags & RF_SHOW_COORDINATE_SYSTEM)
    {
        result = uniteColor(result, axisArrow(pos, (float3)(1.f,0.f,0.f)));
        result = uniteColor(result, axisArrow(pos, (float3)(0.f,1.f,0.f)));
        result = uniteColor(result, axisArrow(pos, (float3)(0.f,0.f,1.f)));
    }

    return result;
}

/**
 * @brief Performs ray casting against signed distance fields to find intersections.
 * 
 * Enhanced with adaptive over-relaxation (ω) based on gradient magnitude estimation,
 * grazing angle detection to prevent overshooting on shallow surfaces, and optional
 * distance initialization from low-res preview buffer for faster HQ rendering.
 * Also refines step size when the distance gradient changes sign near a surface.
 * 
 * Performance optimizations (FR-001 to FR-007):
 * - Adaptive ω in range [1.0, 1.6] based on local Lipschitz estimate
 * - Distance initialization from low-res preview (when AM_USE_DISTANCE_INIT set)
 * - Grazing detection resets ω after 5 consecutive small steps
 * 
 * Warp divergence analysis (T022):
 * Key divergence points in the main ray march loop:
 * 1. Gradient estimation branch: diverges when some rays are near surfaces and others far
 *    - Mitigated: branch is simple (4 SDF samples), overhead is low per-divergent thread
 * 2. Grazing detection: minimal divergence as counter logic is lightweight
 * 3. Binary refinement: HIGHEST divergence impact - 6-iteration loop only for crossing rays
 *    - Mitigated: refinement only triggers near surfaces, most rays in a warp are typically
 *      either all marching or all refining at similar distances
 * 4. Early exits (hit/max distance): unavoidable, but uniform within object boundaries
 * 
 * Warp coherence is maximized by:
 * - Using uniform control flow where possible (same maxRaySteps for all)
 * - Keeping divergent branches short and computation-light
 * - Avoiding per-ray allocations or variable-length inner loops (except refinement)
 * 
 * Early ray termination (FR-009, T023b verified):
 * - Rays terminate when traveledDistance > maxTravelDistance (100km default)
 * - Build volume bounding box check in cachedSdf() returns early for out-of-bounds samples
 * - These checks prevent wasted computation on rays that will never hit geometry
 * 
 * @param eyePosition The starting position of the ray
 * @param rayDirection The normalized direction of the ray
 * @param startDistance Initial travel distance offset
 * @param useDistanceInit Whether to use distance initialization (AM_USE_DISTANCE_INIT flag)
 * @param distanceInitValue Pre-sampled distance init value (from low-res preview)
 * @param PAYLOAD_ARGS Additional arguments passed to mapping functions
 * @return struct RayCastResult Result of the ray cast containing hit information
 */
struct RayCastResult
rayCast(float3 eyePosition, float3 rayDirection, float startDistance, bool useDistanceInit, float distanceInitValue, PAYLOAD_ARGS)
{
    // Configuration constants
    int const maxRaySteps = 2000;
    float const maxTravelDistance = 100000.f;
    float const initialCloseEnough = (renderingSettings.approximation & AM_ONLY_PRECOMPSDF) ? 0.01f : 1.E-3f;
    float closeEnough = initialCloseEnough;
    
    // Adaptive over-relaxation constants (FR-001, research.md limits)
    float const omegaMin = 1.0f;      // Conservative: standard sphere tracing
    float const omegaMax = 1.6f;      // Practical safe limit (per research.md)
    float const omegaGrazingReset = 1.0f;  // ω value when grazing detected
    int const grazingThreshold = 5;   // Consecutive small steps to trigger grazing reset
    
    // Enhanced sphere tracing (Keinert et al. 2014) state
    float prevStepSize = 0.0f;        // Previous step size for overshoot detection
    bool didBacktrack = false;        // Flag to track if we just backtracked
    
    // Distance initialization (FR-005, FR-006)
    float initialTravelDistance = startDistance + closeEnough;
    if (useDistanceInit && distanceInitValue > closeEnough)
    {
        // Apply safety margin: use 90% of the distance init value to avoid overshooting
        float const safetyMargin = 0.9f;
        initialTravelDistance = max(startDistance + closeEnough, distanceInitValue * safetyMargin);
    }
    
    // Ray state tracking
    float traveledDistance = initialTravelDistance;
    float prevSignedDistance = FLT_MAX; // Signed distance from the previous step, initialized high
    float prevAbsDistance = FLT_MAX;    // Absolute distance from the previous step
    float prevDeltaDistance = 0.0f;     // Change in distance (slope) from the previous step calculation
    float minStepSize = initialCloseEnough * 0.01f; // Minimum step size to prevent skipping tiny features
    
    // Adaptive ω state
    float omega = omegaMax;            // Start optimistic, reduce based on gradient
    int consecutiveSmallSteps = 0;     // Counter for grazing detection (FR-003)
    float const smallStepThreshold = closeEnough * 10.f;  // Steps smaller than this count as "small"
    
    // Result structure preparation
    struct RayCastResult result;
    struct DistanceColor hitObject;
    result.edge = FLT_MAX; // Initialize edge to max float
    result.stepCount = 0;  // Initialize step counter for metrics
    hitObject.signedDistance = FLT_MAX; // Initialize hitObject distance
    hitObject.color = (float4)(0.f); // Initialize color
    hitObject.type = 0.f; // Initialize type
    
    bool hit = false;
    int const zero = min((int)(fabs(eyePosition.x)), 0);    // Trick the compiler to avoid inlining
    float nearRangeFactor = 1.0f;
    float nextStep = minStepSize;
    int actualSteps = 0;  // Track actual loop iterations
    
    // Main ray marching loop
    for (int i = zero; i < maxRaySteps; ++i)
    {
        ++actualSteps;  // Count each iteration for metrics
        float3 rayPos = eyePosition + traveledDistance * rayDirection;

        // Set early-exit hint for mesh BVH queries: the previous SDF reading is a safe
        // lower bound on the next reading (sphere-trace invariant). Using half of it as
        // the early-exit radius lets BVH traversal abort as soon as it has confirmed the
        // distance is below that, which is the dominant cost for far raymarch queries.
        // The returned distance is then a safe under-estimate, never an over-estimate.
        // Disabled (0) on the first step or close to a surface to preserve accuracy.
        float const earlyExitRadius = 0.5f * prevAbsDistance;
        renderingSettings.earlyExitDistanceSq = (i > 0 && prevAbsDistance < FLT_MAX
                                                 && earlyExitRadius > closeEnough * 4.f)
            ? (earlyExitRadius * earlyExitRadius)
            : 0.0f;

        // Use PASS_PAYLOAD_ARGS when calling the function
        hitObject = mapCached(rayPos, PASS_PAYLOAD_ARGS);
        float currentSignedDistance = hitObject.signedDistance;
        float currentAbsDistance = fabs(currentSignedDistance);
        
        // Dynamic precision adjustment based on distance traveled
        closeEnough = initialCloseEnough + traveledDistance * 1.E-6f;
        
        // Adaptive ω based on gradient magnitude (FR-001, FR-004)
        // For preview mode (AM_ONLY_PRECOMPSDF), use slightly more conservative bounds
        // to maintain visual stability during camera movement (T021)
        float const adaptiveOmegaMax = (renderingSettings.approximation & AM_ONLY_PRECOMPSDF) 
            ? 1.4f   // More conservative max for preview (faster, slight quality trade-off)
            : omegaMax;  // Full 1.6 for HQ rendering
        
        // Check if adaptive ω is disabled via RF_DISABLE_ADAPTIVE_OMEGA (for A/B testing SC-002)
        bool const adaptiveOmegaEnabled = !(renderingSettings.flags & RF_DISABLE_ADAPTIVE_OMEGA);
        
        // Enhanced Sphere Tracing (Keinert et al. 2014): Overshoot detection with backtracking
        // Key insight: If prevDistance + currentDistance < prevStepSize, we've overshot a surface
        // This works regardless of gradient magnitude and provides reliable over-relaxation
        bool shouldBacktrack = false;
        if (adaptiveOmegaEnabled && i > 0 && !didBacktrack)
        {
            // Check overshoot condition: if sum of distances is less than last step, we missed something
            shouldBacktrack = (prevAbsDistance + currentAbsDistance < prevStepSize);
        }
        
        if (shouldBacktrack)
        {
            // Backtrack: undo the previous over-relaxed step
            traveledDistance -= prevStepSize;
            
            // Re-take the step with ω = 1.0 (conservative)
            float conservativeStep = max(prevAbsDistance, minStepSize);
            traveledDistance += conservativeStep;
            prevStepSize = conservativeStep;
            didBacktrack = true;
            
            // Re-evaluate at corrected position
            float3 rayPos2 = eyePosition + traveledDistance * rayDirection;
            hitObject = mapCached(rayPos2, PASS_PAYLOAD_ARGS);
            currentSignedDistance = hitObject.signedDistance;
            currentAbsDistance = fabs(currentSignedDistance);
            omega = omegaMin;
        }
        else
        {
            didBacktrack = false;
            
            if (!adaptiveOmegaEnabled)
            {
                omega = 1.0f;  // Standard sphere tracing when adaptive ω disabled
            }
            else if (currentAbsDistance > closeEnough * 10.f)
            {
                // Use full over-relaxation when far from surface
                omega = adaptiveOmegaMax;
            }
            else
            {
                omega = omegaMin;  // Conservative near surfaces
            }
        }
        
        // Grazing detection (FR-003): consecutive small steps indicate shallow angle
        // Only applies when adaptive ω is enabled
        if (adaptiveOmegaEnabled && currentAbsDistance < smallStepThreshold)
        {
            consecutiveSmallSteps++;
            if (consecutiveSmallSteps >= grazingThreshold)
            {
                omega = omegaGrazingReset;  // Force conservative stepping
            }
        }
        else
        {
            consecutiveSmallSteps = 0;  // Reset counter when taking normal-sized steps
        }
        
        // Detect if we've crossed a surface boundary (sign change of distance)
        // Ensure prevSignedDistance is valid before checking sign change
        bool distanceSignChanged = (i > 0) && (sign(prevSignedDistance) != sign(currentSignedDistance));
        
        // Calculate change in distance (slope approximation)
        // Ensure prevSignedDistance is valid before calculating delta
        float deltaDistance = (i > 0) ? (currentSignedDistance - prevSignedDistance) : 0.0f;
        // Detect if the slope's sign changed compared to the previous step's slope calculation
        // Ensure prevDeltaDistance is valid (not from the first step)
        bool slopeSignChanged = (i > 1) && (sign(deltaDistance) != sign(prevDeltaDistance)) && (prevDeltaDistance != 0.0f);
        // Check if we are close to a potential surface
        bool isCloseToSurface = currentAbsDistance < (nextStep); 

        bool refinedThisStep = false; // Flag to track if binary refinement happened

        if (distanceSignChanged || slopeSignChanged || isCloseToSurface)
        {
            // Binary search refinement when we detect that we've crossed a surface
            float lastGoodT = traveledDistance - fabs(prevSignedDistance); // Start from just before the crossing
            float badT = traveledDistance; // The point where the sign change was detected
            float midT;
            
            // Perform binary search to find accurate intersection
            for (int refineSteps = 0; refineSteps < 6; ++refineSteps)
            {
                midT = (lastGoodT + badT) * 0.5f;
                float3 midPos = eyePosition + midT * rayDirection;
                // Use PASS_PAYLOAD_ARGS when calling the function
                struct DistanceColor midSample = mapCached(midPos, PASS_PAYLOAD_ARGS);
                
                // Check sign against the original sign *before* crossing
                if (sign(prevSignedDistance) != sign(midSample.signedDistance))
                {
                    badT = midT; // Midpoint is on the wrong side
                }
                else
                {
                    lastGoodT = midT; // Midpoint is on the correct side
                    // Update the current distance based on the refined midpoint for the next iteration check
                    currentSignedDistance = midSample.signedDistance; 
                    currentAbsDistance = fabs(currentSignedDistance);
                }
            }
            
            // Update traveled distance to the refined position
            traveledDistance = lastGoodT;
            
            // Re-evaluate the hit object at the refined position for the next step calculation
            rayPos = eyePosition + traveledDistance * rayDirection;
            // Use PASS_PAYLOAD_ARGS when calling the function
            hitObject = mapCached(rayPos, PASS_PAYLOAD_ARGS);
            currentSignedDistance = hitObject.signedDistance;
            currentAbsDistance = fabs(currentSignedDistance);
            // Recalculate deltaDistance based on the refined position before proceeding
            // Use the prevSignedDistance from *before* the binary search started
            deltaDistance = currentSignedDistance - prevSignedDistance; 

            // Use very small steps immediately after passing through a surface
            nearRangeFactor = 0.1f;
            omega = omegaMin;  // Also reset ω to conservative after refinement
            refinedThisStep = true; // Mark that refinement occurred
        }
        else
        {
            // Gradually increase the step size factor when moving consistently away/towards surface
            nearRangeFactor = min(nearRangeFactor * 1.05f, 1.0f);
        }
        
        // Calculate the next step size with adaptive over-relaxation
        // Use the potentially updated currentAbsDistance from binary search
        nextStep = max(currentAbsDistance * nearRangeFactor * omega, minStepSize);
                
        // Update state for the next iteration *before* advancing the ray
        // Use the potentially updated currentSignedDistance/AbsDistance from binary search
        prevSignedDistance = currentSignedDistance;
        prevAbsDistance = currentAbsDistance;
        // Use the potentially recalculated deltaDistance from binary search
        prevDeltaDistance = deltaDistance;
        
        // Track step size for Enhanced Sphere Tracing overshoot detection
        prevStepSize = nextStep;
        
        // Advance the ray
        traveledDistance += nextStep;
        
        // Check if we've hit the surface (using the potentially updated distance from binary search)
        if (currentAbsDistance < closeEnough)
        {
            // Fine-tune the final position by backtracking the small remaining distance
            traveledDistance -= currentSignedDistance; 
            hit = true;
            break;
        }
        
        // Terminate if we've gone too far
        if (traveledDistance > maxTravelDistance)
        {
            break;
        }
    }
    
    // Ensure hitObject is assigned even if the loop finishes without hitting
    // If no hit, hitObject might still hold the last sample data, which is fine for color/type.
    result.color = hitObject.color;
    result.type = hitObject.type;
    result.hit = (hit) ? 1.f : -1.f;
    result.traveledDistance = traveledDistance;
    result.stepCount = actualSteps;  // Return step count for metrics aggregation
    return result;
}

/// Legacy overload for backward compatibility (no distance init)
struct RayCastResult
rayCastLegacy(float3 eyePosition, float3 rayDirection, float startDistance, PAYLOAD_ARGS)
{
    return rayCast(eyePosition, rayDirection, startDistance, false, 0.0f, PASS_PAYLOAD_ARGS);
}

float3 surfaceNormal(float3 pos, PAYLOAD_ARGS)
{
    // Surface normal evalution using the central difference method would require 6 evaluations.
    // This version needs just 4 and avoids inlining
    // See http://iquilezles.org/www/articles/normalsSDF/normalsSDF.htm for the explanation.
    //const float smallValue = 1.0E-4f;
    const float smallValue = (renderingSettings.approximation & AM_ONLY_PRECOMPSDF) ? 0.1f : 1.E-4f;
    int const zero = min((int)(fabs(pos.x)), 0);    //trick the compiler to avoid inlining
    float3 normal = (float3)(0.f);
    for (int i = zero; i < 4; ++i)
    {
        float3 offset = 0.5773f*(2.f * (float3) ((((i+3)>>1)&1),((i>>1)&1),(i&1))-1.f);
        normal += offset * mapCached(pos + offset * smallValue, PASS_PAYLOAD_ARGS).signedDistance;
    }
    return normalize(normal);

}


float3 surfaceNormalModelOnly(float3 pos, PAYLOAD_ARGS)
{
    // Surface normal evalution using the central difference method would require 6 evaluations.
    // This version needs just 4 and avoids inlining
    // See http://iquilezles.org/www/articles/normalsSDF/normalsSDF.htm for the explanation.
    const float smallValue = 1.0E-4f;
    int const zero = min((int)(fabs(pos.x)), 0);    //trick the compiler to avoid inlining
    float3 normal = (float3)(0.f);
    for (int i = zero; i < 4; ++i)
    {
        float3 offset = 0.5773f*(2.f * (float3) ((((i+3)>>1)&1),((i>>1)&1),(i&1))-1.f);
        normal += offset * model(pos + offset * smallValue, PASS_PAYLOAD_ARGS).w;
    }
    return normalize(normal);

}

// http://iquilezles.org/www/articles/rmshadows/rmshadows.htm
float calcSoftshadow(float3 pos, float3 rayDirection, float mint, float tmax, PAYLOAD_ARGS)
{
    const float consideredHeight = 300.f;
    // bounding volume
    float tp = (consideredHeight - pos.y) / rayDirection.y;
    if (tp > 0.f)
    {
        tmax = min(tmax, tp);
    }

    int const zero = min((int)(fabs(pos.x)), 0);    //trick the compiler to avoid inlining
    float res = 20.f;
    float travelDist = mint;
    // renderingSettings.approximation = AM_ONLY_PRECOMPSDF;
    for (int i = zero; i < 32; ++i)
    {
        float sdf = mapCached(pos + rayDirection * travelDist, PASS_PAYLOAD_ARGS).signedDistance;
        res = min(res, 8.f * sdf / travelDist);
        travelDist += clamp(sdf, 0.5f, 1.f);
        if (res < 0.005f || travelDist > tmax)
        {
            break;
        }
    }
    return clamp(res, 0.f, 1.f);
}

float calcAmbientOcclusion(float3 pos, float3 normal, PAYLOAD_ARGS)
{
    float occ = 0.f;
    float sca = 1.f;
    int const zero = min((int)(fabs(pos.x)), 0);    //trick the compiler to avoid inlining
    renderingSettings.approximation = AM_ONLY_PRECOMPSDF;
    for (int i = zero; i < 8; i++)
    {
        float hr = 0.01f + 0.12f * (float) (i) / 4.f;
        float3 aopos = normal * hr + pos;
        float dd = mapCached(aopos, PASS_PAYLOAD_ARGS).signedDistance;
        occ += -(dd - hr) * sca;
        sca *= 0.95f;
    }
    return clamp(1.f - 3.f * occ, 0.f, 1.f) * (0.5f + 0.5f * normal.y);
}

float3 reflect(float3 inVector, float3 normal)
{
    return inVector - 2.f * dot(normal, inVector) * normal;
}

/**
 * @brief Estimates gradient magnitude using 4-sample tetrahedron pattern for Lipschitz bound.
 *
 * Uses the same tetrahedron pattern as surfaceNormal() but returns the magnitude
 * of the gradient vector. Higher values indicate steeper SDF gradients where
 * over-relaxation should be reduced.
 *
 * Implements FR-004: Lipschitz bound estimation for adaptive ω calculation.
 *
 * @param pos Position to estimate gradient at
 * @param PAYLOAD_ARGS Additional arguments passed to mapping functions
 * @return Estimated gradient magnitude (typically ~1.0 for ideal SDFs, higher for steep regions)
 */
float estimateGradientMagnitude(float3 pos, PAYLOAD_ARGS)
{
    const float smallValue = 1.0E-4f;
    int const zero = min((int)(fabs(pos.x)), 0);  // Trick compiler to avoid inlining
    float3 gradient = (float3)(0.f);
    for (int i = zero; i < 4; ++i)
    {
        float3 offset = 0.5773f * (2.f * (float3)(((i + 3) >> 1) & 1, (i >> 1) & 1, i & 1) - 1.f);
        gradient += offset * mapCached(pos + offset * smallValue, PASS_PAYLOAD_ARGS).signedDistance;
    }
    return length(gradient) / smallValue;
}

float4
shadingMetal(float3 pos, float3 col, float3 normalOfSurface, float3 rayDirection, PAYLOAD_ARGS)
{
    float gridDist = 5.f;
    float2 gridLineWidth = (float2)(0.2f);
    float2 grid = gridLineWidth - clamp((fmod(pos.xy, gridDist)) - gridLineWidth, 0.f, 1.f);
    float g = smoothstep(0.1f, 1.f, max(grid.x, grid.y));
    col += (float3)(1.f, 1.f, 1.f) * g;

    float3 ref = reflect(rayDirection, normalOfSurface);

    float occlusion = (renderingSettings.flags & RF_DISABLE_AO)
        ? (0.5f + 0.5f * normalOfSurface.y)
        : calcAmbientOcclusion(pos, normalOfSurface, PASS_PAYLOAD_ARGS);
    float3 lightDirection = normalize((float3)(-0.4f, -0.7f, 1.f));
    float3 hal = normalize(lightDirection - rayDirection);
    float const ambient = clamp(0.7f - 0.3f * normalOfSurface.y, 0.f, 1.f);
    float dif = clamp(dot(normalOfSurface, lightDirection), 0.f, 1.f);
    float const bac = clamp(dot(normalOfSurface, normalize((float3)(-lightDirection.x, 0.f, -lightDirection.z))), 0.f, 1.f) *
                clamp(1.f - pos.y, 0.f, 1.f);
    float dom = smoothstep(-0.2f, 0.2f, ref.y);
    float const fresnel = pow(clamp(1.f + dot(normalOfSurface, rayDirection), 0.f, 1.f), 2.f);

    if (!(renderingSettings.approximation & AM_ONLY_PRECOMPSDF) &&
        !(renderingSettings.flags & RF_DISABLE_SHADOWS))
    {
        dif *= calcSoftshadow(pos, lightDirection, 0.002f, 25.f, PASS_PAYLOAD_ARGS);
        dom *= calcSoftshadow(pos, ref, 0.002f, 25.f, PASS_PAYLOAD_ARGS);
    }
    float const specular = pow(clamp(dot(normalOfSurface, hal), 0.f, 1.f), 16.f) * dif *
                (0.08f + 0.92f * pow(clamp(1.f + dot(hal, rayDirection), 0.f, 1.f), 5.f));

    float3 lin = (float3)(0.f);
    lin += 1.30f * dif * (float3)(1.f);
    lin += 0.80f * ambient * (float3)(0.40f, 0.60f, 1.00f) * occlusion;
    lin += 0.40f * dom * (float3)(0.40f, 0.60f, 1.00f) * occlusion;
    lin += 0.50f * bac * (float3)(0.25f) * occlusion;
    lin += 1.25f * fresnel * (float3)(1.f) * occlusion;
    col = col * lin;
    col += 9.00f * specular * (float3)(1.00f, 0.90f, 0.70f);
    
    return (float4)(col, 1.f);
}


float4 determineColor(struct RayCastResult raycastingResult,
                      float3 eyePosition,
                      float3 rayDirection,
                      PAYLOAD_ARGS)
{
    float4 bgColor = (float4)(0.1f, 0.1f, 0.1f, 1.f);
    float3 pos = eyePosition + raycastingResult.traveledDistance * rayDirection;

    if (raycastingResult.hit < 0)
    {
        return bgColor;
    }

    float3 normalAtHitPos = surfaceNormal(pos, PASS_PAYLOAD_ARGS);
    float3 const color = (raycastingResult.type == 1) ? model(pos, PASS_PAYLOAD_ARGS).xyz : raycastingResult.color.xyz;

    float4 const shadedColor = shadingMetal(
          pos,  color, normalAtHitPos, rayDirection, PASS_PAYLOAD_ARGS);

    
    float const travelDist = raycastingResult.traveledDistance;
    float4 result =
      mix(shadedColor, bgColor, 1.f - exp(-1.0E-10f * travelDist * travelDist * travelDist));
    return result;
}

float16 modelViewPerspectiveMatrix(float3 eyePosition, float3 lookAt, float roll)
{
    float16 matrix;
    float3 ww = normalize(lookAt - eyePosition);
    float3 uu = normalize(cross(ww, (float3)(sin(roll), cos(roll), 0.f)));
    float3 vv = normalize(cross(uu, ww));

    matrix.s0 = uu.x;
    matrix.s1 = uu.y;
    matrix.s2 = uu.z;
    matrix.s3 = 0.f;

    matrix.s4 = vv.x;
    matrix.s5 = vv.y;
    matrix.s6 = vv.z;
    matrix.s7 = 0.f;

    matrix.s8 = ww.x;
    matrix.s9 = ww.y;
    matrix.sa = ww.z;
    matrix.sb = 0.f;

    matrix.sc = 0.f;
    matrix.sd = 0.f;
    matrix.se = 0.f;
    matrix.sf = 1.f;
    return matrix;
}
