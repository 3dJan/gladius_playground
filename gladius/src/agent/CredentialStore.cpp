#include "CredentialStore.h"

#include <cstdlib>
#include <cstdio>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace gladius::agent
{
    namespace
    {
        std::string makeKey(std::string const & service, std::string const & account)
        {
            return service + ":" + account;
        }

#if defined(__APPLE__)
#ifdef __clang__
        // The SecKeychain C API is deprecated since macOS 10.10 but is still functional. It is kept as an
        // isolated adapter so the rest of the file stays warning free.
        // TODO: Migrate the adapter below to the modern SecItem API.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

        /// @brief RAII guard that releases a SecKeychainItemRef.
        class KeychainItemGuard
        {
          public:
            explicit KeychainItemGuard(SecKeychainItemRef item)
              : m_item(item)
            {
            }

            KeychainItemGuard(KeychainItemGuard const &) = delete;
            KeychainItemGuard & operator=(KeychainItemGuard const &) = delete;

            ~KeychainItemGuard()
            {
                if (m_item != nullptr)
                {
                    CFRelease(m_item);
                }
            }

          private:
            SecKeychainItemRef m_item;
        };

        OSStatus addToKeychain(std::string const & service, std::string const & account, std::string const & secret)
        {
            return SecKeychainAddGenericPassword(nullptr,
                                                 static_cast<UInt32>(service.size()),
                                                 service.data(),
                                                 static_cast<UInt32>(account.size()),
                                                 account.data(),
                                                 static_cast<UInt32>(secret.size()),
                                                 secret.data(),
                                                 nullptr);
        }

        /// @brief Looks up an existing generic password item without loading its secret.
        OSStatus findKeychainItem(std::string const & service, std::string const & account, SecKeychainItemRef & item)
        {
            return SecKeychainFindGenericPassword(nullptr,
                                                  static_cast<UInt32>(service.size()),
                                                  service.data(),
                                                  static_cast<UInt32>(account.size()),
                                                  account.data(),
                                                  nullptr,
                                                  nullptr,
                                                  &item);
        }

        OSStatus updateKeychainItem(SecKeychainItemRef item, std::string const & secret)
        {
            return SecKeychainItemModifyAttributesAndData(item,
                                                          nullptr,
                                                          static_cast<UInt32>(secret.size()),
                                                          secret.data());
        }

        /// @brief Copies the secret of an existing generic password item into secret.
        OSStatus findKeychainSecret(std::string const & service, std::string const & account, std::string & secret)
        {
            void * data = nullptr;
            UInt32 length = 0;
            OSStatus const status = SecKeychainFindGenericPassword(nullptr,
                                                                   static_cast<UInt32>(service.size()),
                                                                   service.data(),
                                                                   static_cast<UInt32>(account.size()),
                                                                   account.data(),
                                                                   &length,
                                                                   &data,
                                                                   nullptr);
            if (status != errSecSuccess || data == nullptr)
            {
                return status;
            }
            secret.assign(static_cast<char const *>(data), length);
            SecKeychainItemFreeContent(nullptr, data);
            return errSecSuccess;
        }

        OSStatus deleteKeychainItem(SecKeychainItemRef item)
        {
            return SecKeychainItemDelete(item);
        }

#ifdef __clang__
#pragma clang diagnostic pop
#endif
#endif

#if defined(__linux__)
        bool hasSecretTool()
        {
            return std::system("command -v secret-tool >/dev/null 2>&1") == 0;
        }
#endif
    } // namespace

    bool InMemoryCredentialStore::storeSecret(std::string const & service,
                                              std::string const & account,
                                              std::string const & secret)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_secrets[makeKey(service, account)] = secret;
        return true;
    }

    std::optional<std::string> InMemoryCredentialStore::loadSecret(std::string const & service,
                                                                   std::string const & account) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto const it = m_secrets.find(makeKey(service, account));
        if (it == m_secrets.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    bool InMemoryCredentialStore::deleteSecret(std::string const & service, std::string const & account)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_secrets.erase(makeKey(service, account));
        return true;
    }

    std::string OsCredentialStore::backendName() const
    {
#if defined(_WIN32)
        return "WinCred";
#elif defined(__APPLE__)
        return "Keychain";
#elif defined(__linux__)
        return "SecretService";
#else
        return "Unsupported";
#endif
    }

    bool OsCredentialStore::storeSecret(std::string const & service,
                                        std::string const & account,
                                        std::string const & secret)
    {
#if defined(_WIN32)
        std::string const target = makeKey(service, account);
        CREDENTIALA cred{};
        cred.Type = CRED_TYPE_GENERIC;
        cred.TargetName = const_cast<LPSTR>(target.c_str());
        cred.CredentialBlobSize = static_cast<DWORD>(secret.size());
        cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(secret.data()));
        cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
        cred.UserName = const_cast<LPSTR>(account.c_str());
        return CredWriteA(&cred, 0) == TRUE;
