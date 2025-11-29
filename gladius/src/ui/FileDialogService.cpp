#include "FileDialogService.h"

#include <array>
#include <cstdio>
#include <sstream>

namespace gladius::ui
{
    namespace
    {
#ifdef __linux__
        /// @brief Check if a command exists in PATH
        [[nodiscard]] bool commandExists(char const * cmd)
        {
            std::string checkCmd = "command -v ";
            checkCmd += cmd;
            checkCmd += " > /dev/null 2>&1";
            return std::system(checkCmd.c_str()) == 0;
        }

        /// @brief Execute a command and read its stdout (blocking)
        [[nodiscard]] std::optional<std::string> executeCommand(std::string const & cmd)
        {
            std::array<char, 4096> buffer{};
            std::string result;

            FILE * pipe = popen(cmd.c_str(), "r");
            if (pipe == nullptr)
            {
                return std::nullopt;
            }

            while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
            {
                result += buffer.data();
            }

            int const status = pclose(pipe);
            if (status != 0)
            {
                return std::nullopt; // User cancelled or error
            }

            // Trim trailing newline
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
            {
                result.pop_back();
            }

            return result.empty() ? std::nullopt : std::make_optional(result);
        }

        /// @brief Escape a string for shell usage
        [[nodiscard]] std::string shellEscape(std::string const & str)
        {
            std::string result = "'";
            for (char c : str)
            {
                if (c == '\'')
                {
                    result += "'\\''";
                }
                else
                {
                    result += c;
                }
            }
            result += "'";
            return result;
        }

        enum class DialogMode
        {
            Save,
            Open,
            Directory
        };

        /// @brief Build zenity command
        [[nodiscard]] std::string buildZenityCommand(DialogMode mode,
                                                      FilePatterns const & patterns,
                                                      std::filesystem::path const & defaultPath)
        {
            std::ostringstream cmd;
            cmd << "zenity --file-selection";

            switch (mode)
            {
            case DialogMode::Save:
                cmd << " --save --confirm-overwrite --title='Save File'";
                break;
            case DialogMode::Open:
                cmd << " --title='Open File'";
                break;
            case DialogMode::Directory:
                cmd << " --directory --title='Select Directory'";
                break;
            }

            if (!defaultPath.empty())
            {
                if (std::filesystem::is_directory(defaultPath))
                {
                    cmd << " --filename=" << shellEscape(defaultPath.string() + "/");
                }
                else
                {
                    cmd << " --filename=" << shellEscape(defaultPath.string());
                }
            }

            if (mode != DialogMode::Directory && !patterns.empty())
            {
                for (auto const & pattern : patterns)
                {
                    cmd << " --file-filter=" << shellEscape(pattern);
                }
                cmd << " --file-filter='All files | *'";
            }

            cmd << " 2>/dev/null";
            return cmd.str();
        }

        /// @brief Build kdialog command
        [[nodiscard]] std::string buildKdialogCommand(DialogMode mode,
                                                       FilePatterns const & patterns,
                                                       std::filesystem::path const & defaultPath)
        {
            std::ostringstream cmd;

            switch (mode)
            {
            case DialogMode::Save:
                cmd << "kdialog --getsavefilename";
                break;
            case DialogMode::Open:
                cmd << "kdialog --getopenfilename";
                break;
            case DialogMode::Directory:
                cmd << "kdialog --getexistingdirectory";
                break;
            }

            cmd << " " << shellEscape(defaultPath.empty() ? std::filesystem::current_path().string()
                                                          : defaultPath.string());

            if (mode != DialogMode::Directory && !patterns.empty())
            {
                std::ostringstream filter;
                for (size_t i = 0; i < patterns.size(); ++i)
                {
                    if (i > 0)
                        filter << " ";
                    filter << patterns[i];
                }
                cmd << " " << shellEscape(filter.str());
            }

            cmd << " 2>/dev/null";
            return cmd.str();
        }

        /// @brief Show dialog using zenity or kdialog (blocking - run in background thread)
        [[nodiscard]] QueriedFilename showLinuxDialog(DialogMode mode,
                                                       FilePatterns const & patterns,
                                                       std::filesystem::path const & defaultPath)
        {
            std::string command;

            if (commandExists("zenity"))
            {
                command = buildZenityCommand(mode, patterns, defaultPath);
            }
            else if (commandExists("kdialog"))
            {
                command = buildKdialogCommand(mode, patterns, defaultPath);
            }
            else
            {
                // Fall back to tinyfiledialogs
                return std::nullopt;
            }

            if (auto result = executeCommand(command))
            {
                return std::filesystem::path(*result);
            }
            return std::nullopt;
        }
#endif // __linux__

        /// @brief Run save dialog (blocking)
        [[nodiscard]] QueriedFilename runSaveDialog(FilePatterns patterns,
                                                     std::filesystem::path defaultPath)
        {
#ifdef __linux__
            if (auto result = showLinuxDialog(DialogMode::Save, patterns, defaultPath))
            {
                return result;
            }
#endif
            // Fallback to tinyfiledialogs
            return querySaveFilename(patterns, defaultPath);
        }

        /// @brief Run open dialog (blocking)
        [[nodiscard]] QueriedFilename runOpenDialog(FilePatterns patterns,
                                                     std::filesystem::path defaultPath)
        {
#ifdef __linux__
            if (auto result = showLinuxDialog(DialogMode::Open, patterns, defaultPath))
            {
                return result;
            }
#endif
            return queryLoadFilename(patterns, defaultPath);
        }

        /// @brief Run directory dialog (blocking)
        [[nodiscard]] QueriedFilename runDirectoryDialog(std::filesystem::path defaultPath)
        {
#ifdef __linux__
            if (auto result = showLinuxDialog(DialogMode::Directory, {}, defaultPath))
            {
                return result;
            }
#endif
            return queryDirectory(defaultPath);
        }

    } // anonymous namespace

    void AsyncFileDialog::saveFile(FilePatterns patterns, std::filesystem::path defaultPath)
    {
        if (isActive())
        {
            return; // Already running
        }
        m_hasResult = false;
        m_future = std::async(std::launch::async, runSaveDialog, std::move(patterns), std::move(defaultPath));
    }

    void AsyncFileDialog::openFile(FilePatterns patterns, std::filesystem::path defaultPath)
    {
        if (isActive())
        {
            return;
        }
        m_hasResult = false;
        m_future = std::async(std::launch::async, runOpenDialog, std::move(patterns), std::move(defaultPath));
    }

    void AsyncFileDialog::selectDirectory(std::filesystem::path defaultPath)
    {
        if (isActive())
        {
            return;
        }
        m_hasResult = false;
        m_future = std::async(std::launch::async, runDirectoryDialog, std::move(defaultPath));
    }

    bool AsyncFileDialog::isActive() const
    {
        if (!m_future.valid())
        {
            return false;
        }
        // Check if still running (wait for 0 time)
        return m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    }

    std::optional<QueriedFilename> AsyncFileDialog::checkResult()
    {
        if (!m_future.valid() || m_hasResult)
        {
            return std::nullopt;
        }

        if (m_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            m_hasResult = true;
            return m_future.get();
        }

        return std::nullopt;
    }

} // namespace gladius::ui
