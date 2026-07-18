
#include "camera-driver/camera-driver.h"
#include "http-server/http-server.h"
#include "wifi/wifi.h"
#include "debug.h"
#include "gpio-config.h"
#include "tf-lite/tf-lite.h"
#include "tflite-person-detect/person_detect_model_data.h"
#include "image-editing/editing.h"
#include "led/rgb-led.h"
#include "led/red-led.h"
#include "app-globals.h"
#include "psram/psram.h"
#include "network.h"
#include "checkpoint-timer/checkpoint-timer.h"
#include "util/misc.h"
#include "define.h"
#include "data-types/frame-mailbox.h"
#include "http-server/http-frame-buffer.h"
#include <stdio.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_timer.h>
#include <esp_camera.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>

// OV2640 pin map for ESP32-S3-CAM-N16R8
// https://www.oceanlabz.in/getting-started-with-esp32-s3-wroom-n16r8-cam-dev-board/?srsltid=AfmBOors-1xeo_-CM5mcneEFHgQY9ps0qX2SHt8gf-S7Ndizot0T4vzk
// in docs: schematics file from: https://www.homotix.it/vendita/moduli-wi-fi/scheda-esp32-s3-n16r8
// https://github.com/microrobotics/ESP32-S3-N16R8/blob/main/ESP32-S3-N16R8_User_Guide.pdf
// https://www.fruugo.it/esp32-s3-wroom-n16r8-modulo-fotocamera-per-scheda-di-sviluppo-cam-con-ov2640/p-358759907-780375612?language=it&ac=google&utm_source=google&utm_medium=paid&gad_source=1&gad_campaignid=22510258486&gbraid=0AAAAADpXug2uMu5_YIv6BL_H9NJZ76oa1&gclid=EAIaIQobChMI8brdxtT0kQMVuZqDBx1AHgjiEAQYASABEgI-qPD_BwE

// camera parameters in camera-driver.cpp!

static CameraHttpServer server;
static WifiManager wifi;
RgbLedController rgb(GPIO_NUM_48, RMT_CHANNEL_0); // 8 LEDs on GPIO48
RedLedController redLed = RedLedController();
static TfLiteWrapper tf_wrapper;

CheckpointTimer jpegTimer;
CheckpointTimer cameraAcquisitionTimer;
CheckpointTimer tensorFlowTimer;
CheckpointTimer httpTimer;

// --- Shared static frame buffer ---
/*static uint8_t* httpFrameBuffers[2] = {nullptr, nullptr};
static volatile uint8_t activeHttpFrameIndex = 0;
static volatile size_t activeHttpFrameLen = 0;
static volatile uint16_t activeHttpFrameWidth = TF_IMAGE_INPUT_SIZE;
static volatile uint16_t activeHttpFrameHeight = TF_IMAGE_INPUT_SIZE;
static volatile pixformat_t activeHttpFrameFormat = PIXFORMAT_GRAYSCALE;
static volatile uint32_t activeHttpFrameSeq = 0;
static volatile int64_t activeHttpFramePublishUs = 0;*/
HttpFrameBuffer httpFrameBuffer(kPublishedFrameMaxBytes);

volatile bool pauseCameraAcquisition = false;

portMUX_TYPE httpFrameMetaLock = portMUX_INITIALIZER_UNLOCKED;


FrameMailbox inferenceMailbox;
FrameMailbox streamMailbox;
FrameMailboxManager inferenceMailboxManager(&inferenceMailbox, kPublishedFrameMaxBytes);
FrameMailboxManager streamMailboxManager(&streamMailbox, kPublishedFrameMaxBytes);