#elif defined(__APPLE__)
        OSStatus const status = addToKeychain(service, account, secret);
        if (status == errSecDuplicateItem)
        {
            SecKeychainItemRef item = nullptr;
            if (findKeychainItem(service, account, item) == errSecSuccess && item != nullptr)
            {
                KeychainItemGuard const guard(item);
                return updateKeychainItem(item, secret) == errSecSuccess;
            }
            return false;
        }
        return status == errSecSuccess;
#elif defined(__linux__)
        if (!hasSecretTool())
        {
            return false;
        }
        int pipefd[2]{};
        if (pipe(pipefd) != 0)
        {
            return false;
        }
        pid_t const pid = fork();
        if (pid == 0)
        {
            close(pipefd[1]);
            dup2(pipefd[0], STDIN_FILENO);
            close(pipefd[0]);
            std::string const label = "Gladius: " + service + " (" + account + ")";
            execlp("secret-tool",
                   "secret-tool",
                   "store",
                   "--label",
                   label.c_str(),
                   "service",
                   service.c_str(),
                   "account",
                   account.c_str(),
                   static_cast<char *>(nullptr));
            _exit(127);
        }
        close(pipefd[0]);
        if (pid < 0)
        {
            close(pipefd[1]);
            return false;
        }
        (void) write(pipefd[1], secret.data(), secret.size());
        close(pipefd[1]);
        int status = 0;
        if (waitpid(pid, &status, 0) < 0)
        {
            return false;
        }
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
        (void) service;
        (void) account;
        (void) secret;
        return false;
#endif
    }

    std::optional<std::string> OsCredentialStore::loadSecret(std::string const & service,
                                                             std::string const & account) const
    {
#if defined(_WIN32)
        std::string const target = makeKey(service, account);
        PCREDENTIALA cred = nullptr;
        if (CredReadA(target.c_str(), CRED_TYPE_GENERIC, 0, &cred) != TRUE || cred == nullptr)
        {
            return std::nullopt;
        }
        std::string secret(reinterpret_cast<char const *>(cred->CredentialBlob), cred->CredentialBlobSize);
        CredFree(cred);
        return secret;
#elif defined(__APPLE__)
        std::string secret;
        if (findKeychainSecret(service, account, secret) != errSecSuccess)
        {
            return std::nullopt;
        }
        return secret;
#elif defined(__linux__)
        if (!hasSecretTool())
        {
            return std::nullopt;
        }
        int pipefd[2]{};
        if (pipe(pipefd) != 0)
        {
            return std::nullopt;
        }
        pid_t const pid = fork();
        if (pid == 0)
        {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);
            execlp("secret-tool",
                   "secret-tool",
                   "lookup",
                   "service",
                   service.c_str(),
                   "account",
                   account.c_str(),
                   static_cast<char *>(nullptr));
            _exit(127);
        }
        close(pipefd[1]);
        if (pid < 0)
        {
            close(pipefd[0]);
            return std::nullopt;
        }
        std::string result;
        char buf[512];
        ssize_t n = 0;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
        {
            result.append(buf, static_cast<std::size_t>(n));
        }
        close(pipefd[0]);
        int status = 0;
        if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            return std::nullopt;
        }
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        {
            result.pop_back();
        }
        return result;
#else
        (void) service;
        (void) account;
        return std::nullopt;
#endif
    }

    bool OsCredentialStore::deleteSecret(std::string const & service, std::string const & account)
    {
#if defined(_WIN32)
        std::string const target = makeKey(service, account);
        return CredDeleteA(target.c_str(), CRED_TYPE_GENERIC, 0) == TRUE;
#elif defined(__APPLE__)
        SecKeychainItemRef item = nullptr;
        if (findKeychainItem(service, account, item) == errSecSuccess && item != nullptr)
        {
            KeychainItemGuard const guard(item);
            return deleteKeychainItem(item) == errSecSuccess;
        }
        return true;
#elif defined(__linux__)
        if (!hasSecretTool())
        {
            return false;
        }
        pid_t const pid = fork();
        if (pid == 0)
        {
            execlp("secret-tool",
                   "secret-tool",
                   "clear",
                   "service",
                   service.c_str(),
                   "account",
                   account.c_str(),
                   static_cast<char *>(nullptr));
            _exit(127);
        }
        if (pid < 0)
        {
            return false;
        }
        int status = 0;
        if (waitpid(pid, &status, 0) < 0)
        {
            return false;
        }
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
        (void) service;
        (void) account;
        return false;
#endif
    }
} // namespace gladius::agent
