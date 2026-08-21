/**
 * config.h  — Register values corrected for ADS1292R @ 2000 SPS
 */

#pragma once

// ==========================================================
// Mode Configuration
// ==========================================================
#define RAW_DATA_ONLY                0          // 0 = Generate both raw ADC data and DSP filtered data

// ==========================================================
// Serial
// ==========================================================
#define SERIAL_BAUD_RATE            115200

// ==========================================================
// SPI
// ==========================================================
#define ADS1292_SPI_CLOCK_HZ         1000000UL  // 1 MHz
#define ADS1292_SPI_BIT_ORDER        MSBFIRST
#define ADS1292_SPI_MODE             SPI_MODE1   // CPOL=0, CPHA=1

// ==========================================================
// Timing
// ==========================================================
#define ADS1292_RESET_PULSE_MS       100   // match ProtoCentral: 100 ms reset pulse
#define ADS1292_POWERUP_DELAY_MS     150
#define ADS1292_DRDY_TIMEOUT_MS      10    // 10ms timeout

// ==========================================================
// Sampling — 2000 SPS (CONFIG1 = 0x04)
// ADS1292R DR[2:0] = 100 → 2000 SPS
// ==========================================================
#define SAMPLE_RATE                  2000
#define WINDOW_SIZE                  2000       // 1.0 second @ 2000 SPS (2000 points in 1 block, 1 POST every 1s)
#define ANALYSIS_WINDOW_SIZE         10000      // 5 seconds @ 2000 SPS

// ==========================================================
// ADS1292R Register Values — EXACTLY from ProtoCentral library
// (protocentralAds1292r.cpp ads1292Init)
// ==========================================================
#define ADS1292_CONFIG1_VAL          0x04          // 2000 SPS (DR[2:0] = 100)
#define ADS1292_CONFIG2_VAL          0b11100000    // 0xE0 — Lead-off comp POWERED UP (bit 6 = 1)
#define ADS1292_LOFF_VAL             0b00010000    // 0x10 — Lead-off defaults (95%, 6nA)
#define ADS1292_CH1SET_VAL           0b00000000    // 0x00 — Ch1 enabled, gain 6, electrode in
#define ADS1292_CH2SET_VAL           0b00000000    // 0x00 — Ch2 enabled, gain 6, electrode in
#define ADS1292_RLD_SENS_VAL         0b00101100    // 0x2C — RLD sensing on Ch2 (P and N)
#define ADS1292_LOFF_SENS_VAL        0b00001100    // 0x0C — LOFF sensing enabled on CH2P and CH2N (bits 2 and 3)
#define ADS1292_RESP1_VAL            0b00000010    // 0x02 — Respiration MOD/DEMOD OFF
#define ADS1292_RESP2_VAL            0b00000011    // 0x03 — Calib OFF, respiration freq defaults

// ==========================================================
// Signal Polarity Fix
// ==========================================================
#define ECG_INVERT_CH2               0

// ==========================================================
// Baseline Wander Removal (median-based)
// ==========================================================
#define BL_STAGE1_WIN                400        // 200ms chunk @ 2000 SPS
#define BL_CANDS_PER_BLOCK           (WINDOW_SIZE / BL_STAGE1_WIN)
#define BL_STAGE2_WIN                5          // Median of 5 chunks
#define BL_HISTORY_LEN               16
#define BL_MAX_STEP_PER_CAND         1500.0f
#define BL_CONTAM_PTP_THRESHOLD      8000.0f

// ==========================================================
// FIR Low-Pass
// ==========================================================
#define LP_MOVING_AVG_WIN            3

// ==========================================================
// Validation (ADS1292R 24-bit ADC @ 2000 SPS)
// ==========================================================
#define VALIDATE_SPIKE_DELTA_MAX     500000     // 500k counts/sample
#define VALIDATE_NOISE_RATIO_MAX     0.30f

// ==========================================================
// Calibration-Range-Based Motion Noise Reduction Config
// ==========================================================
#define CALIBRATION_WINDOW_SAMPLES   10000  // 5s @ 2000 SPS rolling stillness calibration
#define CALIBRATION_MIN_SAMPLES      4000   // 2s @ 2000 SPS minimum stillness before active
#define CALIBRATION_STD_MULTIPLIER   3.0f   // Clamp bound
#define MOTION_SMOOTHING_MIN_ALPHA   0.05f
#define MOTION_SMOOTHING_MAX_ALPHA   0.65f
#define CALIBRATION_EPSILON_STD      30.0f

// ==========================================================
// Gyroscope & Thermal Thresholds
// ==========================================================
#define GYRO_STILL_THRESHOLD_DPS     10.0f
#define GYRO_LIGHT_THRESHOLD_DPS     35.0f
#define DIE_TEMP_WARN_C              60.0f
#define DIE_TEMP_CRIT_C              75.0f

// ==========================================================
// Debug
// ==========================================================
#define DEBUG_ENABLED                1
#if DEBUG_ENABLED
    #define DBG_PRINT(x)     Serial.print(x)
    #define DBG_PRINTLN(x)   Serial.println(x)
    #define DBG_PRINTF(...)  Serial.printf(__VA_ARGS__)
#else
    #define DBG_PRINT(x)
    #define DBG_PRINTLN(x)
    #define DBG_PRINTF(...)
#endif

// ==========================================================
// WiFi & Network
// ==========================================================
#define WIFI_AP_SSID                 "ECG_Setup"
#define WIFI_AP_PASSWORD             "12345678"
#define WIFI_AP_TIMEOUT_MS           15000
#define WIFI_RECONNECT_INTERVAL_MS   5000
#define WIFI_CONNECT_TIMEOUT_MS      10000

// ==========================================================
// Hardware
// ==========================================================
#define LED_PIN                      21

// ==========================================================
// Database / API
// ==========================================================
#define API_HOST                     "ads1292r-code.onrender.com"
#define API_ENDPOINT                 "/api/ecg"
#define API_DEVICE_ID                "ESP_ECG_123"
#define API_USER_ID                  "ESP_ECG_123"
#define API_TIMEOUT_MS               15000
#define NVS_NAMESPACE                "ecg_cfg"

// ==========================================================
// FreeRTOS Upload Queue
// ==========================================================
#define UPLOAD_QUEUE_DEPTH           50
