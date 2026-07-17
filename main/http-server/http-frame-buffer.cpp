
#include "http-server/http-frame-buffer.h"
#include "app-globals.h"
#include "define.h"
#include <stdint.h>

HttpFrameBuffer::HttpFrameBuffer(size_t publishedFrameMaxBytes) : publishedFrameMaxBytes_(publishedFrameMaxBytes) {}

bool HttpFrameBuffer::initPublishedHttpFrameStore(size_t publishedFrameMaxBytes, uint32_t mallocCapSpiram)
{
    if (buffers[0]){
        delete[] buffers[0];
        buffers[0] = nullptr;
    }
    if (buffers[1]){
        delete[] buffers[1];
        buffers[1] = nullptr;
    }
    buffers[0] = (uint8_t*)heap_caps_malloc(publishedFrameMaxBytes, mallocCapSpiram);
    buffers[1] = (uint8_t*)heap_caps_malloc(publishedFrameMaxBytes, mallocCapSpiram);
    if (!buffers[0] || !buffers[1]) {
        ESP_LOGE(MAIN_TAG, "Failed to allocate published HTTP frame buffers");
        return false;
    }

    activeHttpFrameLength = 0;
    activeHttpFrameWidth = TF_IMAGE_INPUT_SIZE; // TODO inizializzare anche questi?
    activeHttpFrameHeight = TF_IMAGE_INPUT_SIZE;
    activeHttpFrameFormat = PIXFORMAT_GRAYSCALE;
    activeHttpFrameSeq = 0;
    activeHttpFramePublishUs = 0;
    return true;
}

void HttpFrameBuffer::publishHttpFrame(const uint8_t* src,
                                        size_t srcLen,
                                        uint16_t width,
                                        uint16_t height,
                                        pixformat_t format,
                                        int64_t publishUs)
{
    if (!src || srcLen == 0) {
        return;
    }

    size_t frameLen = srcLen;
    if (frameLen > publishedFrameMaxBytes_) {
        ESP_LOGW(HTTP_TAG,
                 "Published frame too large (%u > %u), truncating",
                 (unsigned int)frameLen,
                 (unsigned int)publishedFrameMaxBytes_);
        frameLen = publishedFrameMaxBytes_;
    }

    uint8_t publishIndex = activeHttpFrameIndex ^ 1;
    if (!buffers[publishIndex]) {
        return;
    }

    memcpy(buffers[publishIndex], src, frameLen);

    taskENTER_CRITICAL(&httpFrameMetaLock);
    activeHttpFrameLength = frameLen;
    activeHttpFrameWidth = width;
    activeHttpFrameHeight = height;
    activeHttpFrameFormat = format;
    activeHttpFrameIndex = publishIndex;
    activeHttpFrameSeq++;
    activeHttpFramePublishUs = publishUs;
    taskEXIT_CRITICAL(&httpFrameMetaLock);

    #if STOP_ACQUISITION_AFTER_1_FRAME
        if (activeHttpFrameSeq == 1) {
            pauseCameraAcquisition = true;
            ESP_LOGW(CAPTURE_TAG, "STOP_ACQUISITION_AFTER_1_FRAME active: acquisition paused after first frame");
        }
    #endif
}

bool HttpFrameBuffer::isBuffersInitialized(uint8_t index) const
{
    return index < FRAME_BUFFERS && buffers[index] != nullptr;
}

uint8_t* HttpFrameBuffer::getMutableBuffer(uint8_t index)
{
    return (index < FRAME_BUFFERS) ? buffers[index] : nullptr;
}

const uint8_t* HttpFrameBuffer::getBuffer(uint8_t index) const
{
    return (index < FRAME_BUFFERS) ? buffers[index] : nullptr;
}

uint8_t HttpFrameBuffer::getActiveHttpFrameIndex() const 
{ 
    return activeHttpFrameIndex; 
}

size_t HttpFrameBuffer::getActiveHttpFrameLength() const 
{ 
    return activeHttpFrameLength; 
}    

uint16_t HttpFrameBuffer::getActiveHttpFrameWidth() const 
{ 
    return activeHttpFrameWidth; 
}

uint16_t HttpFrameBuffer::getActiveHttpFrameHeight() const 
{ 
    return activeHttpFrameHeight; 
}

pixformat_t HttpFrameBuffer::getActiveHttpFrameFormat() const 
{ 
    return activeHttpFrameFormat; 
}

uint32_t HttpFrameBuffer::getActiveHttpFrameSeq() const 
{ 
    return activeHttpFrameSeq; 
}

int64_t HttpFrameBuffer::getActiveHttpFramePublishUs() const 
{ 
    return activeHttpFramePublishUs; 
}
