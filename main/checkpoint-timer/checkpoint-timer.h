#pragma once

#include <cstdint>
#include <string>

// Tracks elapsed time since the latest checkpoint
// Uses esp_timer_get_time() (microseconds since boot)
class CheckpointTimer
{
public:
    // Initializes and sets the first checkpoint
    CheckpointTimer();

    void checkpoint();

    // Marks the current time as the latest checkpoint
    void checkpoint(std::string tag, std::string message);

    // Returns microseconds since the last checkpoint
    int64_t elapsedUs() const;

    // Returns milliseconds since the last checkpoint
    int32_t elapsedMs() const;

private:
    // Timestamp of the last checkpoint in microseconds
    int64_t last_checkpoint_us;
};
