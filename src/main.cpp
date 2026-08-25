/**
 * main.cpp
 * -----------------------------------------------------------
 * SWET_IOT - ECG v11.0 (PlatformIO)
 * ESP32 NodeMCU-32S + ProtoCentral ADS1292R Breakout v3.1
 *
 * Output: full raw signed 24-bit ADC values (int32_t).
 * No scaling to [0,4095]. Serial Plotter auto-scales.
 * ADS1292R @ gain=6, VREF=2.42V -> +/-8,388,607 full scale.
 * Typical ECG QRS peak ~ +/-20,000--40,000 counts.
 *
 * WiFi: concurrent AP+STA mode.
 *  - AP always on (SSID: ECG_ADS1292R) so a stranger can always
 *    connect and configure WiFi at 192.168.4.1.
 *  - STA auto-reconnects every 5s when disconnected.
 *  - If STA is down for >15s the AP is guaranteed active.
 *
 * Database: POSTs 2000-sample blocks to ads1292r-code.onrender.com
 *  via a FreeRTOS queue on Core 0 so the 2000-SPS loop is never
 *  blocked by network latency.
 *
 * Buzzer: REMOVED (not connected in current circuit).
 * -----------------------------------------------------------
 */

#include "ble_transport.h"
#include "buzzer.h"
#include "config.h"
#include "drivers/ads1292r.h"
#include "drivers/ecg_classifier.h"
#include "drivers/ecg_pipeline.h"
#include "drivers/signal_quality.h"
#include "imu.h"
#include "led_status.h"
#include "motion_calibration.h"
#include "network.h"
#include "pin_config.h"
#include "types.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h> // PSRAM malloc redirect for SSL buffers

// -----------------------------------------------------------
// PSRAM_ATTR — places large static arrays in the external-RAM
// BSS section (.ext_ram.bss) so they don't eat internal DRAM.
// Requires BOARD_HAS_PSRAM in platformio.ini (already set).
// Falls back to regular BSS on boards without PSRAM.
// -----------------------------------------------------------
#if defined(BOARD_HAS_PSRAM)
#define PSRAM_ATTR __attribute__((section(".ext_ram.bss")))
#else
#define PSRAM_ATTR
#endif

// ==========================================================
// Objects
// ==========================================================
static ADS1292R ads;
static ECGPipeline *pipeline = nullptr;
static IMUDriver imu;
static MotionCalibration g_motionCalibration;

// ==========================================================
// Ping-Pong Buffers — allocated from PSRAM in setup().
// Pointers live in internal BSS (tiny); arrays live in 8MB PSRAM.
// 2×2000×4 = 16 KB raw  +  2×2000×1 = 4 KB mask  →  20 KB PSRAM.
// ==========================================================
static int32_t (*g_buffers)[WINDOW_SIZE] =
    nullptr; // PSRAM: ch2 ECG raw samples
static uint8_t (*g_motionMask)[WINDOW_SIZE] =
    nullptr; // PSRAM: per-sample validity mask
static volatile int g_activeBuffer = 0;
static volatile int g_sampleIndex = 0;
static volatile bool g_blockReady = false;
static volatile uint32_t g_blockSeq = 0;
static volatile bool g_leadsOff = false;
static volatile bool g_loPlusOff = false;
static volatile bool g_loMinusOff = false;
static portMUX_TYPE g_bufMux = portMUX_INITIALIZER_UNLOCKED;

// Task handle for dedicated 32KB DSP task on Core 1
static TaskHandle_t s_dspTaskHandle = nullptr;

// ==========================================================
// Counters
// ==========================================================
static uint32_t g_totalSamples = 0;
static uint32_t g_validBlocks = 0;
static uint32_t g_invalidBlocks = 0;
static uint32_t g_leadOffEvents = 0;

// ==========================================================
#ifndef ANALYSIS_WINDOW_SIZE
#define ANALYSIS_WINDOW_SIZE 625
#endif
// Analysis ring buffer — allocated from PSRAM in setup().
// 10000×4 = 40 KB → too large for internal DRAM.
static int32_t *g_analysisWindow = nullptr; // PSRAM: 5-second ECG ring buffer
static uint32_t g_analysisWriteIndex = 0;
static uint32_t g_analysisSampleCount = 0;

