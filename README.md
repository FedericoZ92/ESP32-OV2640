# ESP32 Person Detection Camera

This project implements a **real-time person detection camera** on an ESP32 board with PSRAM support. It captures images from an OV2640 camera, runs **TensorFlow Lite Micro** inference to detect the presence of a person, and serves the latest captured frame over an HTTP server. LEDs indicate detection status, and the system is designed for low-memory embedded environments.

---

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

---

## Hardware Requirements

- ESP32 board with PSRAM support (e.g., ESP32-WROVER)
- OV2640 camera module
- RGB LEDs or separate blue/red LEDs for detection indicators

---

## Software Components

- **FreeRTOS**: Task scheduling and concurrency
- **ESP-IDF**: ESP32 SDK for hardware access
- **ESP32 Camera Driver**: Capture frames from OV2640
- **TensorFlow Lite Micro**: Lightweight ML inference on microcontrollers
- **HTTP Server**: Serve captured frames to web clients

**Custom Libraries**:

- `camera-driver` – camera initialization and capture
- `wifi` – Wi-Fi management
- `led` – LED control
- `psram` – external memory management
- `image-editing` – frame resizing and cropping

---

## How It Works

### Initialization

1. PSRAM and internal memory checked and allocated.
2. TensorFlow Lite Micro arena initialized.
3. Wi-Fi and HTTP server started.
4. Camera initialized and flipped 180° if needed.

### Capture Task

- Runs periodically (every 10 seconds by default).
- Captures a frame from the camera.
- Resizes to 96×96 grayscale for TFLite inference.
- Runs person detection model.
- Updates LED indicators based on detection result.

### HTTP Server

- Latest frame is served at `/capture.jpg`.