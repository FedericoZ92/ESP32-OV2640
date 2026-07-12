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

// JavaScript-compatible boolean string derived from ENABLE_POLLING_FALLBACK.
#if ENABLE_POLLING_FALLBACK
    #define ENABLE_POLLING_FALLBACK_JS "true"
#else
    #define ENABLE_POLLING_FALLBACK_JS "false"
#endif
