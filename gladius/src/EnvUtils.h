#pragma once

#include <cstdlib>

namespace gladius
{
    /// Returns true if the named environment variable is set to any non-empty value.
    /// Uses getenv_s on Windows and secure_getenv on POSIX to avoid deprecation
    /// warnings and substitution attacks in setuid processes.
    [[nodiscard]] inline bool isEnvVarSet(char const * name) noexcept
    {
#ifdef _WIN32
        std::size_t requiredSize = 0U;
        getenv_s(&requiredSize, nullptr, 0U, name);
        return requiredSize > 0U;
#else
        char const * const value = ::secure_getenv(name);
        return value != nullptr && value[0] != '\0';
#endif
    }

} // namespace gladius