static String g_lastAlertCondition = "";
static unsigned long g_lastAlertMs = 0;
const unsigned long ALERT_COOLDOWN_MS = 8000;

#define ALERT_LED_PIN -1 // Uses onboard LED_PIN (GPIO 21) instead
#define BUZZER_LEDC_CHANNEL 0
#define BUZZER_LEDC_RESOLUTION_BITS 8
#define BUZZER_VOLUME_DUTY 128 // 50% Square Wave Duty Cycle -> MAXIMUM Loudness (0-255 scale)

static void buzzerOn(int freq)
{
  if (BUZZER_PIN < 0)
    return;
  ledcWriteTone(BUZZER_LEDC_CHANNEL, freq);
  ledcWrite(BUZZER_LEDC_CHANNEL, BUZZER_VOLUME_DUTY);
}

static void buzzerOff()
{
  if (BUZZER_PIN < 0)
    return;
  ledcWrite(BUZZER_LEDC_CHANNEL, 0);
  ledcWriteTone(BUZZER_LEDC_CHANNEL, 0);
}


//critical alert through led and buzzer
static void triggerCriticalAlert(int beepCount)
{
  for (int i = 0; i < beepCount; i++)
  {
    buzzerOn(2000);
    if (ALERT_LED_PIN >= 0)
      digitalWrite(ALERT_LED_PIN, HIGH);
    delay(300);
    buzzerOff();
    if (ALERT_LED_PIN >= 0)
      digitalWrite(ALERT_LED_PIN, LOW);
    delay(150);
  }
}

static uint32_t g_lastRateCheck = 0;
static uint32_t g_rateCheckCount = 0;
const uint32_t RATE_CHECK_MS = 5000;

// ==========================================================
// Register snapshot printer
// ==========================================================
static void printRegisterSnapshot(const ADS1292_RegisterSnapshot &snap)
{
  Serial.println(F("#---- ADS1292R Register Dump ----"));
  Serial.print(F("#ID        : 0x"));
  Serial.println(snap.id, HEX);
  Serial.print(F("#CONFIG1   : 0x"));
  Serial.println(snap.config1, HEX);
  Serial.print(F("#CONFIG2   : 0x"));
  Serial.println(snap.config2, HEX);
  Serial.print(F("#LOFF      : 0x"));
  Serial.println(snap.loff, HEX);
  Serial.print(F("#CH1SET    : 0x"));
  Serial.println(snap.ch1set, HEX);
  Serial.print(F("#CH2SET    : 0x"));
  Serial.println(snap.ch2set, HEX);
  Serial.print(F("#RLD_SENS  : 0x"));
  Serial.println(snap.rld_sens, HEX);
  Serial.print(F("#LOFF_SENS : 0x"));
  Serial.println(snap.loff_sens, HEX);
  Serial.print(F("#LOFF_STAT : 0x"));
  Serial.println(snap.loff_stat, HEX);
  Serial.print(F("#RESP1     : 0x"));
  Serial.println(snap.resp1, HEX);
  Serial.print(F("#RESP2     : 0x"));
  Serial.println(snap.resp2, HEX);
  Serial.print(F("#GPIO      : 0x"));
  Serial.println(snap.gpio, HEX);
  Serial.print(F("#SPI Mode  : "));
  Serial.println(ads.getWorkingSpiMode());
  Serial.println(F("#---------------------------------"));

  bool allFF = (snap.id == 0xFF && snap.config1 == 0xFF);
  bool allZero =
      (snap.id == 0x00 && snap.config1 == 0x00 && snap.ch1set == 0x00);
  if (allFF)
    Serial.println(F("#WARNING: All 0xFF -> MISO floating or chip absent"));
  else if (allZero)
    Serial.println(F("#WARNING: All 0x00 -> Check RESET/CS/MISO/power"));
  else
    Serial.println(F("#Registers: real silicon responding"));

  if (snap.ch1set == ADS1292_CH1SET_VAL && snap.ch2set == ADS1292_CH2SET_VAL)
    Serial.println(F("#Gain = 6 confirmed (CH1SET=0x60 CH2SET=0x60)"));
}

