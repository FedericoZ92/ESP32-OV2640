#pragma once

#include "define.h"

#include <freertos/FreeRTOS.h>

#include "led/rgb-led.h"
#include "led/red-led.h"

#include "checkpoint-timer/checkpoint-timer.h" //A


extern RgbLedController rgb;
extern RedLedController redLed;

extern volatile bool pauseCameraAcquisition;
extern portMUX_TYPE httpFrameMetaLock;

class CameraHttpServer;
class HttpFrameBuffer;
class FrameMailboxManager;

struct AppTaskContext {
    CameraHttpServer* server = nullptr;
    HttpFrameBuffer* httpFrameBuffer = nullptr;
    FrameMailboxManager* streamMailboxManager = nullptr;
    FrameMailboxManager* inferenceMailboxManager = nullptr;
    portMUX_TYPE* httpFrameMetaLock = nullptr;
};

// A
extern CheckpointTimer jpegTimer;
extern CheckpointTimer cameraAcquisitionTimer;
extern CheckpointTimer tensorFlowTimer;
extern CheckpointTimer httpTimer;

// Large enough for QQVGA grayscale and worst-case QQVGA JPEG payload bursts.
constexpr size_t kPublishedFrameMaxBytes =
    (JPEG_BUFFER_SIZE > (160 * 120 * 2) ? JPEG_BUFFER_SIZE : (160 * 120 * 2));

// The current inference camera mode is QQVGA, so a full grayscale scratch frame fits here.
constexpr size_t kAcquiredFrameMaxPixels = 160 * 120;


