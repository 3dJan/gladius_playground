#include "SurfaceExtractionOptions.h"

namespace gladius::io
{
    void DualContouringOptions::applyPreset()
    {
        switch (qualityPreset)
        {
        case DualContouringQuality::Draft:
            sdfResolution = 65U;
            maxDepth = 5U;  // Increased from 4
            enableCurvatureRefinement = false;
            break;

        case DualContouringQuality::Balanced:
            sdfResolution = 129U;
            maxDepth = 7U;  // Increased from 5 for much better detail
            enableCurvatureRefinement = true;  // Enable for better sphere surfaces
            curvatureThreshold = 0.4F;  // Moderate refinement
            break;

        case DualContouringQuality::Fine:
            sdfResolution = 257U;
            maxDepth = 8U;  // Increased from 6
            enableCurvatureRefinement = true;
            curvatureThreshold = 0.25F;  // More aggressive refinement
            break;

        case DualContouringQuality::UltraFine:
            sdfResolution = 513U;
            maxDepth = 9U;  // Increased from 7
            enableCurvatureRefinement = true;
            curvatureThreshold = 0.15F;  // Very aggressive refinement
            break;

        case DualContouringQuality::Custom:
            // User provides all parameters, don't override
            break;
        }
    }

    void ManifoldDualContouringOptions::applyPreset()
    {
        switch (qualityPreset)
        {
        case ManifoldDualContouringQuality::Draft:
            initialDepth = 4U;
            maxDepth = 6U;
            enableCaching = false;
            break;
        case ManifoldDualContouringQuality::Balanced:
            initialDepth = 5U;
            maxDepth = 7U;
            enableCaching = true;
            break;
        case ManifoldDualContouringQuality::Fine:
            initialDepth = 6U;
            maxDepth = 8U;
            enableCaching = true;
            break;
        case ManifoldDualContouringQuality::UltraFine:
            initialDepth = 7U;
            maxDepth = 9U;
            enableCaching = true;
            break;
        case ManifoldDualContouringQuality::Custom:
            break;
        }

        if (initialDepth > maxDepth)
        {
            initialDepth = maxDepth;
        }
    }
}
