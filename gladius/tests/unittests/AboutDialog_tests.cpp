#include "ui/AboutDialog.h"

#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace gladius::tests
{
    class CopyRightInfoTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            auto const * testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
            auto const testDir = std::string{testInfo->test_suite_name()} + "_" + testInfo->name();
            m_licenseDir = std::filesystem::temp_directory_path() /
                           "gladius_about_dialog_tests" / testDir / "licenses";
            std::filesystem::remove_all(m_licenseDir.parent_path());
            std::filesystem::create_directories(m_licenseDir);
        }

        void TearDown() override
        {
            std::filesystem::remove_all(m_licenseDir.parent_path());
        }

        void writeTextFile(std::filesystem::path const & path, std::string const & content) const
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream output{path};
            output << content;
        }

        std::filesystem::path m_licenseDir;
    };

    TEST_F(CopyRightInfoTest, Cache_WithVendorDirectories_EnumeratesTheirNames)
    {
        std::filesystem::create_directories(m_licenseDir / "fmt");
        std::filesystem::create_directories(m_licenseDir / "zlib");

        gladius::ui::CopyRightInfoCache cache{m_licenseDir};
        std::vector<std::string> names;
        for (auto const & license : cache)
        {
            names.push_back(license.getName());
        }

        EXPECT_EQ(names.size(), 2u);
        EXPECT_NE(std::find(names.begin(), names.end(), "fmt"), names.end());
        EXPECT_NE(std::find(names.begin(), names.end(), "zlib"), names.end());
    }

    TEST_F(CopyRightInfoTest, Info_WithMetadataFiles_ReturnsUrlAndLicenseText)
    {
        auto const vendorDir = m_licenseDir / "fmt";
        writeTextFile(vendorDir / "url", "https://fmt.dev");
        writeTextFile(vendorDir / "LICENSE", "Test license text");

        gladius::ui::CopyRightInfo info{vendorDir};

        EXPECT_EQ(info.getName(), "fmt");
        EXPECT_EQ(info.getOrLoadUrl(), "https://fmt.dev");
        EXPECT_EQ(info.getOrLoadCopyrightText(), "Test license text");
    }

    TEST_F(CopyRightInfoTest, Info_WithoutUrl_ReturnsEmptyString)
    {
        auto const vendorDir = m_licenseDir / "fmt";
        std::filesystem::create_directories(vendorDir);

        gladius::ui::CopyRightInfo info{vendorDir};

        EXPECT_NO_THROW(EXPECT_TRUE(info.getOrLoadUrl().empty()));
    }

    TEST_F(CopyRightInfoTest, Info_WithoutLicense_ReturnsFallbackMessage)
    {
        auto const vendorDir = m_licenseDir / "fmt";
        std::filesystem::create_directories(vendorDir);

        gladius::ui::CopyRightInfo info{vendorDir};

        EXPECT_EQ(info.getOrLoadCopyrightText(), "No LICENSE file found");
    }
} // namespace gladius::tests