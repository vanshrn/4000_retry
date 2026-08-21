/**
 * network.h
 * -----------------------------------------------------------
 * WiFi AP+STA concurrent manager + HTTP upload queue.
 *
 * WiFi Behaviour (mirrors AD8232 v9.4):
 *  - Loads saved credentials from NVS (Preferences "ecg_cfg" namespace).
 *  - On boot: tries STA first. Falls back to AP "ECG_ADS1292R" if fails.
 *  - Blue LED (GPIO2): ON when STA connected, blinking when AP-only.
 *  - /wifi-config POST endpoint: mobile app sends credentials â†’ ESP
 *    connects to new network, saves creds, stops AP, LED turns ON.
 *  - STA drops: reconnects every 5s. After 3 fails â†’ starts AP.
 *  - STA down > 15s: AP guaranteed active.
 *
 * POST Payload (matches AD8232 v9.4 exactly):
 *   https://ads1292r-code-91eg.onrender.com/api/ecg
 *   { userId, deviceId, seq, sr, lo, loPlus, loMinus, data[] }
 * -----------------------------------------------------------
 */

#pragma once
#include "types.h"
#include <Arduino.h>
#include <stdbool.h>
#include <stdint.h>

// Network management states
enum NetState
{
  NET_STATE_STA_BOOT =
      0,                       // Boot / Disconnected: STA auto-connect attempt only (0-20s). AP OFF.
  NET_STATE_PROVISIONING_DUAL, // Fallback: AP ON (ECG_ADS1292R) + STA
                               // auto-connect retries + BLE advertising
  NET_STATE_WIFI_CONNECTED,    // Exclusive: STA connected. AP OFF, BLE advertising
                               // stopped.
  NET_STATE_BLE_CONNECTED      // Exclusive: BLE connected. WiFi OFF.
};

// Call once in setup() after ADS1292R is initialised.
void network_init();

// Call every loop() — handles state machine, reconnect timers, web server, and
// LED state.
void network_update();

// Enqueue a processed Block for upload. Non-blocking.
// dspOnlyData: DSP pipeline filtered data (written to DB raw field)
// blk.filtered_data: Motion noise reduced data (written to DB filtered field)
bool network_uploadBlock(const Block &blk, bool leadsOff, bool loPlus,
                         bool loMinus, const char *warning = nullptr,
                         const char *severity = nullptr,
                         const int32_t *dspOnlyData = nullptr,
                         const PerformanceMetrics *metrics = nullptr,
                         const char *deviceResult = nullptr);

// Returns true if STA is currently connected.
bool network_isConnected();

// Returns current network state machine enum.
NetState network_getState();

// Helper to save WiFi credentials and trigger non-blocking connect
void network_saveCredentialsAndConnect(const String &ssid, const String &pass,
                                       const String &deviceId = "",
                                       const String &userId = "");

// Generate the JSON string payload (exposed for BLE reuse)
void network_buildJsonPayload(const Block &blk, bool leadsOff, bool loPlus,
                              bool loMinus, const char *warning,
                              const char *severity, const char *mode,
                              String &out);

// Synchronously transmit emergency thermal shutdown alert payload to backend
void network_sendThermalShutdownAlert(float tempC);
