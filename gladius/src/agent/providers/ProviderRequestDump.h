#pragma once

#include <filesystem>
#include <cstddef>

#include <nlohmann/json.hpp>

namespace gladius::agent
{
    /**
     * @brief Optional per-request payload dump used to explain prompt-cache behaviour
     *
     * Enabled with `GLADIUS_AGENT_PROVIDER_DUMP=<directory>`. Every completion request is written as
     * one JSON file, with base64 image payloads replaced by their size so the files stay small and
     * consecutive turns can be diffed byte by byte. That is the only way to tell which part of an
     * already sent prefix changed and invalidated the cache.
     */
    class ProviderRequestDump
    {
    public:
        ProviderRequestDump();

        /// @brief True when a dump directory is configured and usable
        bool isEnabled() const;

        /// @brief Write one request payload to the next numbered dump file
        void write(nlohmann::json const & payload) const;

    private:
        static nlohmann::json withRedactedImages(nlohmann::json const & node);

    private:
        std::filesystem::path m_directory;
        mutable std::size_t m_sequence = 0;
    };
}
