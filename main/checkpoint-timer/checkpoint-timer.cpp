#include "checkpoint-timer/checkpoint-timer.h"
#include <esp_timer.h>
#include "debug.h"

// Constructor initializes the first checkpoint
CheckpointTimer::CheckpointTimer()
{
    checkpoint();
}

// Updates the checkpoint to the current time
void CheckpointTimer::checkpoint()
{
    last_checkpoint_us = esp_timer_get_time();
}

// Updates the checkpoint to the current time
void CheckpointTimer::logCheckpoint(std::string tag, std::string message)
{
    int32_t elapsed = elapsedMs();       // compute elapsed BEFORE updating checkpoint
    ESP_LOGI(tag.c_str(), "*** Elapsed: %" PRId32 " ms: %s ***", elapsed, message.c_str());
    checkpoint();                        // now update the checkpoint
}

// Returns elapsed time in microseconds
int64_t CheckpointTimer::elapsedUs() const
{
    return esp_timer_get_time() - last_checkpoint_us;
}

// Returns elapsed time in milliseconds
int32_t CheckpointTimer::elapsedMs() const
{
    return static_cast<int32_t>(elapsedUs() / 1000);
}