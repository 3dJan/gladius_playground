#include "LibraryBrowser.h"
#include "../FileSystemUtils.h"
#include "../IconFontCppHeaders/IconsFontAwesome5.h"
#include "FileChooser.h"
#include "imgui.h"
#include <algorithm>
#include <fmt/format.h>

namespace gladius::ui
{
    LibraryBrowser::LibraryBrowser(events::SharedLogger logger)
        : m_logger(std::move(logger))
    {
    }

    void LibraryBrowser::setLogger(events::SharedLogger logger)
    {
        m_logger = std::move(logger);
    }

    LibraryBrowser::~LibraryBrowser() = default;

    void LibraryBrowser::setRootDirectory(const std::filesystem::path & directory)
    {
        if (m_rootDirectory != directory)
        {
            m_rootDirectory = directory;
            m_needsRefresh = true;
        }
    }

    void LibraryBrowser::setVisibility(bool visible)
    {
        m_visible = visible;
    }

    void LibraryBrowser::refreshDirectories()
    {
        m_needsRefresh = true;
    }

    void LibraryBrowser::scanSubfolders()
    {
        if (!m_needsRefresh)
        {
            return;
        }

        m_subfolders.clear();
        m_fileBrowsers.clear();

        if (!std::filesystem::exists(m_rootDirectory) ||
            !std::filesystem::is_directory(m_rootDirectory))
        {
            return;
        }

        try
        {
            for (const auto & entry : std::filesystem::directory_iterator(m_rootDirectory))
            {
                if (entry.is_directory())
                {
                    auto const folderName = entry.path().filename().string();
                    if (!folderName.empty() && folderName[0] == '.')
                    {
                        continue; // Skip dot-prefixed folders (e.g. .bin)
                    }
                    m_subfolders.push_back(entry.path());
                }
            }
        }
        catch (const std::filesystem::filesystem_error & e)
        {
            if (m_logger)
            {
                m_logger->addEvent({e.what(), events::Severity::Error});
            }
        }

        createFileBrowsers();
        m_needsRefresh = false;
    }

    void LibraryBrowser::createFileBrowsers()
    {
        for (const auto & subfolder : m_subfolders)
        {
            // Use folder name as key
            std::string folderName = subfolder.filename().string();

            // Create file browser for this subfolder if it doesn't exist
            if (m_fileBrowsers.find(folderName) == m_fileBrowsers.end())
            {
                auto browser = std::make_unique<ThreemfFileViewer>(m_logger);
                browser->setDirectory(subfolder);
                browser->setIsShippedPredicate(
                  [](std::filesystem::path const & filePath) -> bool
                  {
                      auto const category =
                        filePath.parent_path().filename().string();
                      auto const name = filePath.stem().string();
                      return isShippedEntry(category, name);
                  });
                browser->setOnDeleteCallback(
                  [this](std::filesystem::path const & filePath) -> bool
                  {
                      try
                      {
                          auto const category =
                            filePath.parent_path().filename().string();
                          auto const name = filePath.stem().string();

                          if (isShippedEntry(category, name))
                          {
                              return false;
                          }

                          auto const binCategoryDir = getBinDir() / category;
                          std::filesystem::create_directories(binCategoryDir);
                          auto const destPath =
                            disambiguateFilename(binCategoryDir, name, ".3mf");
                          std::filesystem::rename(filePath, destPath);
                          m_binNeedsRefresh = true;
                          return true;
                      }
                      catch (std::exception const &)
                      {
                          return false;
                      }
                  });
                m_fileBrowsers[folderName] = std::move(browser);
            }
            else
            {
                // Update directory if it exists but might have changed
                m_fileBrowsers[folderName]->setDirectory(subfolder);
                m_fileBrowsers[folderName]->refreshDirectory();
            }
        }
    }

    void LibraryBrowser::render(SharedDocument doc)
    {
        if (!m_visible)
        {
            return;
        }

        // Scan the directories if needed
        scanSubfolders();

        // Configure window
        ImGuiWindowFlags windowFlags = 0;
        ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);

        if (ImGui::Begin("3MF Library Browser", &m_visible, windowFlags))
        {

            // Skip tab bar if no subfolders or only the root
            if (m_subfolders.empty() ||
                (m_subfolders.size() == 1 && m_subfolders[0] == m_rootDirectory))
            {
                if (m_fileBrowsers.find("Root") != m_fileBrowsers.end())
                {
                    m_fileBrowsers["Root"]->render(doc);
                }
            }
            else
            {
                // Create tabs for each subfolder
                if (ImGui::BeginTabBar("DirectoryTabs", ImGuiTabBarFlags_None))
                {
                    for (const auto & [name, browser] : m_fileBrowsers)
                    {
                        if (ImGui::BeginTabItem(name.c_str()))
                        {
                            browser->render(doc);
                            ImGui::EndTabItem();
                        }
                    }

                    // Bin tab
                    if (ImGui::BeginTabItem(ICON_FA_TRASH_ALT " Bin"))
                    {
                        renderBinTab(doc);
                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }
            }
        }
        ImGui::End();
    }

