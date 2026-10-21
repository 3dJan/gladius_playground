#include "AboutDialog.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef __EMSCRIPTEN__
#include <cmrc/cmrc.hpp>
CMRC_DECLARE(gladius_resources);
#endif

#include "../FileSystemUtils.h"

#include "imgui.h"
#include <fmt/format.h>

#include "Widgets.h"
#include "version.h"
#include "wordwarp.h"

namespace gladius::ui
{
    namespace
    {
        bool isCopyrightAssetFile(std::filesystem::path const & path)
        {
#ifdef __EMSCRIPTEN__
            auto const resources = cmrc::gladius_resources::get_filesystem();
            return resources.is_file(path.generic_string());
#else
            return std::filesystem::exists(path);
#endif
        }

        std::string readCopyrightAssetOrReturnEmpty(std::filesystem::path const & path)
        {
#ifdef __EMSCRIPTEN__
            try
            {
                auto const resource =
                  cmrc::gladius_resources::get_filesystem().open(path.generic_string());
                return warpTextAfter(std::string(resource.begin(), resource.end()), 200);
            }
            catch (std::exception const & e)
            {
                std::cerr << "Loading embedded license failed: " << e.what() << "\n";
                return {};
            }
#else
            return readFileOrReturnEmpty(path);
#endif
        }

        std::vector<std::filesystem::path> getSupplementaryCopyrightFiles(
          std::filesystem::path const & directory)
        {
            std::vector<std::filesystem::path> files;
#ifdef __EMSCRIPTEN__
            auto const resourceDirectory = directory.generic_string();
            auto const resources = cmrc::gladius_resources::get_filesystem();
            if (!resources.is_directory(resourceDirectory))
            {
                return files;
            }

            for (auto const entry : resources.iterate_directory(resourceDirectory))
            {
                auto const filename = std::string(entry.filename());
                if (entry.is_file() && filename.starts_with("COPYING."))
                {
                    files.emplace_back(directory / filename);
                }
            }
#else
            std::error_code error;
            std::filesystem::directory_iterator entry{directory, error};
            std::filesystem::directory_iterator const end;
            while (!error && entry != end)
            {
                auto const filename = entry->path().filename().string();
                if (entry->is_regular_file(error) && filename.starts_with("COPYING."))
                {
                    files.emplace_back(entry->path());
                }
                entry.increment(error);
            }
#endif
            std::sort(files.begin(), files.end());
            return files;
        }
    }

    std::string readFileOrReturnEmpty(std::filesystem::path const & path)
    {
        try
        {
            const std::ifstream file{path};
            if (!file)
            {
                return {};
            }

            std::stringstream sstream;
            sstream << file.rdbuf();
            return warpTextAfter(sstream.str(), 200);
        }
        catch (std::exception const & e)
        {
            std::cerr << "Loading failed: " << e.what() << "\n";
            return {};
        }
    }

    CopyRightInfo::CopyRightInfo(std::filesystem::path path)
        : m_path(std::move(path))
    {
    }

    std::string const & CopyRightInfo::getName() const
    {
        return m_name;
    }

    std::string CopyRightInfo::getOrLoadUrl()
    {
        if (m_url.has_value())
        {
            return m_url.value();
        }

        auto const urlPath = m_path / "url";
        if (isCopyrightAssetFile(urlPath))
        {
            m_url = readCopyrightAssetOrReturnEmpty(urlPath);
        }

        return m_url.value_or(std::string{});
    }

    std::string CopyRightInfo::getOrLoadCopyrightText()
    {
        if (m_licenseText.has_value())
        {
            return m_licenseText->empty() ? "No LICENSE file found" : m_licenseText.value();
        }

        auto const licensePath = m_path / "LICENSE";
        if (!isCopyrightAssetFile(licensePath))
        {
            m_licenseText = "No LICENSE file found";
            return m_licenseText.value();
        }

        auto licenseText = readCopyrightAssetOrReturnEmpty(licensePath);
        for (auto const & supplementaryPath : getSupplementaryCopyrightFiles(m_path))
        {
            auto const supplementaryText = readCopyrightAssetOrReturnEmpty(supplementaryPath);
            if (supplementaryText.empty())
            {
                continue;
            }

            licenseText += "\n\n" + supplementaryPath.filename().string() + ":\n";
            licenseText += supplementaryText;
        }

        m_licenseText = std::move(licenseText);
    return m_licenseText->empty() ? "No LICENSE file found" : m_licenseText.value();
    }

