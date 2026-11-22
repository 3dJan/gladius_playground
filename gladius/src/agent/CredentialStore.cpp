#include "CredentialStore.h"

#include <array>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(__linux__)
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sago/platform_folders.h>
#include <sys/stat.h>
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

#if defined(__linux__)
        bool hasSecretTool()
        {
            return std::system("command -v secret-tool >/dev/null 2>&1") == 0;
        }

        std::filesystem::path encryptedStorePath()
        {
            return std::filesystem::path{sago::getConfigHome()} / "gladius" / ".credentials.enc";
        }

        std::string machinePassphrase()
        {
            std::string seed = "gladius.credential.v1:";
            std::ifstream mid("/etc/machine-id");
            if (mid)
            {
                std::string line;
                std::getline(mid, line);
                seed += line;
            }
            if (char const * user = std::getenv("USER"))
            {
                seed += ":";
                seed += user;
            }
            if (char const * home = std::getenv("HOME"))
            {
                seed += ":";
                seed += home;
            }
            return seed;
        }

        bool deriveKey(unsigned char const * salt, std::size_t saltLen, unsigned char * outKey)
        {
            std::string const pass = machinePassphrase();
            return PKCS5_PBKDF2_HMAC(pass.data(),
                                     static_cast<int>(pass.size()),
                                     salt,
                                     static_cast<int>(saltLen),
                                     10000,
                                     EVP_sha256(),
                                     32,
                                     outKey)
                   == 1;
        }

        nlohmann::json loadEncryptedMap()
        {
            auto const path = encryptedStorePath();
            std::error_code ec;
            if (!std::filesystem::exists(path, ec))
            {
                return nlohmann::json::object();
            }
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            if (!in)
            {
                return nlohmann::json::object();
            }
            auto const sz = in.tellg();
            if (sz < 44) // 16 salt + 12 iv + 16 tag
            {
                return nlohmann::json::object();
            }
            std::vector<unsigned char> blob(static_cast<std::size_t>(sz));
            in.seekg(0, std::ios::beg);
            in.read(reinterpret_cast<char *>(blob.data()), sz);

            unsigned char const * salt = blob.data();
            unsigned char const * iv = blob.data() + 16;
            unsigned char const * tag = blob.data() + 28;
            unsigned char const * ct = blob.data() + 44;
            int const ctLen = static_cast<int>(blob.size() - 44);

            std::array<unsigned char, 32> key{};
            if (!deriveKey(salt, 16, key.data()))
            {
                return nlohmann::json::object();
            }

            EVP_CIPHER_CTX * ctx = EVP_CIPHER_CTX_new();
            if (!ctx)
            {
                return nlohmann::json::object();
            }
            std::vector<unsigned char> pt(static_cast<std::size_t>(ctLen) + 16);
            int len = 0;
            int ptLen = 0;
            bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), iv) == 1
                      && EVP_DecryptUpdate(ctx, pt.data(), &len, ct, ctLen) == 1;
            if (ok)
            {
                ptLen = len;
                ok = EVP_CIPHER_CTX_ctrl(
                       ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<unsigned char *>(tag))
                       == 1
                     && EVP_DecryptFinal_ex(ctx, pt.data() + len, &len) == 1;
                ptLen += len;
            }
            EVP_CIPHER_CTX_free(ctx);
            if (!ok)
            {
                return nlohmann::json::object();
            }
            auto parsed = nlohmann::json::parse(
              std::string(reinterpret_cast<char const *>(pt.data()),
                          static_cast<std::size_t>(ptLen)),
              nullptr,
              false);
            return parsed.is_object() ? parsed : nlohmann::json::object();
        }

        bool saveEncryptedMap(nlohmann::json const & map)
        {
            auto const path = encryptedStorePath();
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);

            std::string const plain = map.dump();
            std::array<unsigned char, 16> salt{};
            std::array<unsigned char, 12> iv{};
            if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1
                || RAND_bytes(iv.data(), static_cast<int>(iv.size())) != 1)
            {
                return false;
            }
            std::array<unsigned char, 32> key{};
            if (!deriveKey(salt.data(), salt.size(), key.data()))
            {
                return false;
            }

            EVP_CIPHER_CTX * ctx = EVP_CIPHER_CTX_new();
            if (!ctx)
            {
                return false;
            }
            std::vector<unsigned char> ct(plain.size() + 16);
            std::array<unsigned char, 16> tag{};
            int len = 0;
            int ctLen = 0;
            bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), iv.data()) == 1
                      && EVP_EncryptUpdate(ctx,
                                           ct.data(),
                                           &len,
                                           reinterpret_cast<unsigned char const *>(plain.data()),
                                           static_cast<int>(plain.size()))
                           == 1;
            if (ok)
            {
                ctLen = len;
                ok = EVP_EncryptFinal_ex(ctx, ct.data() + len, &len) == 1;
                ctLen += len;
                ok = ok
                     && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) == 1;
            }
            EVP_CIPHER_CTX_free(ctx);
            if (!ok)
            {
                return false;
            }

            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                return false;
            }
            out.write(reinterpret_cast<char const *>(salt.data()),
                      static_cast<std::streamsize>(salt.size()));
            out.write(reinterpret_cast<char const *>(iv.data()),
                      static_cast<std::streamsize>(iv.size()));
            out.write(reinterpret_cast<char const *>(tag.data()),
                      static_cast<std::streamsize>(tag.size()));
            out.write(reinterpret_cast<char const *>(ct.data()), ctLen);
            out.close();
            ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
            return true;
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
        return hasSecretTool() ? "SecretService" : "EncryptedFile (AES-256-GCM)";
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
        OSStatus const status = SecKeychainAddGenericPassword(
          nullptr,
          static_cast<UInt32>(service.size()),
          service.data(),
          static_cast<UInt32>(account.size()),
          account.data(),
          static_cast<UInt32>(secret.size()),
          secret.data(),
          nullptr);
        if (status == errSecDuplicateItem)
        {
            SecKeychainItemRef item = nullptr;
            if (SecKeychainFindGenericPassword(nullptr,
                                               static_cast<UInt32>(service.size()),
                                               service.data(),
                                               static_cast<UInt32>(account.size()),
                                               account.data(),
                                               nullptr,
                                               nullptr,
                                               &item)
                  == errSecSuccess
                && item != nullptr)
            {
                OSStatus const upd = SecKeychainItemModifyAttributesAndData(
                  item, nullptr, static_cast<UInt32>(secret.size()), secret.data());
                CFRelease(item);
                return upd == errSecSuccess;
            }
            return false;
        }
        return status == errSecSuccess;
