
#include "tf-lite/tf-lite.h"
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
    ESP_LOGD(TF_TAG, "Largest allocatable block: %d", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGD(TF_TAG, "Free heap before arena allocation: %d, arena size: %d", (int)esp_get_free_heap_size(), (int)arena_size);
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

    // Validate input tensor shape: [1, height, width, 1]
    if (input->dims->size != 4 ||
        input->dims->data[0] != 1 ||
        input->dims->data[1] != height ||
        input->dims->data[2] != width ||
        input->dims->data[3] != 1) {
        ESP_LOGE(TF_TAG, "Unexpected input tensor shape: [%d, %d, %d, %d]",
                 input->dims->data[0],
                 input->dims->data[1],
                 input->dims->data[2],
                 input->dims->data[3]);
        return false;
    }

    ESP_LOGD(TF_TAG, "Input tensor type: %d", input->type);

    // Log first few input pixels
    std::string somePixels = ""; 
    for (int i = 0; i < 10 && i < width * height; i++) {
        somePixels += std::to_string(image_data[i]);
        if (i < 9 && i < width * height - 1) {
            somePixels += ", ";
        }
    }
    ESP_LOGD(TF_TAG, "First input pixels: %s", somePixels.c_str());

    // Copy image data into input tensor
    int expected_size = width * height;
    if (input->type == kTfLiteUInt8) {
        memcpy(input->data.uint8, image_data, expected_size);
    } else if (input->type == kTfLiteFloat32) {
        float* input_f = input->data.f;
        for (int i = 0; i < expected_size; i++) {
            input_f[i] = image_data[i] / 255.0f;
        }
    } else if (input->type == kTfLiteInt8) {
        // kTfLiteInt8 expects values in the range [-128, 127], so we subtract 128 from each pixel.
        // This assumes symmetric quantization. If your model uses asymmetric quantization, you should apply:
        // input_i8[i] = static_cast<int8_t>((image_data[i] - zero_point) / scale);
        int8_t* input_i8 = input->data.int8;
        for (int i = 0; i < expected_size; i++) {
            // Convert [0,255] to [-128,127] using symmetric quantization
            input_i8[i] = static_cast<int8_t>((image_data[i] - 128));
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

    // Get output tensor
    TfLiteTensor* output = interpreter->output(0);
    if (!output) {
        ESP_LOGE(TF_TAG, "Output tensor is null");
        return false;
    }

    int num_outputs = 1;
    if (output->dims->size > 1) {
        num_outputs = output->dims->data[output->dims->size - 1];
    }

    ESP_LOGD(TF_TAG, "Output tensor type: %d, size: %d", output->type, num_outputs);

    // Print first few output values
    ESP_LOGD(TF_TAG, "Output values:");
    if (output->type == kTfLiteFloat32) {
        for (int i = 0; i < num_outputs; i++) {
            ESP_LOGD(TF_TAG, "output[%d] = %f", i, output->data.f[i]);
        }
    } else if (output->type == kTfLiteUInt8) {
        for (int i = 0; i < num_outputs; i++) {
            ESP_LOGD(TF_TAG, "output[%d] = %u", i, output->data.uint8[i]);
        }
    } else if (output->type == kTfLiteInt8) {
        for (int i = 0; i < num_outputs; i++) {
            int8_t raw = output->data.int8[i];
            float dequantized = (raw - output->params.zero_point) * output->params.scale;
            ESP_LOGD(TF_TAG, "output[%d] = %d (dequantized: %f)", i, raw, dequantized);
        }
    } else {
        ESP_LOGW(TF_TAG, "Unknown output tensor type: %d", output->type);
    }

    // Assuming person is class index 1
    int person_index = 1;
    if (person_index >= num_outputs) {
        ESP_LOGW(TF_TAG, "Person index out of bounds");
        return false;
    }

    float confidence = 0.0f;
    if (output->type == kTfLiteFloat32) {
        confidence = output->data.f[person_index];
    } else if (output->type == kTfLiteUInt8) {
        confidence = output->data.uint8[person_index] / 255.0f;
    } else if (output->type == kTfLiteInt8) {
        int8_t raw = output->data.int8[person_index];
        confidence = (raw - output->params.zero_point) * output->params.scale;
    } else {
        ESP_LOGW(TF_TAG, "Unsupported output tensor type: %d", output->type);
    }

    ESP_LOGD(TF_TAG, "Person confidence: %f", confidence);
    bool detected = confidence > 0.5f;
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


