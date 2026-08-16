#include "FaceColorSampler.h"

#if !defined(GLADIUS_ENABLE_OPENCL)

#include "nodes/Model.h"

#include <cmath>

namespace gladius::io
{
    bool FaceColorSampler::hasVolumetricColor(nodes::Model const & model)
    {
        auto * endNode = const_cast<nodes::Model &>(model).getEndNode();
        if (endNode == nullptr)
        {
            return false;
        }

        auto const & parameters = endNode->parameter();
        auto const color = parameters.find(nodes::FieldNames::Color);
        return color != parameters.end() && color->second.getConstSource().has_value();
    }

    float FaceColorSampler::linearToSrgb(float linear)
    {
        if (linear <= 0.0031308F)
        {
            return 12.92F * linear;
        }
        return 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
    }

    Eigen::Vector3f FaceColorSampler::linearToSrgb(Eigen::Vector3f const & linear)
    {
        return {linearToSrgb(linear.x()), linearToSrgb(linear.y()), linearToSrgb(linear.z())};
    }
} // namespace gladius::io

#endif