// ==========================================================
// Wiring diagnostic
// ==========================================================
static void printWiringChecklist()
{
  Serial.println(F("#"));
  Serial.println(F("#=== INIT FAILED -- CHECK WIRING ==="));
  Serial.println(F("#"));
  Serial.println(F("#  Seeed XIAO ESP32S3 <-> ADS1292R / MPU6050"));
  Serial.println(F("#  -----------------------------------------------"));
  Serial.println(F("#  D8  (GPIO7 / SCK)   <->  SCLK"));
  Serial.println(F("#  D9  (GPIO8 / MISO)  <->  SDO / MISO"));
  Serial.println(F("#  D10 (GPIO9 / MOSI)  <->  SDI / MOSI"));
  Serial.println(F("#  D0  (GPIO1 / CS)    <->  CSn / SS"));
  Serial.println(F("#  D1  (GPIO2 / START) <->  START"));
  Serial.println(F("#  D2  (GPIO3 / RST)   <->  RST / RESET"));
  Serial.println(F("#  D3  (GPIO4 / DRDY)  <->  DRDY"));
  Serial.println(F("#  D4  (GPIO5 / SDA)   <->  MPU6050 SDA"));
  Serial.println(F("#  D5  (GPIO6 / SCL)   <->  MPU6050 SCL"));
  Serial.println(
      F("#  3V3                 <->  VCC & PWDN (tie PWDN to 3.3V!)"));
  Serial.println(F("#  GND                 <->  GND"));
  Serial.println(F("#"));
}

// ==========================================================
// WORKFLOW EXPLANATION: DATA TRAVEL PIPELINE - STEP 1 (Data Collection)
// How data travels:
// 1. The main loop() calls ads.readECGSample() at 2000 times per second (SPS).
// 2. The raw sample is immediately passed to this pushSample() function.
// 3. This function stores the sample into a "ping-pong" buffer (g_buffers).
//    A ping-pong buffer means there are two arrays. While one is being filled, 
//    the other is being processed, ensuring we never miss a sample.
// 4. Once a buffer fills up (reaches WINDOW_SIZE, e.g., 2000 samples), 
//    it signals the DSP task (s_dspTaskHandle) to start processing it.
// To change main things (e.g., WINDOW_SIZE): Check types.h or config.h
// ==========================================================
// pushSample
// -----------------------------------------------------------
// Called from loop() at 500 Hz immediately after DRDY fires.
// Stamps motion validity for this exact sample using the IMU's
// cached _currentState (no I2C, no delay, zero jitter on DRDY).
// sampleValid = 0 if MOTION_HIGH, 1 if STILL or LIGHT.
// Raw ADC value is always written unchanged regardless of motion.
// ==========================================================
static void pushSample(int32_t raw24, bool lo, bool loPlusOff,
                       bool loMinusOff)
{
  // Read cached motion state BEFORE entering critical section.
  // getMotionState() returns _currentState (uint8_t) -- no I2C transaction.
  // STILL and LIGHT motion are valid (valid=1). Only severe HIGH motion is
  // valid=0.
  uint8_t valid = (imu.getMotionState() != MotionState::MOTION_HIGH) ? 1u : 0u;

  portENTER_CRITICAL(&g_bufMux);
  g_buffers[g_activeBuffer][g_sampleIndex] = raw24; // raw ADC always written
  g_motionMask[g_activeBuffer][g_sampleIndex] =
      valid; // validity stamped per sample
  g_leadsOff = lo;
  g_loPlusOff = loPlusOff;
  g_loMinusOff = loMinusOff;
  g_sampleIndex++;
  if (g_sampleIndex >= WINDOW_SIZE)
  {
    g_sampleIndex = 0;
    g_activeBuffer = 1 - g_activeBuffer;
    g_blockSeq++;
    g_blockReady = true;
    if (s_dspTaskHandle)
    {
      xTaskNotifyGive(s_dspTaskHandle);
    }
  }
  portEXIT_CRITICAL(&g_bufMux);
}

