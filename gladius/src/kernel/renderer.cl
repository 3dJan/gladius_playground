
void kernel resample(__write_only image2d_t result, __read_only image2d_t source)
{
    const int2 coords = {get_global_id(0), get_global_id(1)};
    // map source into result
    int2 imageSize = (int2)(get_image_width(result), get_image_height(result));

    // Normalized position
    float2 pos =
      (float2)((float) coords.x / (imageSize.x - 1), (float) coords.y / (imageSize.y - 1));

    float4 srcValue = read_imagef(source, samplerLinearPos, pos);
    write_imagef(result, coords, srcValue);
};

void kernel renderScene(__write_only image2d_t result,  
                        PAYLOAD_ARGS,                   
                        float3 eyePosition,             
                        float16 modelViewPerspectiveMat 
)
{
    int2 const coord = {get_global_id(0), get_global_id(1)};

    int2 const imageSize = (int2)(get_image_width(result), get_image_height(result));
    float const aspectRatio =
      (float) (get_image_width(result)) / (float) (get_image_height(result));
    // Normalized position
    float2 screenPos =
      (float2)((float) coord.x / (imageSize.x - 1), 1.f - (float) coord.y / (imageSize.y - 1)) +
      (float2)(-0.5f, -0.5f);
    screenPos.y /= aspectRatio;

    // Let's shoot a ray
    float const lensLength = 1.;
    float3 const rayDirection =
      normalize(matrixVectorMul3f(modelViewPerspectiveMat, (float3)(screenPos, lensLength)));
    float const startDistance = 0.f;
    float const minDistanceForCmpStart = 1.0E-3f;
    float const sizeDecreasePerTd = 1.0E-3f;
    
    // Use legacy rayCast without distance init
    struct RayCastResult const raycastingResult =
      rayCastLegacy(eyePosition, rayDirection, startDistance, PASS_PAYLOAD_ARGS);

    float4 const color =
      determineColor(raycastingResult, eyePosition, rayDirection, PASS_PAYLOAD_ARGS);
    write_imagef(result, coord, (float4)(color));
};

/**
 * @brief Low-res preview kernel that writes traveled distance to buffer (T017)
 *
 * This kernel variant writes the traveledDistance from rayCast to a float image
 * for use as initialization in subsequent HQ rendering passes.
 */
void kernel renderSceneWithDistanceOutput(__write_only image2d_t result,
                                          __write_only image2d_t distanceOutput,
                                          PAYLOAD_ARGS,
                                          float3 eyePosition,
                                          float16 modelViewPerspectiveMat)
{
    int2 const coord = {get_global_id(0), get_global_id(1)};

    int2 const imageSize = (int2)(get_image_width(result), get_image_height(result));
    float const aspectRatio =
      (float) (get_image_width(result)) / (float) (get_image_height(result));
    // Normalized position
    float2 screenPos =
      (float2)((float) coord.x / (imageSize.x - 1), 1.f - (float) coord.y / (imageSize.y - 1)) +
      (float2)(-0.5f, -0.5f);
    screenPos.y /= aspectRatio;

    // Let's shoot a ray
    float const lensLength = 1.;
    float3 const rayDirection =
      normalize(matrixVectorMul3f(modelViewPerspectiveMat, (float3)(screenPos, lensLength)));
    float const startDistance = 0.f;
    
    // Use legacy rayCast (no distance init for preview pass)
    struct RayCastResult const raycastingResult =
      rayCastLegacy(eyePosition, rayDirection, startDistance, PASS_PAYLOAD_ARGS);

    float4 const color =
      determineColor(raycastingResult, eyePosition, rayDirection, PASS_PAYLOAD_ARGS);
    write_imagef(result, coord, (float4)(color));
    
    // Write traveled distance to buffer for HQ init (FR-005, FR-006)
    // Store as single-channel float; only meaningful when hit occurred
    float distanceValue = (raycastingResult.hit > 0.f) ? raycastingResult.traveledDistance : 0.f;
    write_imagef(distanceOutput, coord, (float4)(distanceValue, 0.f, 0.f, 0.f));
};

/**
 * @brief HQ rendering kernel with distance initialization from low-res preview (T016)
 *
 * Uses the distanceInit buffer to skip empty space at the start of each ray,
 * significantly reducing ray march steps for HQ rendering.
 */
void kernel renderSceneWithDistanceInit(__write_only image2d_t result,
                                        __read_only image2d_t distanceInit,
                                        PAYLOAD_ARGS,
                                        float3 eyePosition,
                                        float16 modelViewPerspectiveMat)
{
    int2 const coord = {get_global_id(0), get_global_id(1)};

    int2 const imageSize = (int2)(get_image_width(result), get_image_height(result));
    float const aspectRatio =
      (float) (get_image_width(result)) / (float) (get_image_height(result));
    // Normalized position
    float2 screenPos =
      (float2)((float) coord.x / (imageSize.x - 1), 1.f - (float) coord.y / (imageSize.y - 1)) +
      (float2)(-0.5f, -0.5f);
    screenPos.y /= aspectRatio;

    // Let's shoot a ray
    float const lensLength = 1.;
    float3 const rayDirection =
      normalize(matrixVectorMul3f(modelViewPerspectiveMat, (float3)(screenPos, lensLength)));
    float const startDistance = 0.f;
    
    // Sample distance init buffer with bilinear interpolation (FR-005, FR-006)
    // Normalized position in distanceInit texture
    float2 normPos = (float2)((float)coord.x / (float)(imageSize.x - 1),
                              (float)coord.y / (float)(imageSize.y - 1));
    float4 distInitSample = read_imagef(distanceInit, samplerLinearPosClamp, normPos);
    float distanceInitValue = distInitSample.x;
    
    // Use enhanced rayCast with distance initialization
    bool useDistanceInit = (renderingSettings.approximation & AM_USE_DISTANCE_INIT) && (distanceInitValue > 0.f);
    struct RayCastResult const raycastingResult =
      rayCast(eyePosition, rayDirection, startDistance, useDistanceInit, distanceInitValue, PASS_PAYLOAD_ARGS);

    float4 const color =
      determineColor(raycastingResult, eyePosition, rayDirection, PASS_PAYLOAD_ARGS);
    write_imagef(result, coord, (float4)(color));
};
