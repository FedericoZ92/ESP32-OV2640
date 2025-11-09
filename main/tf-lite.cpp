
#include "tf-lite.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "debug.h"


TfLiteWrapper::TfLiteWrapper(const uint8_t* model_data, size_t arena_size)
    : arena_size_bytes(arena_size)
{
    ESP_LOGI(TF_TAG, "Initializing TfLiteWrapper...");

    // Load the model
    model = tflite::GetModel(model_data);
    if (!model) {
        ESP_LOGE(TF_TAG, "Failed to load TFLite model");
        return;
    }
    ESP_LOGI(TF_TAG, "Model loaded, version: %d", (int)model->version());

    // Allocate tensor arena
    tensor_arena = (uint8_t*)malloc(arena_size_bytes);
    if (!tensor_arena) {
        ESP_LOGE(TF_TAG, "Failed to allocate tensor arena (%d bytes)", (int)arena_size_bytes);
        return;
    }

    // Create the interpreter
    static tflite::MicroInterpreter static_interpreter(model, resolver, tensor_arena, arena_size_bytes, nullptr);
    interpreter = &static_interpreter;

    // Allocate tensors
    TfLiteStatus status = interpreter->AllocateTensors();
    if (status != kTfLiteOk) {
        ESP_LOGE(TF_TAG, "Failed to allocate tensors");
        interpreter = nullptr;
    } else {
        ESP_LOGI(TF_TAG, "Tensors allocated successfully");
    }
}

TfLiteWrapper::~TfLiteWrapper() {
    if (tensor_arena) {
        free(tensor_arena);
        tensor_arena = nullptr;
    }
}