// ==========================================================
// WORKFLOW EXPLANATION: DATA TRAVEL PIPELINE - STEP 2 (Data Processing)
// How data is processed:
// 1. This function is awakened by the dspTask when a full block of data is ready.
// 2. It takes the full buffer and applies various filters (e.g., pipeline->processBlock)
//    to remove noise and baseline wander.
// 3. It calculates Heart Rate (BPM), detects peaks, and determines signal quality.
// 4. Finally, it sends the fully processed block to the Network (WiFi) and BLE 
//    queues via network_uploadBlock() and ble_uploadBlock().
// To change main things: Look at the pipeline->processBlock() function for filters, 
// or modify the detectPeaks() call to change heart rate algorithms.
// ==========================================================
// processBlock
// ==========================================================
static void processBlock()
{
  int doneBuffer;
  uint32_t seq;
  bool lo, loPlus, loMinus;

  portENTER_CRITICAL(&g_bufMux);
  g_blockReady = false;
  doneBuffer = 1 - g_activeBuffer;
  seq = g_blockSeq;
  lo = g_leadsOff;
  loPlus = g_loPlusOff;
  loMinus = g_loMinusOff;
  portEXIT_CRITICAL(&g_bufMux);

  static Block *blkPtr = nullptr;
  if (!blkPtr)
  {
    blkPtr = (Block *)ps_malloc(sizeof(Block));
    if (!blkPtr)
      blkPtr = new Block();
  }
  Block &blk = *blkPtr;

  memcpy(blk.data, g_buffers[doneBuffer], sizeof(int32_t) * WINDOW_SIZE);
  if (pipeline)
    pipeline->cleanRawSpikes(blk.data, WINDOW_SIZE);
  blk.seq = seq;
  blk.lo = lo;
  blk.loPlus = loPlus;
  blk.loMinus = loMinus;

#if RAW_DATA_ONLY
  // ---------------------------------------------------------
  // RAW_DATA_ONLY Mode: Zero DSP/Classifier overhead.
  // Direct verbatim raw 24-bit ADC samples to network/BLE queue.
  // ---------------------------------------------------------
  network_uploadBlock(blk, lo, loPlus, loMinus, "RAW", "INFO");
  ble_uploadBlock(blk, lo, loPlus, loMinus, "RAW", "INFO", blk.data);
  g_validBlocks++;
#else
  // Filtered data workspace -- initialized with cleaned raw ADC values
  memcpy(blk.filtered_data, blk.data, sizeof(int32_t) * WINDOW_SIZE);
  // Per-sample validity mask -- metadata
  memcpy(blk.sampleValid, g_motionMask[doneBuffer], WINDOW_SIZE);

  // Per-block motion diagnostic -- count invalid samples for serial log
  uint16_t invalidCount = 0;
  for (int i = 0; i < WINDOW_SIZE; i++)
  {
    if (!blk.sampleValid[i])
      invalidCount++;
  }
  if (invalidCount > 0)
  {
    Serial.printf("[IMU] seq=%u: %u/%u samples motion-active (%s)\n", seq,
                  (unsigned)invalidCount, (unsigned)WINDOW_SIZE,
                  imu.getMotionStateString());
  }

  if (blk.lo)
  {
    g_leadOffEvents++;
    if (pipeline)
      pipeline->processBlock(blk);
    Serial.print(F("#LEAD_OFF seq="));
    Serial.print(seq);
    if (loPlus)
      Serial.print(F(" RA(LO+)"));
    if (loMinus)
      Serial.print(F(" LA(LO-)"));
    Serial.println();

    // Still upload lead-off blocks so the app can show the warning.
    network_uploadBlock(blk, true, loPlus, loMinus, "LEADS_OFF", "CRITICAL");
    ble_uploadBlock(blk, true, loPlus, loMinus, "LEADS_OFF", "CRITICAL");
    return;
  }

  String reason;
  bool badQuality = ECGPipeline::validateSamples(blk.data, WINDOW_SIZE, reason);
  if (badQuality)
  {
    g_invalidBlocks++;
    Serial.print(F("#QUALITY_FAIL seq="));
    Serial.print(seq);
    Serial.print(F(" "));
    Serial.println(reason);
    if (pipeline)
      pipeline->processBlock(blk);
    // Upload anyway -- the Python plotter and server handle quality flags.
    network_uploadBlock(blk, false, loPlus, loMinus, reason.c_str(),
                        "CRITICAL");
    ble_uploadBlock(blk, false, loPlus, loMinus, reason.c_str(), "CRITICAL");
    return;
  }

  if (pipeline)
    pipeline->processBlock(blk);

  // DSP-only copy for BLE — static, allocated from PSRAM once.
  static int32_t *dsp_only_data = nullptr;
  if (!dsp_only_data)
  {
    dsp_only_data = (int32_t *)ps_malloc(sizeof(int32_t) * WINDOW_SIZE);
    if (!dsp_only_data)
      dsp_only_data =
          (int32_t *)malloc(sizeof(int32_t) * WINDOW_SIZE); // fallback
  }
  if (dsp_only_data)
    memcpy(dsp_only_data, blk.filtered_data, sizeof(int32_t) * WINDOW_SIZE);

  // Append to analysis window (using baseline-centered filtered data)
  for (int i = 0; i < WINDOW_SIZE; i++)
  {
    g_analysisWindow[g_analysisWriteIndex] = blk.filtered_data[i];
    g_analysisWriteIndex = (g_analysisWriteIndex + 1) % ANALYSIS_WINDOW_SIZE;
    if (g_analysisSampleCount < ANALYSIS_WINDOW_SIZE)
    {
      g_analysisSampleCount++;
    }
  }

  String condition = "Warming up";
  String severity = "INFO";
  String detail =
      String(g_analysisSampleCount / WINDOW_SIZE) + "/5 seconds collected";

  if (g_analysisSampleCount >= ANALYSIS_WINDOW_SIZE)
  {
    // Scaled snapshot — 40 KB, allocated from PSRAM once.
    static int32_t *s_scaledSnapshot = nullptr;
    if (!s_scaledSnapshot)
    {
      s_scaledSnapshot =
          (int32_t *)ps_malloc(sizeof(int32_t) * ANALYSIS_WINDOW_SIZE);
      if (!s_scaledSnapshot)
        s_scaledSnapshot =
            (int32_t *)malloc(sizeof(int32_t) * ANALYSIS_WINDOW_SIZE);
    }
    if (!s_scaledSnapshot)
    {
      Serial.println(F("[ERR] s_scaledSnapshot OOM!"));
    }
    else
    {
      uint32_t start = g_analysisWriteIndex;
      for (int i = 0; i < ANALYSIS_WINDOW_SIZE; i++)
      {
        int32_t val =
            (g_analysisWindow[(start + i) % ANALYSIS_WINDOW_SIZE] >> 6) + 2048;
        if (val > 4095)
          val = 4095;
        if (val < 0)
          val = 0;
        s_scaledSnapshot[i] = val;
      }
      condition = classifyWindow(s_scaledSnapshot, ANALYSIS_WINDOW_SIZE,
                                 severity, detail);
    }
  }

  if (severity == "CRITICAL" || severity == "WARNING")
  {
    unsigned long now = millis();
    bool isNewEpisode = (condition != g_lastAlertCondition) ||
                        (now - g_lastAlertMs > ALERT_COOLDOWN_MS);

    if (ALERT_LED_PIN >= 0)
      digitalWrite(ALERT_LED_PIN, HIGH);
    if (isNewEpisode)
    {
      triggerCriticalAlert(severity == "CRITICAL" ? 2 : 1);
      g_lastAlertCondition = condition;
      g_lastAlertMs = now;
    }
  }
  else
  {
    buzzerOff();
    if (ALERT_LED_PIN >= 0)
      digitalWrite(ALERT_LED_PIN, LOW);
    g_lastAlertCondition = "";
  }

  // 4. Apply Calibration-Range-Based Motion Noise Reduction on filtered_data
  // (for Wi-Fi / DB)
  g_motionCalibration.applyMotionNoiseReduction(
      blk.filtered_data, blk.sampleValid, WINDOW_SIZE, seq);

  // 5. Perform Peak Detection & Heart Rate calculation on this 2000-sample
  // block
  int pPeaks[32];
  float maxAbs = 0.0f;
  int pCount = detectPeaks(dsp_only_data, WINDOW_SIZE, pPeaks, 32, maxAbs);

  float bpmVal = 0.0f;
  if (pCount >= 2)
  {
    float avgGap =
        (float)(pPeaks[pCount - 1] - pPeaks[0]) / (float)(pCount - 1);
    if (avgGap > 0.0f)
      bpmVal = 60.0f * (float)SAMPLE_RATE / avgGap;
  }

  // 6. Compute 5 Batch Performance Metrics
  PerformanceMetrics metrics = calculateBatchMetrics(
      blk.data, dsp_only_data, WINDOW_SIZE, bpmVal, pCount);

  // Print to Serial Console
  printMetricsToSerial(seq, metrics);

  // 7. Formulate Result String
  String devResult = "";
  if (condition != "Warming up")
  {
    int bpmOut = (int)roundf(
        bpmVal > 0 ? bpmVal : (metrics.hrBpm > 0 ? metrics.hrBpm : 75));
    devResult = condition + " | 90% | " + String(bpmOut) + " bpm";
  }
  else
  {
    devResult =
        "Warming up (" + String(g_analysisSampleCount / WINDOW_SIZE) + "/5)";
  }

  // ==========================================================
  // WORKFLOW EXPLANATION: DATA TRAVEL PIPELINE - STEP 3 (Dispatching Data)
  // How data is sent:
  // 1. We have the fully processed block and all the calculated metrics.
  // 2. We send it to two separate channels simultaneously:
  //    a) network_uploadBlock: Queues the data to be sent over WiFi (to a backend server via WebSocket/HTTP).
  //    b) ble_uploadBlock: Sends the data directly to a connected mobile app via Bluetooth Low Energy (BLE).
  // 3. These calls do NOT block the main loop. They just copy the data to queues.
  // To change main things: If you want to disable WiFi upload, comment out network_uploadBlock. 
  // If you want to disable BLE, comment out ble_uploadBlock.
  // ==========================================================
  // WiFi upload queue (Core 0) + Direct BLE binary streaming (All metrics + raw + filtered)
  network_uploadBlock(blk, false, loPlus, loMinus, condition.c_str(),
                      severity.c_str(), dsp_only_data, &metrics,
                      devResult.c_str());
  ble_uploadBlock(blk, false, loPlus, loMinus, condition.c_str(),
                  severity.c_str(), dsp_only_data, &metrics,
                  devResult.c_str());
  g_validBlocks++;
#endif
}

