#pragma once

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <tracy/Tracy.hpp>
#include <utility>

// Temporarily enable detailed FWN preparation timings. These logs intentionally
// use std::clog (stderr) instead of std::cout so MCP stdio framing on stdout
// remains untouched. Set to 0 before production/release use.
#ifndef GLADIUS_ENABLE_FWN_PREP_TIMING_LOGS
#define GLADIUS_ENABLE_FWN_PREP_TIMING_LOGS 1
#endif

// Enable debug output for async rendering diagnostics
//#define ASYNC_DEBUG_OUTPUT

namespace gladius
{
    class ScopedProfilingFrame
    {
      public:
        ScopedProfilingFrame(const std::string & name)
            : m_name(name)
        {
            FrameMarkStart(m_name.c_str());
        }

        ~ScopedProfilingFrame()
        {
            FrameMarkEnd(m_name.c_str());
        }

      private:
        const std::string m_name;
    };

#define LOG_LOCATION
    // #define LOG_LOCATION std::cout << "Method: " << __FUNCTION__ << " line: " << __LINE__ << ",
    // Thread ID: " << std::this_thread::get_id() << std::endl; #define ProfileFunction ZoneScoped;

#ifdef ASYNC_DEBUG_OUTPUT
    /// For debugging: prints text to console instead of Tracy
    #define DebugText(text, len) std::cout << "[" << std::this_thread::get_id() << "] " << text << std::endl
    #define DebugValue(value) std::cout << "[" << std::this_thread::get_id() << "] " << #value << " = " << value << std::endl
#else
    /// For profiling: sends text to Tracy
    #define DebugText(text, len) ZoneText(text, len)
    #define DebugValue(value) ZoneValue(value)
#endif

    class ScopedTimeLogger
    {
      public:
        ScopedTimeLogger(const std::string & name)
            : m_name(name)
        {
            m_start = std::chrono::high_resolution_clock::now();
        }

        ~ScopedTimeLogger()
        {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - m_start);
            constexpr int64_t threshold = 100; // Only log if > 100ms

            if (duration.count() > threshold)
            {
                std::cout << m_name << " took " << duration.count() << "ms" << std::endl;
            }
        }

      private:
        std::string m_name;
        std::chrono::time_point<std::chrono::high_resolution_clock> m_start;
    };

    class ScopedFwnPrepTimingLogger
    {
      public:
        explicit ScopedFwnPrepTimingLogger(std::string name, bool enabled = true)
            : m_name(std::move(name))
            , m_enabled(enabled)
            , m_start(std::chrono::high_resolution_clock::now())
        {
        }

        ~ScopedFwnPrepTimingLogger()
        {
            if (!m_enabled)
            {
                return;
            }

            auto const end = std::chrono::high_resolution_clock::now();
            auto const duration = std::chrono::duration<double, std::milli>(end - m_start);
            std::clog << "[FWN prep] " << m_name << " took " << duration.count() << " ms" << std::endl;
        }

      private:
        std::string m_name;
        bool m_enabled = true;
        std::chrono::time_point<std::chrono::high_resolution_clock> m_start;
    };

    inline void logFwnPrepTiming(std::string const & message)
    {
        std::clog << "[FWN prep] " << message << std::endl;
    }

#define GLADIUS_FWN_PREP_JOIN_INNER(left, right) left##right
#define GLADIUS_FWN_PREP_JOIN(left, right) GLADIUS_FWN_PREP_JOIN_INNER(left, right)

#if GLADIUS_ENABLE_FWN_PREP_TIMING_LOGS
#define GLADIUS_FWN_PREP_SCOPE(name)                                                              \
    ::gladius::ScopedFwnPrepTimingLogger GLADIUS_FWN_PREP_JOIN(fwnPrepTimer_, __LINE__)(name)
#define GLADIUS_FWN_PREP_SCOPE_IF(name, enabled)                                                  \
    ::gladius::ScopedFwnPrepTimingLogger GLADIUS_FWN_PREP_JOIN(fwnPrepTimer_, __LINE__)(name, enabled)
#define GLADIUS_FWN_PREP_LOG(message) ::gladius::logFwnPrepTiming(message)
#define GLADIUS_FWN_PREP_LOG_IF(condition, message)                                               \
    do                                                                                             \
    {                                                                                              \
        if (condition)                                                                             \
        {                                                                                          \
            ::gladius::logFwnPrepTiming(message);                                                  \
        }                                                                                          \
    } while (false)
#else
#define GLADIUS_FWN_PREP_SCOPE(name) ((void) 0)
#define GLADIUS_FWN_PREP_SCOPE_IF(name, enabled) ((void) 0)
#define GLADIUS_FWN_PREP_LOG(message) ((void) 0)
#define GLADIUS_FWN_PREP_LOG_IF(condition, message) ((void) 0)
#endif

#define LOG_SCOPE_DURATION ScopedTimeLogger scopedTimeLogger(__FUNCTION__);
#define LOG_SCOPE_DURATION_NAMED(name) ScopedTimeLogger scopedTimeLogger(name);
// #define ProfileFunction LOG_SCOPE_DURATION
#define ProfileFunction ZoneScoped;
// #define ProfileFunction
}