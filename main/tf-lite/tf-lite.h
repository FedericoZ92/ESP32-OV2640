
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
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"

#define TF_IMAGE_INPUT_SIZE 96
#define DETECTION_THRESHOLD 0.6f

class TfLiteWrapper {
public:
    TfLiteWrapper() = default;
    ~TfLiteWrapper() = default;

    // Non-copyable, non-movable — prevents dangling interpreter references
    TfLiteWrapper(const TfLiteWrapper&) = delete;
    TfLiteWrapper& operator=(const TfLiteWrapper&) = delete;
    TfLiteWrapper(TfLiteWrapper&&) = delete;
    TfLiteWrapper& operator=(TfLiteWrapper&&) = delete;

    bool init(const unsigned char* model_data, size_t arena_size, uint8_t* arena_buffer = nullptr);

    bool runInference(const uint8_t* image_data, int width, int height);
    uint8_t* getOutputDataUint8() const;
    TfLiteTensor* getInputTensor();

private:
    const tflite::Model* model = nullptr;
    tflite::MicroInterpreter* interpreter = nullptr;
    tflite::MicroMutableOpResolver<16> resolver; // max 16 ops
    uint8_t* tensor_arena = nullptr;
    size_t arena_size = 0;

    // Storage for the interpreter — aligned, owned by this object
    alignas(alignof(tflite::MicroInterpreter)) uint8_t interpreter_storage[sizeof(tflite::MicroInterpreter)];
};