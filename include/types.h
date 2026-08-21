/**
 * types.h
 * -----------------------------------------------------------
 * SWET_IOT - Shared Types, Enums, Structs
 * Updated: Block.data uses int32_t for full raw ADC range.
 * -----------------------------------------------------------
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// ==========================================================
// Driver Status / Result Codes
// ==========================================================
enum class ADS1292_Status : uint8_t {
    OK = 0,
    SPI_ERROR,
    DEVICE_NOT_FOUND,
    REGISTER_MISMATCH,
    NOT_INITIALIZED,
    TIMEOUT
};

// ==========================================================
// Internal Driver State
// ==========================================================
enum class ADS1292_State : uint8_t {
    UNINIT = 0,
    RESET_DONE,
    CONFIGURED,
    RUNNING,
    STOPPED
};

// ==========================================================
// Raw ECG Sample
// ==========================================================
struct ECGSample {
    int32_t  channel1;          // CH1 (respiration) — 24-bit signed
    int32_t  channel2;          // CH2 (ECG)         — 24-bit signed, polarity corrected
    bool     leadOffDetected;
    bool     loPlusOff;         // RA (IN1P / LO+) disconnected
    bool     loMinusOff;        // LA (IN1N / LO-) disconnected
    uint32_t timestamp_ms;
    bool     valid;
};

// ==========================================================
// Register Snapshot
// ==========================================================
struct ADS1292_RegisterSnapshot {
    uint8_t id;
    uint8_t config1;
    uint8_t config2;
    uint8_t loff;
    uint8_t ch1set;
    uint8_t ch2set;
    uint8_t rld_sens;
    uint8_t loff_sens;
    uint8_t loff_stat;
    uint8_t resp1;
    uint8_t resp2;
    uint8_t gpio;
};

// ==========================================================
// Ping-Pong Block  — full raw 24-bit ADC signed values
// NOTE: int32_t (not uint16_t) — NO scaling to [0,4095].
//       Full ADS1292R range: ±8,388,607 counts at gain=6.
// ==========================================================
#define MAX_DEVICE_ID_LEN 32

enum class MotionState : uint8_t {
    MOTION_STILL = 0,
    MOTION_LIGHT = 1,
    MOTION_HIGH  = 2
};

// ==========================================================
// WINDOW_SIZE must match config.h WINDOW_SIZE
// sampleValid[i] = 1  → sample i is motion-clean (STILL or LIGHT)
// sampleValid[i] = 0  → sample i is motion-corrupted (MOTION_HIGH)
// Raw data[i] is ALWAYS preserved regardless of sampleValid[i].
// Both channels (ch1 respiration, ch2 ECG) share one validity mask
// since motion corrupts both channels simultaneously.
// ==========================================================
#if RAW_DATA_ONLY
struct Block {
    int32_t  data[WINDOW_SIZE];          // ch2 ECG raw signed 24-bit ADC samples (100% RAW)
    uint32_t seq;                        // block sequence number
    bool     lo;                         // any lead-off detected this block
    bool     loPlus;                     // RA (IN1P / LO+) disconnected
    bool     loMinus;                    // LA (IN1N / LO-) disconnected
};
static_assert(sizeof(Block) == (WINDOW_SIZE * 4 + 8),
    "Block struct size mismatch in RAW_DATA_ONLY mode.");
#else
struct Block {
    int32_t  data[WINDOW_SIZE];          // ch2 ECG raw signed 24-bit ADC samples (100% UNTOUCHED)
    int32_t  filtered_data[WINDOW_SIZE]; // ch2 ECG DSP-filtered & motion-calibrated samples
    uint32_t seq;                        // block sequence number
    bool     lo;                         // any lead-off detected this block
    bool     loPlus;                     // RA (IN1P / LO+) disconnected
    bool     loMinus;                    // LA (IN1N / LO-) disconnected
    uint8_t  sampleValid[WINDOW_SIZE];   // per-sample validity: 1=clean, 0=motion-corrupted
};

static_assert(sizeof(Block) == ((WINDOW_SIZE * 9 + 7 + 3) & ~3),
    "Block struct size mismatch! Check types.h for unexpected padding.");
#endif



// ==========================================================
// Wave Measurement (P/T detection)
// ==========================================================
struct WaveMeasurement {
    bool  valid;
    float amplitude;      // signed, in raw ADC counts
    float timeMsFromR;
};

// ==========================================================
// 5 Signal Quality Performance Metrics struct
// ==========================================================
struct PerformanceMetrics {
    // 1. Waveform SNR & SNR Accuracy (%)
    float snrDb = 0.0f;
    float snrAccuracy = 0.0f;

    // 2. R-Peak Detection Accuracy (%)
    float rPeakAccuracy = 0.0f;

    // 3. Heart Rate & Heart Rate Accuracy (%)
    float hrBpm = 0.0f;
    float hrAccuracy = 0.0f;

    // 4. Baseline Wander (mV), CMRR (dB), & Baseline Accuracy (%)
    float baselineWanderMv = 0.0f;
    float cmrrEstDb = 86.0f;
    float baselineAccuracy = 0.0f;

    // 5. Motion Artifact Index & Motion Accuracy (%)
    float motionArtifactIndex = 0.0f;
    float motionAccuracy = 0.0f;
};
