
#pragma once
#include <vector>
#include <cstdint>
#include "debug.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "esp_log.h"
//#include "tflite-person-detect/person_detect_model_data.h"
#include <cstdint>


class TfLiteWrapper {
public:
    TfLiteWrapper(const uint8_t* model_data, size_t arena_size = 10 * 1024);
    ~TfLiteWrapper();

    // Process a frame (to be implemented later)
    bool runInference(const uint8_t* frame_data, int width, int height) { return false; }

private:
    const tflite::Model* model = nullptr;
    tflite::MicroInterpreter* interpreter = nullptr;
    uint8_t* tensor_arena = nullptr;
    size_t arena_size_bytes = 0;

    // Resolver placeholder — may switch to AllOpsResolver if available
    tflite::MicroMutableOpResolver<10> resolver;
};