    CopyRightInfoCache::CopyRightInfoCache(std::filesystem::path licenseDir)
    {
#ifdef __EMSCRIPTEN__
        auto const resourcePath = licenseDir.generic_string();
        auto const resources = cmrc::gladius_resources::get_filesystem();
        if (!resources.is_directory(resourcePath))
        {
            return;
        }

        for (auto const entry : resources.iterate_directory(resourcePath))
        {
            if (entry.is_directory())
            {
                m_licenses.emplace_back(
                  std::filesystem::path{resourcePath} / std::string(entry.filename()));
            }
        }
#else
        std::error_code error;
        if (!std::filesystem::is_directory(licenseDir, error))
        {
            return;
        }

        for (auto const & dir : std::filesystem::directory_iterator(licenseDir, error))
        {
            if (error)
            {
                break;
            }
            m_licenses.emplace_back(dir);
        }
#endif
    }

    CopyRightInfoCache::CopyRigths::const_iterator CopyRightInfoCache::cbegin() const
    {
        return m_licenses.cbegin();
    }

    CopyRightInfoCache::CopyRigths::iterator CopyRightInfoCache::end()
    {
        return m_licenses.end();
    }

    CopyRightInfoCache::CopyRigths::iterator CopyRightInfoCache::begin()
    {
        return m_licenses.begin();
    }

    CopyRightInfoCache::CopyRigths::const_iterator CopyRightInfoCache::cend() const
    {
        return m_licenses.cend();
    }

    AboutDialog::AboutDialog()
#ifdef __EMSCRIPTEN__
        : m_copyRightInfoCache("licenses")
        , m_copyRight("copyright")
#else
        : m_copyRightInfoCache(getAppDir() / "licenses")
        , m_copyRight(getAppDir() / "copyright")
#endif
    {
    }

    void AboutDialog::show()
    {
        m_visible = true;
    }

    void AboutDialog::hide()
    {
        m_visible = false;
    }

    void AboutDialog::render()
    {
        if (!m_visible)
        {
            return;
        }

        ImGui::Begin("About Gladius", &m_visible);

        ImGui::Spacing();
        ImGui::TextUnformatted(
          fmt::format(
            "Gladius {}.{}.{} is a viewer and editor for 3mf files using the volumetric extension.",
            Version::major,
            Version::minor,
            Version::revision)
            .c_str());

        ImGui::Spacing();

        hyperlink("https://github.com/3MFConsortium/gladius",
                  "https://github.com/3MFConsortium/gladius");
        ImGui::Spacing();

        ImGui::TextUnformatted("Copyright 3MF Consortium");

        if (ImGui::CollapsingHeader("Gladius is licensed under BSD 2-Clause License"))
        {
            ImGui::Indent();
            ImGui::TextUnformatted(m_copyRight.getOrLoadCopyrightText().c_str());
        }

        if (ImGui::CollapsingHeader("3rd-party libs and acknowledgments"))
        {
            ImGui::Indent();
            for (auto & cpInfo : m_copyRightInfoCache)
            {
                if (ImGui::CollapsingHeader(cpInfo.getName().c_str()))
                {
                    auto const url = cpInfo.getOrLoadUrl();
                    if (!url.empty())
                    {
                        hyperlink(fmt::format("website: {}", url), url);
                    }
                    ImGui::Separator();
                    ImGui::TextUnformatted(cpInfo.getOrLoadCopyrightText().c_str());
                }
            }
        }
        ImGui::End();
    }
}
