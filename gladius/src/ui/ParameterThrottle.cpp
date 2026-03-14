#include "ParameterThrottle.h"

namespace gladius::ui
{
    ParameterThrottle::ParameterThrottle(std::chrono::milliseconds debounceInterval)
        : m_debounceInterval(debounceInterval)
    {
    }

    bool ParameterThrottle::onParameterChanged()
    {
        m_lastChangeTime = std::chrono::steady_clock::now();
        m_pendingRecompile = true;

        if (m_firstCall)
        {
            m_firstCall = false;
            m_pendingRecompile = false;
            return true; // Immediate recompile on first change
        }

        return false;
    }

    bool ParameterThrottle::shouldRecompile()
    {
        if (!m_pendingRecompile)
        {
            return false;
        }

        auto const now = std::chrono::steady_clock::now();
        auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - m_lastChangeTime);

        if (elapsed >= m_debounceInterval)
        {
            m_pendingRecompile = false;
            return true;
        }

        return false;
    }

    bool ParameterThrottle::hasPendingRecompile() const
    {
        return m_pendingRecompile;
    }

    void ParameterThrottle::setDebounceInterval(std::chrono::milliseconds debounceInterval)
    {
        m_debounceInterval = debounceInterval;
    }

    std::chrono::milliseconds ParameterThrottle::debounceInterval() const
    {
        return m_debounceInterval;
    }

    void ParameterThrottle::reset()
    {
        m_pendingRecompile = false;
        m_firstCall = true;
        m_lastChangeTime = {};
    }
} // namespace gladius::ui
