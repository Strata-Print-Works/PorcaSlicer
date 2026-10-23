#pragma once

#include <chrono>
#include <optional>

namespace Slic3r::GUI {

// The caller resets this when focus, selection or another input interrupts the gesture.
class SelectAllSequence
{
public:
    using Clock = std::chrono::steady_clock;

    bool press(Clock::time_point now, int plate, long input_timestamp = 0)
    {
        // Input may wait behind selection/sidebar rebuilding on large projects.
        // Compare the native key times when available, not callback arrival times.
        const bool quick = input_timestamp > 0 && m_last_input_timestamp > 0 ?
            input_timestamp >= m_last_input_timestamp && input_timestamp - m_last_input_timestamp <= 500 :
            m_last_press && now >= *m_last_press && now - *m_last_press <= std::chrono::milliseconds(500);
        const bool expand = m_last_press && plate == m_plate && quick;
        m_last_press = now;
        m_last_input_timestamp = input_timestamp;
        m_plate = plate;
        return expand;
    }

    void reset() { m_last_press.reset(); }

private:
    std::optional<Clock::time_point> m_last_press;
    long m_last_input_timestamp {0};
    int m_plate {-1};
};

} // namespace Slic3r::GUI