    void LibraryBrowser::scanBinFolder()
    {
        if (!m_binNeedsRefresh)
        {
            return;
        }

        m_binSubfolders.clear();
        m_binBrowsers.clear();

        auto const binDir = getBinDir();
        if (!std::filesystem::exists(binDir) || !std::filesystem::is_directory(binDir))
        {
            m_binNeedsRefresh = false;
            return;
        }

        try
        {
            for (auto const & entry : std::filesystem::directory_iterator(binDir))
            {
                if (entry.is_directory())
                {
                    m_binSubfolders.push_back(entry.path());
                }
            }
        }
        catch (std::filesystem::filesystem_error const & e)
        {
            if (m_logger)
            {
                m_logger->addEvent({e.what(), events::Severity::Error});
            }
        }

        createBinBrowsers();
        m_binNeedsRefresh = false;
    }

    void LibraryBrowser::createBinBrowsers()
    {
        for (auto const & subfolder : m_binSubfolders)
        {
            auto const folderName = subfolder.filename().string();
            auto browser = std::make_unique<ThreemfFileViewer>(m_logger);
            browser->setDirectory(subfolder);

            browser->setOnRestoreCallback(
              [this](std::filesystem::path const & filePath) -> bool
              {
                  try
                  {
                      auto const category =
                        filePath.parent_path().filename().string();
                      auto const name = filePath.stem().string();
                      auto const userCatDir = getUserLibraryDir() / category;
                      std::filesystem::create_directories(userCatDir);
                      auto const destPath =
                        disambiguateFilename(userCatDir, name, ".3mf");
                      std::filesystem::rename(filePath, destPath);

                      // Clean up empty bin category dir
                      auto const binCatDir = getBinDir() / category;
                      if (std::filesystem::is_empty(binCatDir))
                      {
                          std::filesystem::remove(binCatDir);
                      }

                      m_needsRefresh = true;
                      m_binNeedsRefresh = true;
                      return true;
                  }
                  catch (std::exception const &)
                  {
                      return false;
                  }
              });

            browser->setOnPermanentDeleteCallback(
              [this](std::filesystem::path const & filePath) -> bool
              {
                  try
                  {
                      auto const category =
                        filePath.parent_path().filename().string();
                      std::filesystem::remove(filePath);

                      auto const binCatDir = getBinDir() / category;
                      if (std::filesystem::exists(binCatDir) &&
                          std::filesystem::is_empty(binCatDir))
                      {
                          std::filesystem::remove(binCatDir);
                      }

                      m_binNeedsRefresh = true;
                      return true;
                  }
                  catch (std::exception const &)
                  {
                      return false;
                  }
              });

            m_binBrowsers[folderName] = std::move(browser);
        }
    }

    void LibraryBrowser::renderBinTab(SharedDocument doc)
    {
        scanBinFolder();

        // "Empty Bin" button
        if (!m_binSubfolders.empty())
        {
            if (ImGui::Button(ICON_FA_TRASH " Empty Bin"))
            {
                ImGui::OpenPopup("ConfirmEmptyBin");
            }

            if (ImGui::BeginPopupModal("ConfirmEmptyBin", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted(
                  "Permanently delete all items in the bin?\nThis cannot be undone.");
                ImGui::Separator();
                if (ImGui::Button("Delete All", ImVec2(120, 0)))
                {
                    auto const binDir = getBinDir();
                    try
                    {
                        for (auto const & catEntry :
                             std::filesystem::directory_iterator(binDir))
                        {
                            if (catEntry.is_directory())
                            {
                                std::filesystem::remove_all(catEntry.path());
                            }
                        }
                    }
                    catch (std::exception const &)
                    {
                    }
                    m_binNeedsRefresh = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0)))
                {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::Separator();
        }

        if (m_binBrowsers.empty())
        {
            ImGui::TextDisabled("Bin is empty");
            return;
        }

        for (auto const & [name, browser] : m_binBrowsers)
        {
            if (ImGui::CollapsingHeader(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                browser->render(doc);
            }
        }
    }

} // namespace gladius::ui
