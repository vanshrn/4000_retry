# ESP32-S3 RAM Architecture & Exact Memory Breakdown

This document provides the exact, component-by-component memory usage and allocation breakdown for both **Internal SRAM (DRAM)** and **External PSRAM (SPIRAM)** on the **Seeed Studio XIAO ESP32-S3** running at **4000 SPS** with **Queue Depth = 50**.

---

## 1. Hardware Memory Specifications

| Memory Type | Physical Chip | Total Hardware Capacity | Byte Count | Address Bus | Access Speed |
|---|---|---|---|---|---|
| **Internal SRAM / DRAM** | On-Die Silicon (ESP32-S3) | **512 KB** (320 KB usable heap/BSS) | 327,680 bytes | Internal 32-bit Bus | Zero-wait-state (240 MHz) |
| **External PSRAM (SPIRAM)** | External APMemory OPI Chip | **8 MB** | 8,388,608 bytes | Octal-SPI (OPI DTR) | High-speed cache-backed (80 MHz) |

---

## 2. External PSRAM (8 MB) — Exact Itemized Breakdown (Queue Depth = 50)

Total Physical PSRAM Capacity: 8,388,608 bytes (8.00 MB)

| Component / Buffer | Formula | Bytes | Size (MB/KB) | Location / Purpose |
|---|---|---|---|---|
| 1. Upload Queue Pool (50 blocks) | 50 * 32,260 | 1,613,000 | 1.54 MB | Holds 50s of offline raw & filtered ECG data |
| 2. JSON Serialization Buffer | 256 * 1024 | 262,144 | 256.00 KB | Formats 2D JSON array for WS/POST |
| 3. 5-Second Analysis Ring Buffer | 20,000 * 4 | 80,000 | 78.13 KB | Ring buffer for live arrhythmia analysis |
| 4. ECG Pipeline Instance & History | sizeof(ECGPipeline) | 32,100 | 31.35 KB | Holds workBuf and baseline history |
| 5. ADC Raw Ping-Pong Buffers | 2 * 4000 * 4 | 32,000 | 31.25 KB | Core 1 high-speed SPI sampling buffers |
| 6. DSP Comb Notch Temp Array | 4000 * 4 | 16,000 | 15.63 KB | PSRAM scratchpad for 50/100 Hz filter |
| 7. DSP Gaussian LP Temp Array | 4000 * 4 | 16,000 | 15.63 KB | PSRAM scratchpad for low-pass filter |
| 8. DSP Savitzky-Golay Temp Array | 4000 * 4 | 16,000 | 15.63 KB | PSRAM scratchpad for polynomial smoother |
| 9. ADC Motion Validity Mask Buffer | 2 * 4000 * 1 | 8,000 | 7.81 KB | Ping-pong per-sample motion flags |
| 10. PSRAM Heap Metadata & Overhead | Heap Tables | 28,604 | 27.93 KB | Memory manager tracking structures |
| **TOTAL ALLOCATED / USED PSRAM** | — | **2,103,848** | **2.01 MB (24.8%)** | — |
| **FREE PSRAM REMAINING** | — | **6,284,760** | **5.99 MB (~6.30 MB / 75.2%)** | Headroom for dynamic buffers |
| **EXACT SUM TOTAL** | — | **8,388,608** | **8.00 MB (100.0%)** | Full hardware capacity |

---

## 3. Internal SRAM / DRAM (320 KB Usable) — Exact Itemized Breakdown

Total Usable Internal DRAM Capacity: 327,680 bytes (320.00 KB)

| Component / Memory Area | Type | Bytes | Size (KB) | Purpose |
|---|---|---|---|---|
| 1. Wi-Fi & Bluetooth MAC Baseband RAM | System/HW | 65,536 | 64.00 KB | DMA packet rings & radio descriptors |
| 2. mbedTLS SSL Handshake & RX/TX Context | TLS Socket | 36,864 | 36.00 KB | AES-GCM crypto & TLS record session |
| 3. Static Data (.data) & System BSS | Static Link | 31,432 | 30.70 KB | Global variables & sensor driver state |
| 4. uploadTask FreeRTOS Stack (Core 0) | Task Stack | 16,384 | 16.00 KB | Background cloud streaming task stack |
| 5. dspTask FreeRTOS Stack (Core 1) | Task Stack | 16,384 | 16.00 KB | Core 1 DSP & metrics calculation stack |
| 6. loopTask Arduino Stack (Core 1) | Task Stack | 16,384 | 16.00 KB | 4000 SPS SPI DRDY acquisition stack |
| 7. TCP/IP (LwIP) Socket Buffers | Network | 12,288 | 12.00 KB | Low-level TCP transmit & receive rings |
| 8. wsTaskWorker FreeRTOS Stack (Core 0) | Task Stack | 8,192 | 8.00 KB | WebSocket polling worker stack |
| 9. localStatusTask Stack (Core 0) | Task Stack | 4,096 | 4.00 KB | Port 9000 local heartbeat task stack |
| 10. FreeRTOS Queue Controls (50 pointers) | OS Objects | 400 | 0.39 KB | 50 pointers * 4 bytes for both queues |
| 11. Heap Management Tables & Alignment | Overhead | 39,372 | 38.45 KB | FreeRTOS heap alloc overhead |
| **TOTAL ALLOCATED / USED INTERNAL DRAM** | — | **247,332** | **241.53 KB (75.5%)** | — |
| **FREE INTERNAL HEAP** | — | **80,348** | **78.47 KB (24.5%)** | Contiguous memory for TLS reconnects |
| **EXACT SUM TOTAL** | — | **327,680** | **320.00 KB (100.0%)** | Full usable DRAM capacity |

---

## 4. Summary: Dual-RAM Architecture

- **Internal SRAM (512 KB)**: Dedicated to zero-latency tasks (FreeRTOS stacks, Radio baseband, TLS crypto engines).
- **External PSRAM (8 MB)**: Dedicated to large data arrays (50-second offline upload queue, 256 KB JSON buffers, 4000-sample signal processing arrays).