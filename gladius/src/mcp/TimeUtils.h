#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace gladius::mcp
{
    /// Format a time_point as ISO-8601 UTC string (e.g. "2026-03-17T10:00:00.123Z").
    inline std::string formatIso8601Utc(std::chrono::system_clock::time_point const & tp)
    {
        auto time = std::chrono::system_clock::to_time_t(tp);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &time);
#else
        gmtime_r(&time, &utc);
#endif

        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                        tp.time_since_epoch()) %
                      1000;

        char buf[32];
        std::snprintf(buf,
                      sizeof(buf),
                      "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                      utc.tm_year + 1900,
                      utc.tm_mon + 1,
                      utc.tm_mday,
                      utc.tm_hour,
                      utc.tm_min,
                      utc.tm_sec,
                      static_cast<int>(millis.count()));
        return buf;
    }

    /// Parse an ISO-8601 UTC timestamp to a time_point. Returns epoch on failure.
    inline std::chrono::system_clock::time_point parseIso8601Utc(std::string const & iso)
    {
        std::tm tm{};
        int millis = 0;

        // Try full format with milliseconds first: "2026-03-17T10:00:00.123Z"
#ifdef _WIN32
        if (sscanf_s(iso.c_str(),
#else
        if (std::sscanf(iso.c_str(),
#endif
                        "%4d-%2d-%2dT%2d:%2d:%2d.%3dZ",
                        &tm.tm_year,
                        &tm.tm_mon,
                        &tm.tm_mday,
                        &tm.tm_hour,
                        &tm.tm_min,
                        &tm.tm_sec,
                        &millis) < 6)
        {
            // Fallback: try without milliseconds: "2026-03-17T10:00:00Z"
#ifdef _WIN32
            if (sscanf_s(iso.c_str(),
#else
            if (std::sscanf(iso.c_str(),
#endif
                            "%4d-%2d-%2dT%2d:%2d:%2dZ",
                            &tm.tm_year,
                            &tm.tm_mon,
                            &tm.tm_mday,
                            &tm.tm_hour,
                            &tm.tm_min,
                            &tm.tm_sec) < 6)
            {
                return std::chrono::system_clock::time_point{}; // epoch
            }
        }

        // Basic range validation (Fix #8)
        if (tm.tm_mon < 1 || tm.tm_mon > 12 ||
            tm.tm_mday < 1 || tm.tm_mday > 31 ||
            tm.tm_hour < 0 || tm.tm_hour > 23 ||
            tm.tm_min < 0 || tm.tm_min > 59 ||
            tm.tm_sec < 0 || tm.tm_sec > 60) // 60 for leap second
        {
            return std::chrono::system_clock::time_point{}; // epoch
        }

        tm.tm_year -= 1900;
        tm.tm_mon -= 1;

#ifdef _WIN32
        auto time = _mkgmtime(&tm);
#else
        auto time = timegm(&tm);
#endif
        return std::chrono::system_clock::from_time_t(time) +
               std::chrono::milliseconds(millis);
    }

} // namespace gladius::mcp
