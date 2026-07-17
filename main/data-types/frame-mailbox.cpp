
#include "data-types/frame-mailbox.h"
#include "esp_log.h"
#include "define.h"
#include "debug.h"
#include <esp_heap_caps.h>
#include <cstring>

bool FrameMailboxManager::initFrameMailbox(const char* tag, size_t kPublishedFrameMaxBytes, uint32_t caps) //caps MALLOC_CAP_SPIRAM
{
    if (!mailbox) {
        return false;
    }
    mailbox->tag = tag;
    mailbox->buffers[0] = (uint8_t*)heap_caps_malloc(kPublishedFrameMaxBytes, caps);
    mailbox->buffers[1] = (uint8_t*)heap_caps_malloc(kPublishedFrameMaxBytes, caps);
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

void FrameMailboxManager::publish( size_t kPublishedFrameMaxBytes,
                                    const uint8_t* src,
                                    size_t srcLen,
                                    uint16_t width,
                                    uint16_t height,
                                    pixformat_t format,
                                    int64_t captureUs)
{
    if (!src || !mailbox || srcLen == 0) {
        return;
    }

    size_t frameLen = srcLen;
    if (frameLen > kPublishedFrameMaxBytes) {
        ESP_LOGW(MAIN_TAG,
                 "Mailbox %s frame too large (%u > %u), truncating",
                 mailbox->tag ? mailbox->tag : "unknown",
                 (unsigned int)frameLen,
                 (unsigned int)kPublishedFrameMaxBytes);
        frameLen = kPublishedFrameMaxBytes;
    }

    uint8_t publishIndex = mailbox->activeIndex ^ 1;
    if (!mailbox->buffers[publishIndex]) {
        return;
    }

    memcpy(mailbox->buffers[publishIndex], src, frameLen);
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