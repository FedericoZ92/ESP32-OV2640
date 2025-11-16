
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


class TfLiteWrapper {
public:
    TfLiteWrapper() = default;
    TfLiteWrapper(const unsigned char* model_data, size_t arena_size = 10*1024);

    bool runInference(const uint8_t* image_data, int width, int height);
    uint8_t* getOutputDataUint8() const;
    TfLiteTensor* getInputTensor();

private:
    const tflite::Model* model;
    tflite::MicroInterpreter* interpreter;
    tflite::MicroMutableOpResolver<16> resolver; // max 16 ops
    uint8_t* tensor_arena;
    size_t arena_size;
};