extern "C" void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);

    // --- Print chip info ---
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    ESP_LOGI(OV2640_TAG, "This is %s chip with %d CPU core(s)", CONFIG_IDF_TARGET, (int)chip_info.cores);

    if (!PSRAM::isAvailable()) {
        ESP_LOGD(MAIN_TAG, "PSRAM NOT available.\n");
        return;
    }

    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(OV2640_TAG, "Get flash size failed");
        return;
    }
    ESP_LOGI(OV2640_TAG, "Flash size: %" PRIu32 " MB, PSRAM size: %" PRIu32 " MB", flash_size / (1024 * 1024), (uint32_t)(PSRAM::getSize() / (1024 * 1024)));

    // --- Camera reset sequence ---
    CameraDriver::operateCameraResetSequence();
    vTaskDelay(pdMS_TO_TICKS(100));

    log_RAM_status("pre Wi-Fi initialization"); // TODO restore

    // --- Wi-Fi initialization ---
    esp_err_t wifiInitErr = wifi.init();
    if (wifiInitErr != ESP_OK) {
        ESP_LOGE(OV2640_TAG, "Wi-Fi base init failed: %s", esp_err_to_name(wifiInitErr));
        return;
    }
    //STA: station. Wi-Fi STA is a device that connects to a Wi-Fi access point. 
    esp_err_t wifiStaErr = wifi.initSTA(WIFI_NETWORK, WIFI_PASSWORD); // connect to your network
    if (wifiStaErr != ESP_OK) {
        ESP_LOGE(OV2640_TAG, "Wi-Fi STA connection failed, capture/HTTP startup aborted");
        return;
    }

    // --- Camera initialization ---
    CameraDriver camera;
    if (camera.init() != ESP_OK) {
        ESP_LOGE(OV2640_TAG, "Camera initialization failed");
        return;
    }

    if (!httpFrameBuffer.initPublishedHttpFrameStore(kPublishedFrameMaxBytes, MALLOC_CAP_SPIRAM)) {
        return;
    }

    static AppTaskContext appTaskContext = {};
    appTaskContext.server = &server;
    appTaskContext.httpFrameBuffer = &httpFrameBuffer;
    appTaskContext.streamMailboxManager = &streamMailboxManager;
    appTaskContext.inferenceMailboxManager = &inferenceMailboxManager;
    appTaskContext.httpFrameMetaLock = &httpFrameMetaLock;

    #if ENABLE_RGB_STREAM_TASK
        if (!streamMailboxManager.initFrameMailbox("stream", MALLOC_CAP_SPIRAM)) {
            return;
        }
    #endif
    #if ENABLE_INFERENCE
        if (!inferenceMailboxManager.initFrameMailbox("inference", MALLOC_CAP_SPIRAM)) {
            return;
        }
    #endif

    // HTTP server task (always on; transport-specific handlers are configured below).
    auto http_server_task = [](void* arg) {
        #if USE_TCP
            server.setCaptureHandler(CameraHttpServer::captureRgbTcpCallback);
            server.setStreamHandler(CameraHttpServer::streamRgbTcpCallback);
        #endif

        if (server.start() == ESP_OK) {
            #if USE_TCP
                ESP_LOGI(OV2640_TAG, "TCP mode active. HTTP server started at http://%s/", wifi.getLocalIP().c_str());
            #else
                ESP_LOGI(OV2640_TAG, "UDP mode active. HTTP status page available at http://%s/", wifi.getLocalIP().c_str());
            #endif
        } else {
            ESP_LOGE(OV2640_TAG, "HTTP server not started");
        }
        vTaskDelete(NULL); // Clean up task after starting server
    };
    xTaskCreatePinnedToCore(
        http_server_task,
        "http_server_task",
        4096,
        NULL,
        5,
        NULL,
        CORE_ID_HTTP_SERVER
    );

    #if USE_UDP
        ESP_LOGI(OV2640_TAG, "UDP mode active. Stream port: %d", UDP_STREAM_PORT);
        xTaskCreatePinnedToCore(
            [](void* arg) {
                AppTaskContext* ctx = static_cast<AppTaskContext*>(arg);
                if (ctx && ctx->server) {
                    ctx->server->udp_stream_task(arg);
                }
            },
            "udp_stream_task",
            6144,
            &appTaskContext,
            5,
            NULL,
            0
        );
    #endif

    #if ENABLE_RGB_STREAM_TASK
        xTaskCreatePinnedToCore(
            [](void* arg) {
                AppTaskContext* ctx = static_cast<AppTaskContext*>(arg);
                if (ctx && ctx->server) {
                    ctx->server->http_stream_publish_task(arg);
                }
            },
            "http_stream_publish_task",
            6144,
            &appTaskContext,
            5,
            &streamMailbox.consumerTaskHandle,
            tskNO_AFFINITY
        );
        ESP_LOGI(OV2640_TAG, "Started stream publish task");
    #else
        ESP_LOGW(OV2640_TAG, "RGB stream publish task disabled");
    #endif

    #if ENABLE_INFERENCE
        // --- Allocate TensorFlow Lite arena intelligently ---
        uint8_t* tensor_arena = nullptr;
        const size_t arena_size = ARENA_SIZE;
        if (!tensor_arena) {
            ESP_LOGW(TF_TAG, "Internal RAM low, allocating arena in PSRAM");
            tensor_arena = (uint8_t*) heap_caps_malloc(arena_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (!tensor_arena) {
            ESP_LOGE(TF_TAG, "Failed to allocate tensor arena!");
            return;
        }

        // --- Initialize TensorFlow Lite wrapper using allocated arena ---
        if (!tf_wrapper.init(g_person_detect_model_data, arena_size, tensor_arena)) {
            ESP_LOGE(TF_TAG, "TfLite initialization failed!");
            return;
        }

        xTaskCreatePinnedToCore(
            [](void* arg) {
                TfLiteWrapper* wrapperPtr = static_cast<TfLiteWrapper*>(arg);
                if (wrapperPtr) {
                    wrapperPtr->inference_task(arg);
                }
            },
            "inference_task",
            6144,
            &tf_wrapper,
            5,
            &inferenceMailbox.consumerTaskHandle,
            tskNO_AFFINITY
        );
        ESP_LOGI(OV2640_TAG, "Started inference task");
    #else
        ESP_LOGW(TF_TAG, "Inference disabled for test build (ENABLE_INFERENCE=0)");
    #endif

    // --- Start capture task ---
    #if ENABLE_CAMERA_ACQUISITION_TASK
        xTaskCreatePinnedToCore( 
                                [](void* arg) {
                                    CameraDriver* cameraPtr = static_cast<CameraDriver*>(arg);
                                    if (cameraPtr) {
                                        cameraPtr->capture_task(arg);
                                    }
                                },
                                "capture_task",
                                4096,
                                &camera,
                                5,
                                NULL,
                                1); //xCoreID 0 for main core, 1 for app core
        ESP_LOGI(OV2640_TAG, "Started camera acquisition task");
    #else
        ESP_LOGW(OV2640_TAG, "Camera acquisition task disabled");
    #endif

    // --- Optional: run for a limited time, then reboot ---
    /*char buffer[1024]; 
    for (int i = 0; i < 30; i++) { // log task stats every 5 seconds for 25 seconds
        vTaskGetRunTimeStats(buffer);
        printf("Task Name\tRun Time\tCPU %%\n");
        printf("%s\n", buffer);
        vTaskDelay(pdMS_TO_TICKS(1000 * 10)); //10 sec 
    }*/

    //ESP_LOGW(OV2640_TAG, "Rebooting system...");
    //esp_restart();
}