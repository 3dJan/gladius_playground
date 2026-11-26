#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef GLADIUS_WEBGPU_SOURCE_DIR
#define GLADIUS_WEBGPU_SOURCE_DIR ""
#endif

namespace gladius::tests
{
    namespace
    {
        /// @brief One preprocessor conditional while scanning a source file.
        struct GuardFrame
        {
            bool activeNativeOnly{false};
            bool elseNativeOnly{false};
        };

        std::string toLower(std::string_view const value)
        {
            std::string result(value);
            std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            return result;
        }

        /**
         * @brief Classify a preprocessor directive into native-only and browser-only branches.
         *
         * Guards that do not mention __EMSCRIPTEN__ inherit the enclosing branch, so unrelated
         * feature switches never mask a violation.
         */
        void applyDirective(std::vector<GuardFrame> & stack,
                            std::string const & directive,
                            bool const parentNativeOnly)
        {
            bool const mentionsEmscripten = directive.find("__emscripten__") != std::string::npos;
            bool const negated =
              directive.rfind("#ifndef", 0) == 0 || directive.find("!defined") != std::string::npos;

            // "#ifdef" and "#ifndef" are covered by the "#if" prefix match.
            if (directive.rfind("#if", 0) == 0)
            {
                stack.push_back({mentionsEmscripten ? negated : parentNativeOnly,
                                 mentionsEmscripten ? !negated : parentNativeOnly});
                return;
            }
            if (directive.rfind("#else", 0) == 0 && !stack.empty())
            {
                std::swap(stack.back().activeNativeOnly, stack.back().elseNativeOnly);
                return;
            }
            if (directive.rfind("#endif", 0) == 0 && !stack.empty())
            {
                stack.pop_back();
            }
        }

        /**
         * @brief Collect lines that wait on WebGPU callbacks outside a native-only branch.
         *
         * emdawnwebgpu delivers callbacks only from the JS event loop, so pumping it from wasm
         * (directly, or through compute::runBlocking) can never make progress and freezes the tab.
         */
        std::vector<std::string> findBrowserBlockingPumps(std::filesystem::path const & file)
        {
            std::ifstream input{file};
            std::vector<std::string> violations;
            std::vector<GuardFrame> stack;
            std::string line;
            std::size_t lineNumber = 0u;
            while (std::getline(input, line))
            {
                ++lineNumber;
                std::string const lowerLine = toLower(line);
                if (!lowerLine.empty() && lowerLine.front() == '#')
                {
                    applyDirective(stack,
                                   lowerLine,
                                   stack.empty() ? false : stack.back().activeNativeOnly);
                    continue;
                }

                std::size_t const commentStart = line.find("//");
                std::string const code =
                  commentStart == std::string::npos ? line : line.substr(0, commentStart);
                std::string const lowerCode = toLower(code);
                bool const nativeOnly = stack.empty() ? false : stack.back().activeNativeOnly;
                bool const pumpsContext = lowerCode.find("->processevents(") != std::string::npos;
                if (nativeOnly || (!pumpsContext && lowerCode.find("runblocking(") == std::string::npos))
                {
                    continue;
                }
                violations.push_back(file.filename().string() + ":" + std::to_string(lineNumber) +
                                     " -> " + code);
            }
            return violations;
        }

        std::string describe(std::vector<std::string> const & violations)
        {
            std::ostringstream details;
            for (auto const & violation : violations)
            {
                details << '\n' << violation;
            }
            return details.str();
        }
    }

    TEST(WebGPUErrorScopeHygiene, ScanWebGpuSources_BrowserBuildNeverPumpsCallbacks)
    {
        std::filesystem::path const sourceDir{GLADIUS_WEBGPU_SOURCE_DIR};
        if (!std::filesystem::exists(sourceDir))
        {
            GTEST_SKIP() << "Missing WebGPU source directory: " << sourceDir.string();
        }

        std::vector<std::string> violations;
        std::size_t scannedFiles = 0u;
        for (auto const & entry : std::filesystem::directory_iterator{sourceDir})
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".cpp")
            {
                continue;
            }
            ++scannedFiles;
            auto const found = findBrowserBlockingPumps(entry.path());
            violations.insert(violations.end(), found.begin(), found.end());
        }

        ASSERT_GT(scannedFiles, 0u) << "No WebGPU sources found in " << sourceDir.string();
        EXPECT_TRUE(violations.empty())
          << "Code compiled for the browser build must not pump the host event loop or drive"
          << " compute::runBlocking. Use co_await compute::WaitUntil (stepped by the host loop),"
          << " or guard the call site with #ifdef __EMSCRIPTEN__ and throw"
          << " webgpu::makeBrowserBlockingUnavailableError. Violations:" << describe(violations);
    }

    TEST(WebGPUErrorScopeHygiene, ScanDocumentSource_BrowserBuildNeverPumpsCallbacks)
    {
        std::filesystem::path const document{
          std::filesystem::path{GLADIUS_WEBGPU_SOURCE_DIR}.parent_path() / "Document.cpp"};
        if (!std::filesystem::exists(document))
        {
            GTEST_SKIP() << "Missing Document.cpp: " << document.string();
        }

        auto const violations = findBrowserBlockingPumps(document);
        EXPECT_TRUE(violations.empty())
          << "Document.cpp pumps WebGPU callbacks outside a native-only branch:"
          << describe(violations);
    }
} // namespace gladius::tests
