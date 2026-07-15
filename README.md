# ESP32 Person Detection Camera

![Project Image](docs/20251226_151428.jpg)

This project implements a **real-time person detection camera** on an ESP32 board with PSRAM support. It captures images from an OV2640 camera, runs **TensorFlow Lite Micro** inference to detect the presence of a person, and serves the latest captured frame over an HTTP server. LEDs indicate detection status, and the system is designed for low-memory embedded environments.

## Table of Contents

- [Features](#features)
- [Hardware Requirements](#hardware-requirements)
- [Software Requirements](#software-requirements)
- [Build and Flash](#build-and-flash)
- [Usage](#usage)
- [Configuration](#configuration)
- [Project Structure](#project-structure)
- [Dependencies](#dependencies)
- [Troubleshooting](#troubleshooting)
- [License](#license)

## Features

- **Camera capture**: Periodically captures frames from an OV2640 camera module.
- **Person detection**: Uses a TensorFlow Lite Micro model to detect whether a person is present in the frame.
- **LED indicators**:
  - **Blue LED** – person detected
  - **Red LED** – no person detected
- **HTTP streaming**: Serve the latest captured frame at `/capture.jpg`.
- **PSRAM-aware memory allocation**: Uses external PSRAM for frame buffers and TensorFlow Lite arena when available.
- **Wi-Fi connectivity**: Connects to a specified Wi-Fi network and exposes a local HTTP server.
- **Automatic reboot**: Optional runtime limit to restart the device for reliability.

## Hardware Requirements

- ESP32 board with PSRAM support (e.g., ESP32-WROVER)
- OV2640 camera module
- RGB LEDs or separate blue/red LEDs for detection indicators
- USB cable for power supply and programming

## Software Requirements

- ESP-IDF v5.0 or later
- CMake
- Python 3.7 or later

## Build and Flash

1. Set the target chip:
   ```bash
   idf.py set-target esp32
   ```

2. Configure the project (optional, for custom settings):
   ```bash
   idf.py menuconfig
   ```

3. Build the project:
   ```bash
   idf.py build
   ```

4. Flash and monitor:
   ```bash
   idf.py -p <PORT> flash monitor
   ```

Replace `<PORT>` with your ESP32's serial port (e.g., COM3 on Windows or /dev/ttyUSB0 on Linux).

## Usage

1. Power on the ESP32 board.
2. The device will connect to the configured Wi-Fi network.
3. Once connected, note the IP address from the serial output.
4. Access the HTTP server at `http://<IP_ADDRESS>/capture.jpg` to view the latest captured frame.
5. The LEDs will indicate detection status: blue for person detected, red for no person.

## Configuration

The project uses default settings. You can modify `sdkconfig` or use `idf.py menuconfig` to adjust:
- Wi-Fi SSID and password
- Camera settings (resolution, flip)
- Detection interval
- HTTP server port
- LED GPIO pins

## Project Structure

- `main/`: Source code directory
  - `main.cpp`: Main application entry point
  - `gpio-config.h`: GPIO pin configurations
  - `network.h`: Network-related definitions
- `camera-driver/`: Camera initialization and capture
- `wifi/`: Wi-Fi management
- `led/`: LED control
- `psram/`: External memory management
- `image-editing/`: Frame resizing and cropping
- `http-server/`: HTTP server implementation
- `tf-lite/`: TensorFlow Lite Micro integration
- `tflite-person-detect/`: Person detection model
- `script/`: Utility scripts for model conversion
- `docs/`: Documentation and images
- `build/`: Build output directory
- `CMakeLists.txt`: Project build configuration
- `sdkconfig`: Project configuration file
- `partitions.csv`: Partition table

## Dependencies

- ESP-IDF components: FreeRTOS, Wi-Fi, HTTP server, camera driver
- Managed components: esp32-camera, esp-tflite-micro, esp-nn, esp-jpeg
- TensorFlow Lite Micro model: Person detection model (included)

## Troubleshooting

- Ensure the ESP32 target is set correctly: `idf.py set-target esp32`
- Check serial port permissions on Linux/macOS
- Verify camera module is properly connected
- For Wi-Fi issues, check SSID and password in configuration
- For build issues, clean and rebuild: `idf.py clean && idf.py build`
- Monitor serial output for debug information

For technical queries, please refer to the [ESP-IDF documentation](https://docs.espressif.com/projects/esp-idf/) or open an issue on the project repository.


### Pipeline, software objects and parameters

# 1. Camera Capture and Preprocessing Loop

* **Acquisition:** The `capture_task` (running on Core 1) continuously pulls a raw frame buffer (`camera_fb_t`) from the hardware driver using `esp_camera_fb_get()`.

* **Decoding and Formatting:** If the camera is set to output JPEG, the chip performs a software decompression via `allocatingDecodeCameraJpeg` into a raw RGB565 format in external memory. It then converts the pixels to grayscale (`convertRgb565ToGrayscale`) and crops the center down to a 96x96 window (`tfliteGray96x96InputBuffer`).

* **Double-Buffering Architecture:** To prevent the web server from reading a frame while the camera is actively writing to it, the system utilizes two static frame buffers (`httpFrameBuffers[2]`) allocated in PSRAM, restricted to a maximum size of `kPublishedFrameMaxBytes`.

* **Atomic Metadata Swap:** Once a new frame is copied into the inactive buffer, the application enters a critical section lock using `httpFrameMetaLock` to instantly update active variables like length, width, height, format, sequence ID, and the publication timestamp.

# 2. Network Transport & HTTP Server

* **Server Configurations:** The network layer initiates the HTTP server on Core 0 via `CameraHttpServer::start`. It configures the engine using `HTTPD_DEFAULT_CONFIG()` while manually forcing `keep_alive_enable = true` and setting `max_open_sockets = 7` to handle concurrent connections.

* **Data Transmission Pipelines:** Depending on your preprocessor macros, the system streams the active data buffer using one of three methods:
* **HTTP Polling (`/capture.rgb`):** The client browser requests frames repeatedly. The server responds with an octet-stream or JPEG payload and injects custom tracking telemetry (`X-Frame-Seq`, `X-Frame-Age-Ms`, `X-Frame-Len`) straight into the HTTP headers.

* **HTTP Multipart Stream (`/stream.rgb`):** The server holds a single TCP connection open indefinitely, continuously pushing back-to-back frames separated by a multipart chunk boundary string.

* **UDP Stream (`udp_stream_task`):** The application bypasses HTTP entirely, splitting the active memory buffer into packet blocks up to `UDP_STREAM_MAX_PAYLOAD` bytes. It prefixes each block with a custom `UdpFrameHeader` packet (tracking magic bytes, chunk index, and frame age) and pushes them over raw sockets via `sendto` on `UDP_STREAM_PORT`.

* **Wi-Fi Radio Optimization:** To ensure the radio does not drop packets or introduce latency spikes during network transactions, the `WifiManager` explicitly disables Wi-Fi power saving by setting `esp_wifi_set_ps(WIFI_PS_NONE)`.

# 3. Core Network Parameters (TCP Window & LWIP Mailbox)

* **TCP Window Size:** There is no configuration or reference to TCP window sizes anywhere within your application source code.

* **LWIP Mailbox Queue Sizes:** Mailbox or queue depths for the network stack are completely absent from these files.

* **Where They Exist:** Both the TCP Window size and the LWIP mailbox parameters are managed implicitly by the default settings of the underlying ESP-IDF SDK framework. To change them, you must adjust them through the ESP-IDF interactive project configuration utility (`menuconfig`), as they cannot be manipulated from this project's code files.