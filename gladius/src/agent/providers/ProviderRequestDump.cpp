#include "ProviderRequestDump.h"

#include <fstream>
#include <iomanip>
#include <sstream>

#include <cstdlib>

namespace gladius::agent
{
    namespace
    {
        char const * const kDumpDirectoryEnv = "GLADIUS_AGENT_PROVIDER_DUMP";
    }

    ProviderRequestDump::ProviderRequestDump()
    {
        char const * raw = std::getenv(kDumpDirectoryEnv);
        if (raw == nullptr || *raw == '\0')
        {
            return;
        }

        m_directory = raw;
        std::error_code ec;
        std::filesystem::create_directories(m_directory, ec);
        if (ec)
        {
            m_directory.clear();
        }
    }

    bool ProviderRequestDump::isEnabled() const
    {
        return !m_directory.empty();
    }

    nlohmann::json ProviderRequestDump::withRedactedImages(nlohmann::json const & node)
    {
        if (node.is_array())
        {
            nlohmann::json result = nlohmann::json::array();
            for (auto const & child : node)
            {
                result.push_back(withRedactedImages(child));
            }
            return result;
        }

        if (!node.is_object())
        {
            return node;
        }

        if (auto it = node.find("source"); it != node.end() && it->is_object() && it->contains("data"))
        {
            nlohmann::json redacted = node;
            auto const & data = it->at("data");
            redacted["source"] = {{"type", "base64"}, {"bytes", data.is_string() ? data.get<std::string>().size() : 0}};
            return redacted;
        }

        nlohmann::json result = nlohmann::json::object();
        for (auto it = node.begin(); it != node.end(); ++it)
        {
            result[it.key()] = withRedactedImages(it.value());
        }
        return result;
    }

    void ProviderRequestDump::write(nlohmann::json const & payload) const
    {
        if (!isEnabled())
        {
            return;
        }

        std::ostringstream name;
        name << "request-" << std::setw(3) << std::setfill('0') << m_sequence++ << ".json";

        std::ofstream out(m_directory / name.str());
        if (!out.is_open())
        {
            return;
        }
        out << withRedactedImages(payload).dump(2);
    }
}
