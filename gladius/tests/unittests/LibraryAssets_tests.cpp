#include "io/3mf/Lib3mfLoader.h"
#include "io/3mf/LibraryMetadata.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace gladius::io::tests
{
    namespace
    {
        struct ShippedLibraryAsset
        {
            std::string fileName;
            std::string functionName;
            std::vector<std::pair<std::string, Lib3MF::eImplicitPortType>> expectedInputs{};
        };
    } // namespace

    class ShippedMechanicalLibraryAssetTest
      : public ::testing::TestWithParam<ShippedLibraryAsset>
    {
      protected:
        void SetUp() override
        {
            m_wrapper = loadLib3mfScoped();
            ASSERT_TRUE(m_wrapper) << "Failed to load Lib3MF library";
        }

        Lib3MF::PWrapper m_wrapper;
    };

    TEST_P(ShippedMechanicalLibraryAssetTest,
           NativeReopen_ImportsTaggedFunctionWithMetadataAndThumbnail)
    {
        auto const & asset = GetParam();
        auto const assetPath = std::filesystem::path("testdata") / "mechanical" / asset.fileName;
        ASSERT_TRUE(std::filesystem::exists(assetPath)) << assetPath;

        auto model = m_wrapper->CreateModel();
        ASSERT_TRUE(model);
        auto reader = model->QueryReader("3mf");
        reader->SetStrictModeActive(false);
        ASSERT_NO_THROW(reader->ReadFromFile(assetPath.string()));

        EXPECT_TRUE(model->HasPackageThumbnailAttachment());

        auto const metadata = readLibraryMetadata(model);
        ASSERT_TRUE(metadata.has_value());
        EXPECT_FALSE(metadata->libraryDescription.empty());
        auto const tagCount = 1 + std::count(metadata->libraryTags.begin(),
                                             metadata->libraryTags.end(),
                                             ',');
        EXPECT_GE(tagCount, 5);

        auto const taggedIds = parseResourceIds(metadata->libraryFunctions);
        ASSERT_EQ(taggedIds.size(), 1u);

        bool foundTaggedFunction = false;
        auto functions = model->GetFunctions();
        while (functions->MoveNext())
        {
            auto function = functions->GetCurrentFunction();
            if (!function || function->GetModelResourceID() != taggedIds.front())
            {
                continue;
            }

            foundTaggedFunction = true;
            EXPECT_EQ(function->GetDisplayName(), asset.functionName);

            bool hasShapeOutput = false;
            auto outputs = function->GetOutputs();
            while (outputs->MoveNext())
            {
                auto output = outputs->GetCurrent();
                if (output && output->GetDisplayName() == "shape" &&
                    output->GetType() == Lib3MF::eImplicitPortType::Scalar)
                {
                    hasShapeOutput = true;
                    break;
                }
            }
            EXPECT_TRUE(hasShapeOutput) << "Tagged function must expose a scalar 'shape' output";

            if (!asset.expectedInputs.empty())
            {
                auto inputs = function->GetInputs();
                std::size_t inputIndex = 0;
                while (inputs->MoveNext())
                {
                    ASSERT_LT(inputIndex, asset.expectedInputs.size());
                    auto input = inputs->GetCurrent();
                    ASSERT_TRUE(input);
                    EXPECT_EQ(input->GetDisplayName(), asset.expectedInputs[inputIndex].first);
                    EXPECT_EQ(input->GetType(), asset.expectedInputs[inputIndex].second);
                    ++inputIndex;
                }
                EXPECT_EQ(inputIndex, asset.expectedInputs.size());
            }
            break;
        }
        EXPECT_TRUE(foundTaggedFunction) << "Tagged resource ID does not identify a function";

        auto prunedModel = pruneSourceForImport(assetPath, nullptr);
        ASSERT_TRUE(prunedModel.has_value()) << "Selective import closure could not be built";
        auto importTarget = m_wrapper->CreateModel();
        ASSERT_TRUE(importTarget);
        ASSERT_NO_THROW(importTarget->MergeFromModel(prunedModel->get()));

        bool importedTaggedFunction = false;
        auto importedFunctions = importTarget->GetFunctions();
        while (importedFunctions->MoveNext())
        {
            auto function = importedFunctions->GetCurrentFunction();
            if (function && function->GetDisplayName() == asset.functionName)
            {
                importedTaggedFunction = true;
                break;
            }
        }
        EXPECT_TRUE(importedTaggedFunction)
          << "Fresh-model import did not include the tagged function";
    }

    INSTANTIATE_TEST_SUITE_P(
      ParametricBuildingBlocks,
      ShippedMechanicalLibraryAssetTest,
      ::testing::Values(
        ShippedLibraryAsset{"brick_stud.3mf", "brick_stud"},
        ShippedLibraryAsset{"brick_socket_cutter.3mf", "brick_socket_cutter"},
        ShippedLibraryAsset{"building_brick_nm.3mf", "building_brick_nm"},
        ShippedLibraryAsset{"brick_8mm_pitch_nm.3mf", "brick_8mm_pitch_nm"},
        ShippedLibraryAsset{"brick_16mm_pitch_nm.3mf", "brick_16mm_pitch_nm"},
        ShippedLibraryAsset{"plate_8mm_pitch_nm.3mf", "plate_8mm_pitch_nm"},
        ShippedLibraryAsset{"tile_8mm_pitch_nm.3mf", "tile_8mm_pitch_nm"},
        ShippedLibraryAsset{"plate_16mm_pitch_nm.3mf", "plate_16mm_pitch_nm"},
        ShippedLibraryAsset{"tile_16mm_pitch_nm.3mf", "tile_16mm_pitch_nm"},
        ShippedLibraryAsset{"building_brick_fit_coupon.3mf", "building_brick_fit_coupon"},
        ShippedLibraryAsset{"car_coupler_16mm_pitch.3mf",
                "car_coupler_16mm_pitch",
                    {{"pos", Lib3MF::eImplicitPortType::Vector},
                     {"preview_yaw_rad", Lib3MF::eImplicitPortType::Scalar},
                     {"socket_clearance", Lib3MF::eImplicitPortType::Scalar},
                     {"hinge_clearance", Lib3MF::eImplicitPortType::Scalar}}}),
      [](::testing::TestParamInfo<ShippedLibraryAsset> const & info)
      {
          return info.param.functionName;
      });

} // namespace gladius::io::tests
