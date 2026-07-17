#pragma once

#include "define.h"
#include <stdint.h>
#include <sensor.h>
#include <cstddef>

#define FRAME_BUFFERS 2

class HttpFrameBuffer 
{
public:
    HttpFrameBuffer(size_t publishedFrameMaxBytes);

    bool initPublishedHttpFrameStore(size_t publishedFrameMaxBytes, uint32_t mallocCapSpiram); //MALLOC_CAP_SPIRAM

    void publishHttpFrame(const uint8_t* src,
                            size_t srcLen,
                            uint16_t width,
                            uint16_t height,
                            pixformat_t format,
                            int64_t publishUs);

    bool isBuffersInitialized(uint8_t index) const;
    uint8_t* getMutableBuffer(uint8_t index);
    const uint8_t* getBuffer(uint8_t index) const;
    uint8_t getActiveHttpFrameIndex() const;
    size_t getActiveHttpFrameLength() const;
    uint16_t getActiveHttpFrameWidth() const;
    uint16_t getActiveHttpFrameHeight() const;
    pixformat_t getActiveHttpFrameFormat() const;
    uint32_t getActiveHttpFrameSeq() const;
    int64_t getActiveHttpFramePublishUs() const;

private:
    uint8_t* buffers[FRAME_BUFFERS] = {nullptr, nullptr};
    volatile uint8_t activeHttpFrameIndex = 0;
    volatile size_t activeHttpFrameLength = 0;
    volatile uint16_t activeHttpFrameWidth = 0; //TF_IMAGE_INPUT_SIZE;
    volatile uint16_t activeHttpFrameHeight = 0; //TF_IMAGE_INPUT_SIZE;
    volatile pixformat_t activeHttpFrameFormat = PIXFORMAT_GRAYSCALE; //?
    volatile uint32_t activeHttpFrameSeq = 0;
    volatile int64_t activeHttpFramePublishUs = 0;
    size_t publishedFrameMaxBytes_ = 0;

};

extern HttpFrameBuffer httpFrameBuffer;        

