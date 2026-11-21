#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace gladius::agent
{
    /// @brief Interface for storing and retrieving BYOK API keys securely
    class ICredentialStore
    {
      public:
        virtual ~ICredentialStore() = default;

        /// @brief Store a secret under service + account key
        virtual bool storeSecret(std::string const & service,
                                 std::string const & account,
                                 std::string const & secret) = 0;

        /// @brief Retrieve a stored secret, or std::nullopt if absent or unavailable
        [[nodiscard]] virtual std::optional<std::string>
        loadSecret(std::string const & service, std::string const & account) const = 0;

        /// @brief Delete a stored secret; returns true if removed or not present
        virtual bool deleteSecret(std::string const & service, std::string const & account) = 0;

        /// @brief Human-readable backend name (e.g. "WinCred", "Keychain", "SecretService", "Memory")
        [[nodiscard]] virtual std::string backendName() const = 0;
    };

    /// @brief OS-backed credential store (Windows Credential Manager, macOS Keychain, Linux Secret Service)
    class OsCredentialStore final : public ICredentialStore
    {
      public:
        bool storeSecret(std::string const & service,
                         std::string const & account,
                         std::string const & secret) override;

        [[nodiscard]] std::optional<std::string>
        loadSecret(std::string const & service, std::string const & account) const override;

        bool deleteSecret(std::string const & service, std::string const & account) override;

        [[nodiscard]] std::string backendName() const override;
    };

    /// @brief Ephemeral in-memory credential store for tests and headless sessions without a keyring
    class InMemoryCredentialStore final : public ICredentialStore
    {
      public:
        bool storeSecret(std::string const & service,
                         std::string const & account,
                         std::string const & secret) override;

        [[nodiscard]] std::optional<std::string>
        loadSecret(std::string const & service, std::string const & account) const override;

        bool deleteSecret(std::string const & service, std::string const & account) override;

        [[nodiscard]] std::string backendName() const override
        {
            return "Memory";
        }

      private:
        mutable std::mutex m_mutex;
        std::unordered_map<std::string, std::string> m_secrets;
    };
} // namespace gladius::agent
