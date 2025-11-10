
#include "tf-lite.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "debug.h"

TfLiteWrapper::TfLiteWrapper(const unsigned char* model_data, size_t arena_size_)
: model(nullptr), interpreter(nullptr), tensor_arena(nullptr), arena_size(arena_size_)
{
    // Map model
    model = tflite::GetModel(model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TF_TAG, "Model schema version mismatch!");
        return;
    }

    // Add only the ops your model uses
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddMaxPool2D();
    resolver.AddAveragePool2D();
    resolver.AddFullyConnected();
    resolver.AddSoftmax();
    resolver.AddReshape();
    resolver.AddQuantize();
    resolver.AddDequantize();
    resolver.AddRelu();
    resolver.AddRelu6();

    // Allocate tensor arena
    // Need to allocate enough RAM for the tensor arena, which holds:
    //- Input/output tensors
    //- Intermediate tensors
    //- Scratch buffers
    //- Activations
    ESP_LOGI(TF_TAG, "Largest allocatable block: %d", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TF_TAG, "Free heap before arena allocation: %d, arena size: %d", (int)esp_get_free_heap_size(), (int)arena_size);
        tensor_arena = (uint8_t*)heap_caps_malloc(arena_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!tensor_arena) {
        ESP_LOGE(TF_TAG, "Failed to allocate tensor arena!");
        return;
    }

    // Create interpreter
    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tensor_arena, arena_size);
    interpreter = &static_interpreter;

    TfLiteStatus status = interpreter->AllocateTensors();
    if (status != kTfLiteOk) {
        ESP_LOGE(TF_TAG, "Tensor allocation failed, err: %d", (int)status);
        interpreter = nullptr;
    }
}

bool TfLiteWrapper::runInference(const uint8_t* image_data, int width, int height)
{
    if (!interpreter) {
        ESP_LOGE(TF_TAG, "Interpreter is null");
        return false;
    }

    TfLiteTensor* input = interpreter->input(0);
    if (!input) {
        ESP_LOGE(TF_TAG, "Input tensor is null");
        return false;
    }

    ESP_LOGI(TF_TAG, "Input tensor type: %d", input->type);

    // Log first few pixels
    ESP_LOGI(TF_TAG, "First 10 input pixels:");
    for (int i = 0; i < 10 && i < width * height * 3; i++) {
        ESP_LOGI(TF_TAG, "%d: %u", i, image_data[i]);
    }

    // Copy image data into input tensor
    if (input->type == kTfLiteUInt8) {
        memcpy(input->data.uint8, image_data, width * height * 3);
    } else if (input->type == kTfLiteFloat32) {
        float* input_f = input->data.f;
        for (int i = 0; i < width * height * 3; i++) {
            input_f[i] = image_data[i] / 255.0f;
        }
    } else {
        ESP_LOGE(TF_TAG, "Unsupported input tensor type: %d", input->type);
        return false;
    }

    // Run inference
    TfLiteStatus status = interpreter->Invoke();
    if (status != kTfLiteOk) {
        ESP_LOGE(TF_TAG, "Model invocation failed with status %d", status);
        return false;
    }

    TfLiteTensor* output = interpreter->output(0);
    if (!output) {
        ESP_LOGE(TF_TAG, "Output tensor is null");
        return false;
    }

    int num_outputs = 1;
    if (output->dims->size > 1) {
        num_outputs = output->dims->data[output->dims->size - 1];
    }

    ESP_LOGI(TF_TAG, "Output tensor type: %d, size: %d", output->type, num_outputs);

    // Print first few output values
    ESP_LOGI(TF_TAG, "Output values:");
    if (output->type == kTfLiteFloat32) {
        for (int i = 0; i < num_outputs; i++) {
            ESP_LOGI(TF_TAG, "output[%d] = %f", i, output->data.f[i]);
        }
    } else if (output->type == kTfLiteUInt8) {
        for (int i = 0; i < num_outputs; i++) {
            ESP_LOGI(TF_TAG, "output[%d] = %u", i, output->data.uint8[i]);
        }
    } else {
        ESP_LOGW(TF_TAG, "Unknown output tensor type: %d", output->type);
    }

    // Assuming output is [1, num_classes] float and person is class 1
    int person_index = 1; // adjust if needed
    float confidence = 0.0f;
    if (output->type == kTfLiteFloat32) {
        confidence = output->data.f[person_index];
    } else if (output->type == kTfLiteUInt8) {
        confidence = output->data.uint8[person_index] / 255.0f;
    }

    ESP_LOGI(TF_TAG, "Person confidence: %f", confidence);
    bool detected = confidence > 0.5f;
    ESP_LOGI(TF_TAG, "Person detected? %s", detected ? "YES" : "NO");

    return detected;
}

uint8_t* TfLiteWrapper::getOutputDataUint8() const {
    //TfLiteTensor* output = interpreter->output(0);
    //return output->data.uint8;   // for uint8 quantized models
    return 0;
}

TfLiteTensor* TfLiteWrapper::getInputTensor() {
    return interpreter ? interpreter->input(0) : nullptr;
}


