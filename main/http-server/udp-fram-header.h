#pragma once

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t frameSeq;
    uint16_t chunkIndex;
    uint16_t chunkCount;
    uint16_t payloadLen;
    uint16_t frameLen;
    uint32_t frameAgeMs;
} UdpFrameHeader;



