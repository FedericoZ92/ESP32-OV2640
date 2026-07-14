
* **Out-of-the-box (TCP Upload):** You can expect **20 to 60 Mbps** in a typical home Wi-Fi environment.
* **Maximum Optimized (TCP Upload):** Up to **83 to 93 Mbps** (requires configuring high-performance buffers in `sdkconfig` and utilizing the S3's OPI PSRAM).
* **Maximum Optimized (UDP Upload):** Up to **98 to 106 Mbps**.
* **Theoretical Hardware Limit (PHY Rate):** **150 Mbps** (using 802.11n, 40 MHz bandwidth, 1T1R mode).

---

### 2. Official Performance Benchmarks (ESP-IDF)

Espressif’s official testing reveals how much throughput you can squeeze out of the chip depending on your `sdkconfig` optimization:

| Protocol / Mode | Default Configuration | Highly Optimized (Iperf Config) |
| --- | --- | --- |
| **TCP Upload (TX)** | ~58 - 64 Mbps | **83 - 93 Mbps** |
| **UDP Upload (TX)** | ~96 - 106 Mbps | **98 - 106 Mbps** |
| **TCP Download (RX)** | ~46 - 60 Mbps | **73 - 88 Mbps** |
| **UDP Download (RX)** | ~86 - 92 Mbps | **88 - 99 Mbps** |

*Note: The absolute maximum values (93+ Mbps TCP) require using a module with 8-line (OPI) PSRAM and maximizing LwIP TCP window sizes.*

---

### 3. What this means for your Camera Stream

For a live video streaming server, the ESP32-S3's Wi-Fi speed is **overkill**.

* A standard **VGA (640x480) MJPEG stream** at 20–30 frames per second only requires about **2 to 5 Mbps**.
* An **SVGA (800x600)** or **HD (1280x720) stream** requires about **6 to 12 Mbps**.

If your stream is lagging, the bottleneck is almost never the Wi-Fi upload speed itself. Instead, it is usually caused by:

1. **CPU Overhead:** The time it takes for the chip to compress raw sensor data into JPEG frames.
2. **PSRAM Bottlenecks:** Slow PSRAM allocation or using 4-line (QSPI) instead of 8-line (OPI) PSRAM configuration.
3. **2.4 GHz Interference:** The ESP32-S3 only supports 2.4 GHz Wi-Fi, which is highly prone to congestion from nearby routers, Bluetooth, and microwaves.