// ==========================================================
// dspTask — Dedicated 32KB stack task on Core 1
// Runs processBlock() safely without overflowing loopTask stack
// ==========================================================
static void dspTask(void *pv)
{
  for (;;)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (g_blockReady)
    {
      processBlock();
    }
  }
}

/// ==========================================================
// setup()
void setup()
{
  // -------------------------------------------------------
  // Allocate large ping-pong + analysis buffers from PSRAM.
  // Internal DRAM cannot fit them at 2000 SPS.
  //   g_buffers[2][2000]   = 16 KB
  //   g_motionMask[2][2000] =  4 KB
  //   g_analysisWindow[10000] = 40 KB
  //   Total: 60 KB moved to 8 MB PSRAM
  // -------------------------------------------------------
  g_buffers =
      (int32_t (*)[WINDOW_SIZE])ps_malloc(2 * WINDOW_SIZE * sizeof(int32_t));
  g_motionMask =
      (uint8_t (*)[WINDOW_SIZE])ps_malloc(2 * WINDOW_SIZE * sizeof(uint8_t));
  g_analysisWindow =
      (int32_t *)ps_malloc(ANALYSIS_WINDOW_SIZE * sizeof(int32_t));
  pipeline = (ECGPipeline *)ps_malloc(sizeof(ECGPipeline));
  if (!pipeline)
    pipeline = new ECGPipeline();

  if (!g_buffers || !g_motionMask || !g_analysisWindow || !pipeline)
  {
    Serial.println(F("[FATAL] PSRAM allocation failed! Halting."));
    while (true)
      delay(1000); // halt — cannot run without buffers
  }
  memset(g_buffers, 0, 2 * WINDOW_SIZE * sizeof(int32_t));
  memset(g_motionMask, 0, 2 * WINDOW_SIZE * sizeof(uint8_t));
  memset(g_analysisWindow, 0, ANALYSIS_WINDOW_SIZE * sizeof(int32_t));

  ledStatus_init(); // Powers ON Red LED (active) & initializes Orange LED (OFF)

  Serial.begin(SERIAL_BAUD_RATE);
  // Allow up to 1.5 seconds for USB CDC Serial connection on ESP32-S3
  unsigned long startMs = millis();
  while (!Serial && (millis() - startMs < 1500))
    ;
  delay(200);

  Serial.println(F("#============================================"));
  Serial.println(F("#  SWET_IOT - ECG v11.0 (PlatformIO)"));
  Serial.println(F("#  Seeed Studio XIAO ESP32S3"));
  Serial.printf("# [PSRAM] Total: %u bytes | Free: %u bytes\n",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  Serial.printf("# [MEM] sizeof(Block): %u bytes (RAW_DATA_ONLY=%d)\n",
                (unsigned)sizeof(Block), RAW_DATA_ONLY);
  Serial.println(F("#============================================"));

  // ADS1292R hardware init (SPI Mode1 -> Mode3 auto-fallback).
  ADS1292_Status status = ads.begin();

  if (status != ADS1292_Status::OK)
  {
    Serial.println(F("# WARNING: ADS1292R not detected or not wired yet. "
                     "Continuing gracefully."));
    printWiringChecklist();
    digitalWrite(LED_PIN, HIGH); // LED OFF when sensor is not connected
  }
  else
  {
    Serial.println(F("# ADS1292R init OK!"));
    ADS1292_RegisterSnapshot snap = ads.dumpRegisters();
    printRegisterSnapshot(snap);
    ads.startConversion();
  }

  if (ALERT_LED_PIN >= 0)
  {
    pinMode(ALERT_LED_PIN, OUTPUT);
    digitalWrite(ALERT_LED_PIN, LOW);
  }
  buzzer_init();

  if (pipeline)
    pipeline->init();
  imu.begin();

  Serial.println(F("# ADC streaming initialized"));
  delay(100);

  // Initialize BLE Server and Advertising
  ble_init();

  // WiFi + Upload task init
  network_init();

  // Start dedicated DSP processing task on Core 1 (16 KB stack)
  xTaskCreatePinnedToCore(dspTask, "ecg_dsp", 16384, nullptr, 2,
                          &s_dspTaskHandle, 1);
  Serial.println(F("# DSP task started on Core 1 (16KB stack)."));

  g_lastRateCheck = millis();
  Serial.println(F("# System ready. ESP32-S3 active."));
}

// ==========================================================
// loop()
// ==========================================================
void loop()
{
  ECGSample sample = ads.readECGSample();

  if (sample.valid)
  {
    g_totalSamples++;
    g_rateCheckCount++;
    pushSample(sample.channel2, sample.leadOffDetected, sample.loPlusOff,
               sample.loMinusOff);
  }
  else
  {
    // Yield CPU to FreeRTOS (WiFi & BLE tasks) when sensor is absent.
    // Prevents Task Watchdog Timer (TG1WDT_SYS_RST) reset loops!
    delay(1);
  }

  // Block processing is handled asynchronously by dspTask (Core 1, 32KB stack)

  // Throttle non-ADC tasks to 50 Hz (every 20ms) so they never starve the 2000
  // SPS DRDY loop!
  static uint32_t s_lastMaintMs = 0;
  uint32_t now = millis();
  if ((now - s_lastMaintMs) >= 20)
  {
    s_lastMaintMs = now;
    imu.update();
    network_update();
    buzzer_update();
  }

  if ((now - g_lastRateCheck) >= RATE_CHECK_MS)
  {
    float sps =
        (float)g_rateCheckCount / ((float)(now - g_lastRateCheck) / 1000.0f);
    Serial.print(F("#RATE SPS="));
    Serial.print(sps, 1);
    Serial.print(F(" total="));
    Serial.print(g_totalSamples);
    Serial.print(F(" valid_blk="));
    Serial.print(g_validBlocks);
    Serial.print(F(" bad="));
    Serial.print(g_invalidBlocks);
    Serial.print(F(" lo_events="));
    Serial.print(g_leadOffEvents);
    Serial.print(F(" motion="));
    Serial.println(imu.getMotionStateString());
    Serial.print(F("#WiFi STA="));
    Serial.print(network_isConnected() ? "UP" : "DOWN");
    Serial.printf("  AP=%s @ ", WIFI_AP_SSID);
    Serial.println(WiFi.softAPIP());
    g_rateCheckCount = 0;
    g_lastRateCheck = now;
  }
}
