#include "FileSystemUtils.h"

#include <filesystem>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _MSVC_LANG
#include <unistd.h>
#endif
#include <sago/platform_folders.h>

namespace gladius
{
    std::filesystem::path getAppDir()
    {
#ifdef WIN32
        char * executablePath;
        if (_get_pgmptr(&executablePath) != 0)
        {
            return {};
        }
        return std::filesystem::path{executablePath}.parent_path();
#else
        char executablePath[PATH_MAX];
        ssize_t len = ::readlink("/proc/self/exe", executablePath, sizeof(executablePath));
        if (len == -1 || len >= sizeof(executablePath))
            len = 0;
        executablePath[len] = '\0';

        return std::filesystem::path{executablePath}.parent_path();
#endif
    }

    std::filesystem::path getShippedLibraryDir()
    {
        return getAppDir() / "library";
    }

    std::filesystem::path getUserLibraryDir()
    {
        auto const dir =
          std::filesystem::path{sago::getDataHome()} / "gladius" / "library";

        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        // Silently ignore errors — the caller will notice if the dir is missing.
        return dir;
    }

    std::size_t syncLibraryDirectory(
      std::filesystem::path const & source,
      std::filesystem::path const & target)
    {
        if (!std::filesystem::exists(source) ||
            !std::filesystem::is_directory(source))
        {
            return 0;
        }

        std::size_t copiedCount = 0;

        for (auto const & entry :
             std::filesystem::recursive_directory_iterator(source))
        {
            auto const relativePath =
              std::filesystem::relative(entry.path(), source);
            auto const targetPath = target / relativePath;

            if (entry.is_directory())
            {
                std::error_code ec;
                std::filesystem::create_directories(targetPath, ec);
            }
            else if (entry.is_regular_file() &&
                     !std::filesystem::exists(targetPath))
            {
                std::error_code ec;
                std::filesystem::create_directories(targetPath.parent_path(), ec);
                std::filesystem::copy_file(
                  entry.path(),
                  targetPath,
                  std::filesystem::copy_options::skip_existing,
                  ec);
                if (!ec)
                {
                    ++copiedCount;
                }
            }
        }

        return copiedCount;
    }

    std::size_t syncShippedLibrary()
    {
        return syncLibraryDirectory(getShippedLibraryDir(), getUserLibraryDir());
    }

    std::filesystem::path getBinDir()
    {
        return getUserLibraryDir() / ".bin";
    }

    bool isShippedEntry(std::string const & category, std::string const & name)
    {
        auto const shippedPath = getShippedLibraryDir() / category / (name + ".3mf");
        return std::filesystem::exists(shippedPath);
    }

    std::filesystem::path disambiguateFilename(std::filesystem::path const & directory,
                                               std::string const & stem,
                                               std::string const & extension)
    {
        auto basePath = directory / (stem + extension);
        if (!std::filesystem::exists(basePath))
        {
            return basePath;
        }

        for (int i = 1; i <= 999; ++i)
        {
            auto candidate = directory / (stem + "_" + std::to_string(i) + extension);
            if (!std::filesystem::exists(candidate))
            {
                return candidate;
            }
        }

        // Practically unreachable
        throw std::runtime_error(
          "Cannot disambiguate filename: all 999 suffix slots exhausted for '" + stem + extension +
          "' in " + directory.string());
    }
}
