
#include "data-types/frame-mailbox.h"
#include "esp_log.h"
#include "define.h"
#include "debug.h"
#include <esp_heap_caps.h>
#include <cstring>

FrameMailboxManager::FrameMailboxManager(FrameMailbox* mailbox, size_t publishedFrameMaxBytes)
    : mailbox(mailbox), publishedFrameMaxBytes_(publishedFrameMaxBytes) {}

bool FrameMailboxManager::initFrameMailbox(const char* tag, uint32_t caps) //caps MALLOC_CAP_SPIRAM
{
    if (!mailbox) {
        return false;
    }
    mailbox->tag = tag;
    if (mailbox->buffers[0]){
        delete[] mailbox->buffers[0];
        mailbox->buffers[0] = nullptr;
    }
    if (mailbox->buffers[1]){
        delete[] mailbox->buffers[1];
        mailbox->buffers[1] = nullptr;
    }
    mailbox->buffers[0] = (uint8_t*)heap_caps_malloc(publishedFrameMaxBytes_, caps);
    mailbox->buffers[1] = (uint8_t*)heap_caps_malloc(publishedFrameMaxBytes_, caps);
    if (!mailbox->buffers[0] || !mailbox->buffers[1]) {
        ESP_LOGE(MAIN_TAG, "Failed to allocate mailbox buffers for %s", tag ? tag : "unknown");
        return false;
    }
    mailbox->activeIndex = 0;
    mailbox->frameLen = 0;
    mailbox->frameWidth = 0;
    mailbox->frameHeight = 0;
    mailbox->frameFormat = PIXFORMAT_GRAYSCALE;
    mailbox->frameSeq = 0;
    mailbox->frameCaptureUs = 0;
    mailbox->consumerTaskHandle = nullptr;
    return true;
}

void FrameMailboxManager::publish(const uint8_t* cameraFrameSrc,
                                    size_t srcLen,
                                    uint16_t width,
                                    uint16_t height,
                                    pixformat_t format,
                                    int64_t captureUs)
{
    if (!cameraFrameSrc || !mailbox || srcLen == 0) {
        return;
    }

    size_t frameLen = srcLen;
    if (frameLen > publishedFrameMaxBytes_) {
        ESP_LOGW(MAIN_TAG,
                 "Mailbox %s frame too large (%u > %u), truncating",
                 mailbox->tag ? mailbox->tag : "unknown",
                 (unsigned int)frameLen,
                 (unsigned int)publishedFrameMaxBytes_);
        frameLen = publishedFrameMaxBytes_;
    }

    uint8_t publishIndex = mailbox->activeIndex ^ 1;
    if (!mailbox->buffers[publishIndex]) {
        return;
    }

    memcpy(mailbox->buffers[publishIndex], cameraFrameSrc, frameLen);
    taskENTER_CRITICAL(&mailbox->lock);
    mailbox->frameLen = frameLen;
    mailbox->frameWidth = width;
    mailbox->frameHeight = height;
    mailbox->frameFormat = format;
    mailbox->activeIndex = publishIndex;
    mailbox->frameSeq++;
    mailbox->frameCaptureUs = captureUs;
    taskEXIT_CRITICAL(&mailbox->lock);

    if (mailbox->consumerTaskHandle) {
        xTaskNotifyGive(mailbox->consumerTaskHandle);
    }
}

bool FrameMailboxManager::snapshot(FrameSnapshot* snapshot)
{
    if (!mailbox || !snapshot) {
        return false;
    }

    taskENTER_CRITICAL(&mailbox->lock);
    const uint8_t activeIndex = mailbox->activeIndex;
    snapshot->len = mailbox->frameLen;
    snapshot->width = mailbox->frameWidth;
    snapshot->height = mailbox->frameHeight;
    snapshot->format = mailbox->frameFormat;
    snapshot->seq = mailbox->frameSeq;
    snapshot->captureUs = mailbox->frameCaptureUs;
    taskEXIT_CRITICAL(&mailbox->lock);

    snapshot->data = mailbox->buffers[activeIndex];
    return snapshot->data != nullptr && snapshot->len > 0;
}