#elif defined(__linux__)
        if (hasSecretTool())
        {
            int pipefd[2]{};
            if (pipe(pipefd) == 0)
            {
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
                if (pid >= 0)
                {
                    (void) write(pipefd[1], secret.data(), secret.size());
                    close(pipefd[1]);
                    int status = 0;
                    if (waitpid(pid, &status, 0) >= 0 && WIFEXITED(status)
                        && WEXITSTATUS(status) == 0)
                    {
                        return true;
                    }
                }
                else
                {
                    close(pipefd[1]);
                }
            }
        }
        auto map = loadEncryptedMap();
        map[makeKey(service, account)] = secret;
        return saveEncryptedMap(map);
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
        void * data = nullptr;
        UInt32 length = 0;
        OSStatus const status = SecKeychainFindGenericPassword(
          nullptr,
          static_cast<UInt32>(service.size()),
          service.data(),
          static_cast<UInt32>(account.size()),
          account.data(),
          &length,
          &data,
          nullptr);
        if (status != errSecSuccess || data == nullptr)
        {
            return std::nullopt;
        }
        std::string secret(static_cast<char const *>(data), length);
        SecKeychainItemFreeContent(nullptr, data);
        return secret;
#elif defined(__linux__)
        if (hasSecretTool())
        {
            int pipefd[2]{};
            if (pipe(pipefd) == 0)
            {
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
                if (pid >= 0)
                {
                    std::string result;
                    char buf[512];
                    ssize_t n = 0;
                    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
                    {
                        result.append(buf, static_cast<std::size_t>(n));
                    }
                    close(pipefd[0]);
                    int status = 0;
                    if (waitpid(pid, &status, 0) >= 0 && WIFEXITED(status)
                        && WEXITSTATUS(status) == 0)
                    {
                        while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
                        {
                            result.pop_back();
                        }
                        if (!result.empty())
                        {
                            return result;
                        }
                    }
                }
                else
                {
                    close(pipefd[0]);
                }
            }
        }
        auto const map = loadEncryptedMap();
        std::string const k = makeKey(service, account);
        if (map.contains(k) && map[k].is_string())
        {
            return map[k].get<std::string>();
        }
        return std::nullopt;
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
        if (SecKeychainFindGenericPassword(nullptr,
                                           static_cast<UInt32>(service.size()),
                                           service.data(),
                                           static_cast<UInt32>(account.size()),
                                           account.data(),
                                           nullptr,
                                           nullptr,
                                           &item)
              == errSecSuccess
            && item != nullptr)
        {
            OSStatus const status = SecKeychainItemDelete(item);
            CFRelease(item);
            return status == errSecSuccess;
        }
        return true;
#elif defined(__linux__)
        if (hasSecretTool())
        {
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
            if (pid >= 0)
            {
                int status = 0;
                (void) waitpid(pid, &status, 0);
            }
        }
        auto map = loadEncryptedMap();
        std::string const k = makeKey(service, account);
        if (map.contains(k))
        {
            map.erase(k);
            return saveEncryptedMap(map);
        }
        return true;
#else
        (void) service;
        (void) account;
        return false;
#endif
    }
} // namespace gladius::agent
