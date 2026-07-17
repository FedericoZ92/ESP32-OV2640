#pragma once

#include "esp_camera.h"
#include "esp_err.h"
#include "frame-snapshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include <cstdint>

struct FrameMailbox {
    uint8_t* buffers[2] = {nullptr, nullptr};
    volatile uint8_t activeIndex = 0;
    volatile size_t frameLen = 0;
    volatile uint16_t frameWidth = 0;
    volatile uint16_t frameHeight = 0;
    volatile pixformat_t frameFormat = PIXFORMAT_GRAYSCALE;
    volatile uint32_t frameSeq = 0;
    volatile int64_t frameCaptureUs = 0;
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    TaskHandle_t consumerTaskHandle = nullptr;
    const char* tag = nullptr;
};

class FrameMailboxManager 
{
public:
    FrameMailboxManager(FrameMailbox* mailbox, size_t publishedFrameMaxBytes);
    ~FrameMailboxManager() = default;
    bool initFrameMailbox(const char* tag, uint32_t caps);
    void publish( const uint8_t* src,
                    size_t srcLen,
                    uint16_t width,
                    uint16_t height,
                    pixformat_t format,
                    int64_t captureUs);
    bool snapshot(FrameSnapshot* snapshot);

private:
    FrameMailbox* mailbox;
    size_t publishedFrameMaxBytes_ = 0;
};

extern FrameMailbox inferenceMailbox;
extern FrameMailbox streamMailbox;
extern FrameMailboxManager inferenceMailboxManager;
extern FrameMailboxManager streamMailboxManager;

