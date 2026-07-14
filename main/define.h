#pragma once

// Size of temporary JPEG-related working buffer (bytes).
#define JPEG_BUFFER_SIZE (20 * 1024)

// TensorFlow Lite arena size reserved for model execution (bytes).
#define ARENA_SIZE (128 * 1024)

// Keep /stream.rgb connection open indefinitely when set to 1.
// Set to 0 to stream a limited burst and reconnect.
#define STREAM_KEEP_OPEN 1

// Enable TensorFlow inference path in capture_task when set to 1.
// Set to 0 to suspend inference for streaming performance tests.
#define ENABLE_INFERENCE 0

// Enable browser fallback from /stream.rgb to /capture.rgb polling when set to 1.
#define ENABLE_POLLING_FALLBACK 0

// Stop camera acquisition after the first successfully published frame when set to 1.
#define STOP_ACQUISITION_AFTER_1_FRAME 0

// Transport selection: set exactly one of these to 1.
// USE_TCP uses the existing HTTP server transport.
// USE_UDP uses datagram streaming over UDP.
#define USE_TCP 1
#define USE_UDP 0

// UDP stream settings (used only when USE_UDP=1).
#define UDP_STREAM_PORT 5001
#define UDP_STREAM_MAX_PAYLOAD 1024

#if ((USE_TCP + USE_UDP) != 1)
    #error "Invalid transport config: set exactly one of USE_TCP or USE_UDP to 1"
#endif

// JavaScript-compatible boolean string derived from ENABLE_POLLING_FALLBACK.
#if ENABLE_POLLING_FALLBACK
    #define ENABLE_POLLING_FALLBACK_JS "true"
#else
    #define ENABLE_POLLING_FALLBACK_JS "false"
#endif
