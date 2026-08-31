/**
 * network.cpp
 * -----------------------------------------------------------
 * WiFi AP+STA manager + HTTP upload queue (ADS1292R Edition @ 4000 SPS)
 *
 * WiFi Behaviour:
 *  - On boot: loads saved credentials from NVS (Preferences).
 *  - Tries STA first. If it fails/no creds → starts AP "ECG_ADS1292R".
 *  - Blue LED (GPIO2) ON when STA is connected, OFF otherwise.
 *  - AP runs concurrently (WIFI_AP_STA) while STA reconnects.
 *  - When a client connects to AP and POSTs /wifi-config, AP stops
 *    and STA takes over — LED turns ON.
 *  - If STA drops: reconnects every 5s. If 3 fails → starts AP again.
 *  - AP is also force-started if STA is down for > 15s.
 *
 * Payload format:
 * {
 *   "userId":   "ESP_ECG_123",
 *   "deviceId": "ESP_ECG_123",
 *   "seq":      <uint32>,
 *   "sr":       4000,
 *   "lo":       false,
 *   "loPlus":   false,
 *   "loMinus":  false,
 *   "data":     [[raw0, filt0], ...]   // 4000 full-scale ADS1292R sample pairs
 * @ 4000 SPS
 * }
 *
 * POST endpoint: https://ads1292r-code-91eg.onrender.com/api/ecg
 * (NOT /api/ecg/live/... — that is the SSE GET stream for Python)
 * -----------------------------------------------------------
 */

#include "network.h"
#include "ble_transport.h"
#include "buzzer.h"
#include "config.h"
#include "led_status.h"
#include "signal_quality.h"
#include "types.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#define DEBUG_WEBSOCKETS(...) Serial.printf( __VA_ARGS__ )
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include "esp_heap_caps.h"
#define JSON_BUF_SIZE \
  (256 * 1024) // 256 KB — allocated in PSRAM for 8000 sample 2D payload (4000 SPS)
#define NUM_UPLOAD_WORKERS 3
#include <inttypes.h> // PRId32 format specifier for int32_t in snprintf

// -----------------------------------------------------------
// PSRAM_ATTR — same macro as in main.cpp; places large statics
// in PSRAM BSS so they don't exhaust internal DRAM.
// -----------------------------------------------------------
#if defined(BOARD_HAS_PSRAM)
#define PSRAM_ATTR __attribute__((section(".ext_ram.bss")))
#else
#define PSRAM_ATTR
#endif

// -----------------------------------------------------------
// Upload queue payload — holds raw and filtered data arrays separately
// -----------------------------------------------------------
struct UploadPayload
{
  int32_t raw_data[WINDOW_SIZE];
  int32_t filtered_data[WINDOW_SIZE];
  uint32_t seq;
  bool leadsOff;
  bool loPlus;
  bool loMinus;
  char warning[32];
  char severity[32];
  char mode[16];
  char deviceResult[128];
  PerformanceMetrics metrics;
  bool hasMetrics;
};

// -----------------------------------------------------------
// Network & Upload State
// -----------------------------------------------------------
static UploadPayload *s_payloadPool = nullptr;
static QueueHandle_t s_uploadQueue = nullptr;
static QueueHandle_t s_freePayloadQueue = nullptr;
static WebServer s_server(80);
static Preferences s_prefs;

static WebSocketsClient s_webSocket;
static volatile bool s_wsConnected = false;
static volatile uint32_t s_lastWsConnectedMs = 0;
static SemaphoreHandle_t s_wsMutex = nullptr;
static uint32_t s_lastWsLogMs = 0;

static NetState s_netState = NET_STATE_STA_BOOT;
static bool s_staConnected = false;
static bool s_apActive = false;
static bool s_hasCredentials = false;

static String s_savedSsid;
static String s_savedPass;
static char s_deviceId[MAX_DEVICE_ID_LEN];
static char s_userId[MAX_DEVICE_ID_LEN];

static uint32_t s_bootConnectStartMs = 0;
static uint32_t s_lastReconnectMs = 0;
static uint32_t s_connectStartMs = 0;
static uint32_t s_dualModeStartMs = 0;
static bool s_staRetryAllowed = true;

// -----------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------
static void wsTaskWorker(void *pv);
static void uploadTask(void *pv);
static void localStatusTask(void *pv);
static size_t buildJsonBuffer(const UploadPayload &p, char *buf,
                              size_t bufSize);
static bool postPayload(const char *buf, size_t len);
static void ledOn();
static void ledOff();
static void saveCredentials(const String &ssid, const String &pass);
static bool loadCredentials(String &ssid, String &pass);

static void enterBootStaMode();
static void enterProvisioningDualMode();
static void enterWifiConnectedMode();
static void closeHttpConnection();

// -----------------------------------------------------------
// LED helpers (Driven by ledStatus_update())
// -----------------------------------------------------------
static void ledOn() { ledStatus_update(); }
static void ledOff() { ledStatus_update(); }

// -----------------------------------------------------------
// Credentials (NVS)
// -----------------------------------------------------------
static void saveCredentials(const String &ssid, const String &pass)
{
  s_prefs.begin(NVS_NAMESPACE, false);
  s_prefs.putString("wifi_ssid", ssid);
  s_prefs.putString("wifi_pass", pass);
  s_prefs.end();
  Serial.println("[NET] WiFi credentials saved to NVS.");
}

static bool loadCredentials(String &ssid, String &pass)
{
  s_prefs.begin(NVS_NAMESPACE, true);
  ssid = s_prefs.getString("wifi_ssid", "");
  pass = s_prefs.getString("wifi_pass", "");
  s_prefs.end();
  return ssid.length() > 0;
}

static void loadIds()
{
  s_prefs.begin(NVS_NAMESPACE, true);
  String did = s_prefs.getString("device_id", API_DEVICE_ID);
  String uid = s_prefs.getString("user_id", API_USER_ID);
  s_prefs.end();
  did.toCharArray(s_deviceId, MAX_DEVICE_ID_LEN);
  uid.toCharArray(s_userId, MAX_DEVICE_ID_LEN);
}

NetState network_getState() { return s_netState; }

void network_saveCredentialsAndConnect(const String &ssid, const String &pass,
                                       const String &did, const String &uid)
{
  if (did.length() > 0)
  {
    did.toCharArray(s_deviceId, MAX_DEVICE_ID_LEN);
    s_prefs.begin(NVS_NAMESPACE, false);
    s_prefs.putString("device_id", did);
    s_prefs.end();
  }
  if (uid.length() > 0)
  {
    uid.toCharArray(s_userId, MAX_DEVICE_ID_LEN);
    s_prefs.begin(NVS_NAMESPACE, false);
    s_prefs.putString("user_id", uid);
    s_prefs.end();
  }

  if (ssid.length() > 0 && pass.length() > 0)
  {
    saveCredentials(ssid, pass);
    s_savedSsid = ssid;
    s_savedPass = pass;
    s_hasCredentials = true;

    Serial.printf(
        "[NET] Credentials updated (SSID: %s). Initiating connection...\n",
        ssid.c_str());

    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(s_savedSsid.c_str(), s_savedPass.c_str());
    s_connectStartMs = millis();
    s_lastReconnectMs = millis();
  }
}

// -----------------------------------------------------------
// State Transitions
// -----------------------------------------------------------
static void enterBootStaMode()
{
  Serial.println(
      F("[NET] Entering STATE 1: STA Boot Auto-Connect (0-20s, AP OFF)..."));
  s_netState = NET_STATE_STA_BOOT;
  s_bootConnectStartMs = millis();
  s_staConnected = false;
  s_apActive = false;

  WiFi.mode(WIFI_STA);

  if (s_hasCredentials)
  {
    Serial.printf("[NET] STA Boot: Auto-connecting to '%s'...\n",
                  s_savedSsid.c_str());
    WiFi.begin(s_savedSsid.c_str(), s_savedPass.c_str());
    s_connectStartMs = millis();
  }
  else
  {
    Serial.println(F("[NET] STA Boot: No saved credentials present."));
  }

  ble_startAdvertising();
  ledOff();
}

static void enterProvisioningDualMode()
{
  Serial.println(F("[NET] Entering STATE 2: Provisioning Dual Mode (AP ON + "
                   "STA 30s Retries + BLE Advertising)..."));
  s_netState = NET_STATE_PROVISIONING_DUAL;
  s_staConnected = false;
  s_dualModeStartMs = millis();
  s_staRetryAllowed = true;

  WiFi.mode(WIFI_AP_STA);

  bool ok = WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
  if (!ok)
  {
    WiFi.mode(WIFI_AP_STA);
    ok = WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
  }
  if (ok)
  {
    s_apActive = true;
    Serial.print(F("[NET] SoftAP Started. SSID: "));
    Serial.print(WIFI_AP_SSID);
    Serial.print(F(" | IP: "));
    Serial.println(WiFi.softAPIP());
  }
  else
  {
    Serial.println(F("[NET] SoftAP start FAILED!"));
  }

  ble_startAdvertising();

  if (s_hasCredentials)
  {
    Serial.printf("[NET] Dual Mode: Auto-connecting to '%s' (30s window)...\n",
                  s_savedSsid.c_str());
    WiFi.begin(s_savedSsid.c_str(), s_savedPass.c_str());
    s_connectStartMs = millis();
    s_lastReconnectMs = millis();
  }

  ledOff();
}

static void enterWifiConnectedMode()
{
  Serial.println(F("[NET] Entering STATE 3: WiFi Connected Mode..."));
  s_netState = NET_STATE_WIFI_CONNECTED;
  s_staConnected = true;

  // Turn ON Connectivity LED (silent, no buzzer beep)
  ledStatus_setWifiConnected(true);

  ble_startAdvertising();
  ledOn();
  Serial.print(F("[NET] STA connected! IP="));
  Serial.println(WiFi.localIP());

  // Push JSON status update to BLE status characteristic for mobile app
  String ipStr = WiFi.localIP().toString();
  String jsonStatus = "{\"status\":\"wifi_connected\",\"ip\":\"" + ipStr +
                      "\",\"deviceId\":\"" + s_deviceId + "\"}";
  ble_sendStatus(jsonStatus);
}

// -----------------------------------------------------------
// Web Server CORS and Handlers
// -----------------------------------------------------------
static void sendCORSHeaders()
{
  s_server.sendHeader("Access-Control-Allow-Origin", "*");
  s_server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  s_server.sendHeader("Access-Control-Allow-Headers",
                      "Content-Type, Authorization, X-Requested-With");
}

static void handleNotFound()
{
  sendCORSHeaders();
  if (s_server.method() == HTTP_OPTIONS)
  {
    s_server.send(204);
    return;
  }
  s_server.send(404, "application/json", "{\"error\":\"Not found\"}");
}

static void handleWifiConfig()
{
  sendCORSHeaders();
  if (s_server.method() == HTTP_OPTIONS)
  {
    s_server.send(204);
    return;
  }

  if (!s_server.hasArg("plain"))
  {
    s_server.send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }
  String body = s_server.arg("plain");
  Serial.println("[NET] /wifi-config: " + body);

  JsonDocument doc;
  if (deserializeJson(doc, body))
  {
    s_server.send(400, "application/json", "{\"error\":\"Bad JSON\"}");
    return;
  }

  String ssid = doc["ssid"] | "";
  String password = doc["password"] | "";
  String newDid = doc["deviceId"] | "";
  String newUid = doc["userId"] | "";

  network_saveCredentialsAndConnect(ssid, password, newDid, newUid);

  String resp = "{";
  resp += "\"status\":\"ok\",";
  resp += "\"message\":\"WiFi credentials saved. Connecting to WiFi...\",";
  resp += "\"connected\":" +
          String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  resp += "\"deviceId\":\"" + String(s_deviceId) + "\",";
  resp += "\"userId\":\"" + String(s_userId) + "\",";
  resp += "\"ssid\":\"" + String(s_savedSsid) + "\"";
  resp += "}";

  s_server.send(200, "application/json", resp);
  Serial.println("[CFG] Credentials saved & HTTP response sent.");
}

static void handleStatus()
{
  sendCORSHeaders();
  if (s_server.method() == HTTP_OPTIONS)
  {
    s_server.send(204);
    return;
  }

  bool connected = (WiFi.status() == WL_CONNECTED);
  String ipStr = connected
                     ? WiFi.localIP().toString()
                     : (s_apActive ? WiFi.softAPIP().toString() : "0.0.0.0");

  String resp = "{";
  resp += "\"status\":\"ok\",";
  resp += "\"connected\":" + String(connected ? "true" : "false") + ",";
  resp += "\"apActive\":" + String(s_apActive ? "true" : "false") + ",";
  resp += "\"state\":" + String((int)s_netState) + ",";
  resp += "\"deviceId\":\"" + String(s_deviceId) + "\",";
  resp += "\"userId\":\"" + String(s_userId) + "\",";
  resp += "\"ip\":\"" + ipStr + "\",";
  resp += "\"ssid\":\"" + String(s_savedSsid) + "\"";
  resp += "}";

  s_server.send(200, "application/json", resp);
}

// -----------------------------------------------------------
// network_init()
// -----------------------------------------------------------
void network_init()
{
  pinMode(LED_PIN, OUTPUT);
  ledOff();

  loadIds();
  Serial.printf("[NET] deviceId=%s  userId=%s\n", s_deviceId, s_userId);

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
  s_apActive = true;
  WiFi.setSleep(true); // MANDATORY: ESP-IDF requires Wi-Fi modem sleep = true when both WiFi and Bluetooth are active
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  WiFi.setTxPower(WIFI_POWER_17dBm); // Limit peak TX current spikes
  WiFi.setAutoReconnect(true);       // Native ESP-IDF background auto-reconnect
  delay(100);

  s_server.on("/wifi-config", HTTP_POST, handleWifiConfig);
  s_server.on("/wifi-config", HTTP_OPTIONS, handleWifiConfig);
  s_server.on("/status", HTTP_GET, handleStatus);
  s_server.on("/status", HTTP_OPTIONS, handleStatus);
  s_server.onNotFound(handleNotFound);
  s_server.begin();
  Serial.println(F("[NET] Web server started on port 80 (CORS enabled)."));

  String ssid, pass;
  if (loadCredentials(ssid, pass))
  {
    s_savedSsid = ssid;
    s_savedPass = pass;
    s_hasCredentials = true;
    Serial.println(F("[NET] Saved credentials found."));
  }
  else
  {
    Serial.println(F("[NET] No saved credentials found."));
  }

  // Start initial 20-second STA boot auto-connect window
  enterBootStaMode();

  s_payloadPool = (UploadPayload *)ps_malloc(sizeof(UploadPayload) * UPLOAD_QUEUE_DEPTH);
  if (!s_payloadPool)
    s_payloadPool = (UploadPayload *)malloc(sizeof(UploadPayload) * UPLOAD_QUEUE_DEPTH);
  if (!s_payloadPool)
  {
    Serial.println(F("[NET] FATAL: UploadPayload PSRAM alloc failed!"));
    return;
  }

  s_uploadQueue = xQueueCreate(UPLOAD_QUEUE_DEPTH, sizeof(UploadPayload *));
  s_freePayloadQueue = xQueueCreate(UPLOAD_QUEUE_DEPTH, sizeof(UploadPayload *));
  if (!s_uploadQueue || !s_freePayloadQueue)
  {
    Serial.println(F("[NET] FATAL: upload queue creation failed!"));
    return;
  }

  // Populate free payload pointer pool from PSRAM array
  for (int i = 0; i < UPLOAD_QUEUE_DEPTH; i++)
  {
    UploadPayload *p = &s_payloadPool[i];
    xQueueSend(s_freePayloadQueue, &p, 0);
  }

  s_wsMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(wsTaskWorker, "ws_loop_task", 8192, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(uploadTask, "ecg_upload", 16384, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(localStatusTask, "local_status", 4096, nullptr, 1, nullptr, 0);
  Serial.println(F("[NET] Persistent WebSocket + HTTP upload + Local Status tasks started on Core 0."));
}

// -----------------------------------------------------------
// network_update() — call every loop() iteration (non-blocking)
// -----------------------------------------------------------
void network_update()
{
  ble_update();
  s_server.handleClient();

  uint32_t now = millis();
  bool currentlyWifiConnected = (WiFi.status() == WL_CONNECTED);
  bool bleConnected = ble_isConnected();
  static bool s_lastBleConnectedState = false;

  // BLE connection state tracking — LED & notifications ONLY.
  // Never resets Wi-Fi radio modes or state machine!
  if (bleConnected != s_lastBleConnectedState)
  {
    s_lastBleConnectedState = bleConnected;
    ledStatus_setBleConnected(bleConnected);
    if (bleConnected)
    {
      Serial.println(F("[NET] BLE Client connected! Dual Wi-Fi + BLE active."));
    }
    else
    {
      Serial.println(F("[NET] BLE Client disconnected. Wi-Fi background stream active."));
    }
  }

  // Refresh LED status continuously based on real-time WiFi/BLE state
  ledStatus_update();

  // Wi-Fi State machine logic — runs independently of BLE
  switch (s_netState)
  {
  case NET_STATE_STA_BOOT:
  {
    if (currentlyWifiConnected)
    {
      Serial.println(F("[NET] STA connected during boot phase!"));
      enterWifiConnectedMode();
    }
    else if (now - s_bootConnectStartMs >= 20000)
    { // 20s timeout
      Serial.println(F("[NET] 20s boot STA timeout expired. Falling back to "
                       "Dual AP+STA Provisioning..."));
      enterProvisioningDualMode();
    }
    break;
  }

  case NET_STATE_PROVISIONING_DUAL:
  {
    if (currentlyWifiConnected)
    {
      Serial.println(F("[NET] STA connected during dual provisioning!"));
      enterWifiConnectedMode();
    }
    else
    {
      if (s_staRetryAllowed)
      {
        if (now - s_dualModeStartMs < 30000)
        {
          // Retry saved network every 10s during the 30s search window
          if (s_hasCredentials && (now - s_lastReconnectMs >= 10000))
          {
            s_lastReconnectMs = now;
            s_connectStartMs = now;
            wl_status_t st = WiFi.status();
            if (st != WL_CONNECTED && st != WL_IDLE_STATUS)
            {
              uint32_t rem = (30000 - (now - s_dualModeStartMs)) / 1000;
              Serial.printf(
                  "[NET] Dual mode: reconnect attempt to '%s' (%lus remaining)...\n",
                  s_savedSsid.c_str(), (unsigned long)rem);
              WiFi.begin(s_savedSsid.c_str(), s_savedPass.c_str());
            }
          }
        }
        else
        {
          // 30s window expired — STOP STA retries completely and switch to rock-solid pure SoftAP
          s_staRetryAllowed = false;
          Serial.println(F("[NET] 30s saved network search expired. Stopping STA scan, locking pure AP 'ECG_Setup' for new mobile connection..."));
          WiFi.disconnect(true, false);
          WiFi.softAPdisconnect(true);
          delay(50);
          WiFi.mode(WIFI_AP);
          WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
          s_apActive = true;
          ble_startAdvertising();
          ledOff();
        }
      }
    }
    break;
  }

  case NET_STATE_WIFI_CONNECTED:
  {
    if (!currentlyWifiConnected)
    {
      Serial.println(F("[NET] WiFi STA connection dropped! Reconnecting..."));
      s_staConnected = false;
      ledStatus_setWifiConnected(false);
      closeHttpConnection(); // kill persistent TLS socket — must re-handshake after reconnect
      enterProvisioningDualMode();
      break;
    }
    break;
  }

  default:
    break;
  }
}

bool network_isConnected()
{
  return s_staConnected || (WiFi.status() == WL_CONNECTED);
}

// -----------------------------------------------------------
// -----------------------------------------------------------
// -----------------------------------------------------------
// ==========================================================
// WORKFLOW EXPLANATION: DATA TRAVEL PIPELINE - STEP 4 (Network Queueing)
// How data travels:
// 1. This function is called by the DSP task. It's designed to be extremely fast and NON-BLOCKING.
// 2. Instead of waiting for a slow WiFi connection to send the data, it takes the processed
//    block (blk) and puts it into a FreeRTOS queue (s_uploadQueue).
// 3. This allows the DSP task to immediately go back to processing the next block without dropping samples.
// To change main things: The queue depth is UPLOAD_QUEUE_DEPTH (check config.h or network.h).
// If the network is slow, this queue buffers the data. If it fills up, the oldest data might be dropped.
// ==========================================================
// network_uploadBlock() — non-blocking, called from loop()
// -----------------------------------------------------------
bool network_uploadBlock(const Block &blk, bool leadsOff, bool loPlus,
                         bool loMinus, const char *warning,
                         const char *severity, const int32_t *dspOnlyData,
                         const PerformanceMetrics *metrics,
                         const char *deviceResult)
{
  if (!s_uploadQueue || !s_freePayloadQueue)
    return false;

  UploadPayload *p = nullptr;
  if (xQueueReceive(s_freePayloadQueue, &p, 0) != pdTRUE)
  {
    // If free pool empty, drop/reuse oldest pending item from queue
    if (xQueueReceive(s_uploadQueue, &p, 0) != pdTRUE)
      return false;
  }

  // 1. Column 0 (Raw in DB): Raw 24-bit ADC samples from ADS1292R
  memcpy(p->raw_data, blk.data, sizeof(int32_t) * WINDOW_SIZE);

  // 2. Column 1 (Filtered in DB): Filtered ECG data
#if RAW_DATA_ONLY
  memcpy(p->filtered_data, blk.data, sizeof(int32_t) * WINDOW_SIZE);
#else
  memcpy(p->filtered_data, blk.filtered_data, sizeof(int32_t) * WINDOW_SIZE);
#endif

  p->seq = blk.seq;
  p->leadsOff = leadsOff;
  p->loPlus = loPlus;
  p->loMinus = loMinus;
  strcpy(p->mode, "wifi");

  if (warning != nullptr)
  {
    strncpy(p->warning, warning, sizeof(p->warning) - 1);
    p->warning[sizeof(p->warning) - 1] = '\0';
  }
  else
  {
    p->warning[0] = '\0';
  }

  if (severity != nullptr)
  {
    strncpy(p->severity, severity, sizeof(p->severity) - 1);
    p->severity[sizeof(p->severity) - 1] = '\0';
  }
  else
  {
    p->severity[0] = '\0';
  }

  if (deviceResult != nullptr)
  {
    strncpy(p->deviceResult, deviceResult, sizeof(p->deviceResult) - 1);
    p->deviceResult[sizeof(p->deviceResult) - 1] = '\0';
  }
  else
  {
    p->deviceResult[0] = '\0';
  }

  if (metrics != nullptr)
  {
    p->metrics = *metrics;
    p->hasMetrics = true;
  }
  else
  {
    p->hasMetrics = false;
  }

  UBaseType_t queuedBefore = uxQueueMessagesWaiting(s_uploadQueue);
  Serial.printf("[NET QUEUE] Enqueue block seq=%u | Mode=%s | LO=%s | Queue "
                "Depth Before: %u/%d\n",
                blk.seq, p->mode, leadsOff ? "TRUE" : "FALSE",
                (unsigned)queuedBefore, UPLOAD_QUEUE_DEPTH);

  xQueueSend(s_uploadQueue, &p, 0);

  UBaseType_t queuedAfter = uxQueueMessagesWaiting(s_uploadQueue);
  Serial.printf("[NET QUEUE] Enqueued block seq=%u successfully -> Queue "
                "count: %u/%d\n",
                blk.seq, (unsigned)queuedAfter, UPLOAD_QUEUE_DEPTH);
  return true;
}

// -----------------------------------------------------------
// network_sendThermalShutdownAlert()
// Synchronously transmit emergency thermal shutdown alert payload to backend
// -----------------------------------------------------------
void network_sendThermalShutdownAlert(float tempC)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println(F("[NET] Wi-Fi not connected. Skipping emergency thermal "
                     "shutdown API post."));
    return;
  }

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure(); // Skip TLS verification for fast emergency payload

  String url = String("https://") + API_HOST + API_ENDPOINT;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000); // 3 seconds timeout

  String json = "{";
  json += "\"userId\":\"" + String(s_userId) + "\",";
  json += "\"deviceId\":\"" + String(s_deviceId) + "\",";
  json += "\"seq\":" + String(999999) + ",";
  json += "\"sr\":" + String(SAMPLE_RATE) + ",";
  json += "\"lo\":true,";
  json += "\"dieTempC\":" + String(tempC, 1) + ",";
  json += "\"thermalShutdown\":true,";
  json += "\"mode\":\"wifi\",";
  json += "\"warnings\":[\"THERMAL_SHUTDOWN\"],";
  json += "\"severity\":\"CRITICAL\",";
  json += "\"data\":[0],";
  json += "\"filtered_data\":[0]";
  json += "}";

  Serial.printf(
      "[NET] Sending Emergency Thermal Shutdown Alert Payload to %s...\n",
      url.c_str());
  int httpCode = http.POST(json);
  Serial.printf("[NET] Emergency Alert POST Response Code: %d\n", httpCode);
  http.end();
}

// -----------------------------------------------------------
// syncUserIdFromBackend() — Called every 15s in Core 0
// -----------------------------------------------------------
static unsigned long s_lastUserSyncMs = 0;

static void syncUserIdFromBackend()
{
  if (strlen(s_deviceId) == 0)
    return;

  String url = String("https://") + API_HOST + "/api/wifi-config/device/" +
               String(s_deviceId);

  WiFiClientSecure syncClient;
  syncClient.setInsecure();

  HTTPClient http;
  http.begin(syncClient, url);
  int code = http.GET();
  if (code == 200 || code == 201)
  {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload))
    {
      const char *uid = doc["userId"];
      if (uid && strlen(uid) > 0 && strcmp(uid, s_userId) != 0)
      {
        strncpy(s_userId, uid, MAX_DEVICE_ID_LEN - 1);
        s_userId[MAX_DEVICE_ID_LEN - 1] = '\0';

        s_prefs.begin(NVS_NAMESPACE, false);
        s_prefs.putString("user_id", s_userId);
        s_prefs.end();
        Serial.print(F("[NET] Synced userId from backend: "));
        Serial.println(s_userId);
      }
    }
  }
  http.end();
}

// -----------------------------------------------------------
// uploadTask() — Core 0
// -----------------------------------------------------------

// -----------------------------------------------------------
// localStatusTask() — Dedicated Local Heartbeat & Device Status Task (Core 0)
// -----------------------------------------------------------
#define LOCAL_SERVER_PORT 9000
#define LOCAL_HEARTBEAT_INTERVAL_MS 5000

static void localStatusTask(void *pv)
{
  unsigned long lastHeartbeat = 0;
  bool wasConnected = false;

  for (;;)
  {
    bool isConnected = (WiFi.status() == WL_CONNECTED);

    if (isConnected != wasConnected)
    {
      wasConnected = isConnected;
      Serial.println("[LocalStatusTask] Network state transition: " +
                     String(isConnected ? "CONNECTED" : "DISCONNECTED"));

      IPAddress gw = WiFi.gatewayIP();
      if (gw != (uint32_t)0 && gw != IPAddress(192, 168, 4, 1))
      {
        String url = String("http://") + gw.toString() + ":" +
                     String(LOCAL_SERVER_PORT) + "/device-status?connected=" +
                     (isConnected ? "true" : "false") +
                     "&deviceId=" + String(s_deviceId);
        HTTPClient httpLocal;
        httpLocal.setTimeout(1500);
        if (httpLocal.begin(url))
        {
          int code = httpLocal.GET();
          Serial.println("[LocalStatusTask] device-status notified code=" +
                         String(code));
          httpLocal.end();
        }
      }
    }

    // Heartbeat
    if (isConnected)
    {
      unsigned long now = millis();
      if (now - lastHeartbeat >= LOCAL_HEARTBEAT_INTERVAL_MS)
      {
        lastHeartbeat = now;
        IPAddress gw = WiFi.gatewayIP();
        if (gw != (uint32_t)0 && gw != IPAddress(192, 168, 4, 1))
        {
          String url = String("http://") + gw.toString() + ":" +
                       String(LOCAL_SERVER_PORT) +
                       "/heartbeat?deviceId=" + String(s_deviceId);
          HTTPClient httpLocal;
          httpLocal.setTimeout(1500);
          if (httpLocal.begin(url))
          {
            int code = httpLocal.GET();
            Serial.printf("[LocalStatusTask] Heartbeat to %s -> code=%d\n",
                          url.c_str(), code);
            httpLocal.end();
          }
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// -----------------------------------------------------------
// WebSocket Client Event Handler & Dedicated Background Loop Task
// -----------------------------------------------------------
static void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
  Serial.printf("[WS DEBUG] Event Type: %d, length: %u\n", type, length);
  switch (type)
  {
  case WStype_DISCONNECTED:
    s_wsConnected = false;
    if (millis() - s_lastWsLogMs >= 5000)
    {
      s_lastWsLogMs = millis();
      Serial.println(F("[WS] Disconnected from WebSocket server. HTTP POST "
                       "fallback active..."));
    }
    if (payload && length > 0) {
      Serial.printf("[WS] Disconnect payload: %s\n", (const char *)payload);
    }
    break;
  case WStype_CONNECTED:
    s_wsConnected = true;
    s_lastWsConnectedMs = millis();
    Serial.printf("[WS] Connected to WebSocket endpoint at wss://%s/ws!\n",
                  API_HOST);
    if (payload && length > 0) {
      Serial.printf("[WS] Connect payload/url: %s\n", (const char *)payload);
    }
    break;
  case WStype_TEXT:
    Serial.printf("[WS] Server msg (TEXT): %s\n", (const char *)payload);
    break;
  case WStype_BIN:
    Serial.printf("[WS] Server msg (BIN), length: %u\n", length);
    break;
  case WStype_ERROR:
    s_wsConnected = false;
    Serial.printf("[WS] ERROR! ");
    if (payload && length > 0) {
      Serial.printf("Details: %s\n", (const char *)payload);
    } else {
      Serial.println("No error details provided.");
    }
    break;
  case WStype_PING:
    Serial.printf("[WS] PING received, length: %u\n", length);
    break;
  case WStype_PONG:
    Serial.printf("[WS] PONG received, length: %u\n", length);
    break;
  default:
    Serial.printf("[WS] Unhandled event type: %d\n", type);
    break;
  }
}

static void wsTaskWorker(void *pv)
{
  uint32_t lastInitMs = 0;
  bool wasWifiConnected = false;

  for (;;)
  {
    bool wifiOk = (s_staConnected || WiFi.status() == WL_CONNECTED);

    if (wifiOk)
    {
      // If Wi-Fi just came UP or WebSocket is disconnected for > 3 seconds, attempt fast reconnect
      if (!wasWifiConnected || (!s_wsConnected && (millis() - lastInitMs >= 3000)))
      {
        lastInitMs = millis();
        if (s_wsMutex && xSemaphoreTake(s_wsMutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
          s_webSocket.disconnect();
          s_webSocket.beginSSL(API_HOST, 443, "/ws", "", "");
          s_webSocket.onEvent(webSocketEvent);
          s_webSocket.setReconnectInterval(2000);
          s_webSocket.enableHeartbeat(10000, 3000, 2); // 10s ping keep-alive to prevent Render TCP timeouts
          xSemaphoreGive(s_wsMutex);
          Serial.printf("[WS-Client] Fast reconnect wss://%s/ws...\n", API_HOST);
        }
      }
      wasWifiConnected = true;

      if (s_wsMutex && xSemaphoreTake(s_wsMutex, pdMS_TO_TICKS(10)) == pdTRUE)
      {
        s_webSocket.loop();
        xSemaphoreGive(s_wsMutex);
      }
    }
    else
    {
      if (wasWifiConnected)
      {
        wasWifiConnected = false;
        s_wsConnected = false;
        if (s_wsMutex && xSemaphoreTake(s_wsMutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
          s_webSocket.disconnect();
          xSemaphoreGive(s_wsMutex);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(1)); // 1ms delay for 5x faster TCP ACK/packet processing
  }
}

// ==========================================================
// WORKFLOW EXPLANATION: DATA TRAVEL PIPELINE - STEP 5 (Actual Internet Transmission)
// How data is sent to the internet:
// 1. This uploadTask runs continuously in the background on Core 0.
// 2. It waits for data to appear in the s_uploadQueue.
// 3. When it gets a block, it formats it into a large JSON string via buildJsonBuffer().
// 4. It then tries to send the JSON via WebSocket (if connected) for fastest streaming.
// 5. If WebSocket is down, it falls back to a raw TLS HTTP POST request.
// To change main things: You can change the JSON format in buildJsonBuffer(), or change
// the API endpoints at the top of the file/in config.h.
// ==========================================================
static void uploadTask(void *pv)
{
  static char *s_jsonBuf = nullptr;
  if (!s_jsonBuf)
  {
    s_jsonBuf = (char *)ps_malloc(JSON_BUF_SIZE);
    if (!s_jsonBuf)
      s_jsonBuf = (char *)malloc(JSON_BUF_SIZE);
  }

  UploadPayload *p = nullptr;

  for (;;)
  {
    if (!s_jsonBuf)
    {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }

    if ((s_staConnected || WiFi.status() == WL_CONNECTED) &&
        (millis() - s_lastUserSyncMs >= 60000))
    {
      s_lastUserSyncMs = millis();
      syncUserIdFromBackend();
    }

    if (xQueueReceive(s_uploadQueue, &p, pdMS_TO_TICKS(10)) == pdTRUE)
    {
      if (s_staConnected || WiFi.status() == WL_CONNECTED)
      {
        size_t len = buildJsonBuffer(*p, s_jsonBuf, JSON_BUF_SIZE);
        if (len > 0)
        {
          bool sentViaWs = false;
          if (s_wsConnected)
          {
            uint32_t t0 = millis();
            if (s_wsMutex &&
                xSemaphoreTake(s_wsMutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
              sentViaWs = s_webSocket.sendTXT((uint8_t *)s_jsonBuf, len);
              xSemaphoreGive(s_wsMutex);
            }
            if (sentViaWs)
            {
              uint32_t elapsed = millis() - t0;
              UBaseType_t remaining =
                  s_uploadQueue ? uxQueueMessagesWaiting(s_uploadQueue) : 0;
              Serial.printf("[WS STREAM] Sent seq=%u (%u ms, %u bytes) | Queue "
                            "remaining: %u/%d\n",
                            (unsigned)p->seq, (unsigned)elapsed, (unsigned)len,
                            (unsigned)remaining, UPLOAD_QUEUE_DEPTH);
              // Yield 10ms after a successful heavy 24KB transmission to allow TCP ACKs to clear smoothly
              vTaskDelay(10 / portTICK_PERIOD_MS);
            }
            else
            {
              Serial.printf("[WS DEBUG] sendTXT failed for seq=%u, length=%u bytes\n",
                            (unsigned)p->seq, (unsigned)len);
              // Yield briefly to let WebSocket worker re-establish TCP socket
              vTaskDelay(50 / portTICK_PERIOD_MS);
            }
          }
          else
          {
            // WebSocket is reconnecting — yield to give wsTaskWorker full Core 0 CPU time
            vTaskDelay(50 / portTICK_PERIOD_MS);
          }
        }
      }
      else
      {
        static uint32_t lastNoWifiLog = 0;
        if (millis() - lastNoWifiLog >= 5000)
        {
          lastNoWifiLog = millis();
          Serial.println(F("[NET] Wi-Fi STA disconnected. Dropping block to "
                           "keep stream real-time."));
        }
      }

      // Return payload pointer to free pool so queue never stalls
      xQueueSend(s_freePayloadQueue, &p, 0);
    }
    else
    {
      vTaskDelay(1 / portTICK_PERIOD_MS);
    }
  }
}

// -----------------------------------------------------------
// fast_itoa() — Ultra-fast integer to ASCII string converter
// Converts int32_t to char* in 1.5 microseconds (no format string parsing)
// -----------------------------------------------------------
static inline int fast_itoa(int32_t val, char *buf)
{
  if (val == 0)
  {
    buf[0] = '0';
    return 1;
  }
  int len = 0;
  uint32_t uval;
  if (val < 0)
  {
    buf[len++] = '-';
    uval = (uint32_t)(-val);
  }
  else
  {
    uval = (uint32_t)val;
  }
  char temp[12];
  int tpos = 0;
  while (uval > 0)
  {
    temp[tpos++] = '0' + (uval % 10);
    uval /= 10;
  }
  while (tpos > 0)
  {
    buf[len++] = temp[--tpos];
  }
  return len;
}

// -----------------------------------------------------------
// buildJsonBuffer()
// Formats raw 24-bit "data" array into PSRAM char buffer.
// -----------------------------------------------------------
static size_t buildJsonBuffer(const UploadPayload &p, char *buf,
                              size_t bufSize)
{
  int pos = 0;

  // Header fields matching standard JSON schema
  pos +=
      snprintf(buf + pos, bufSize - pos,
               "{\"userId\":\"%s\",\"deviceId\":\"%s\",\"mode\":\"%s\","
               "\"seq\":%u,\"sr\":%d,\"lo\":%s,\"loPlus\":%s,\"loMinus\":%s,",
               s_userId, s_deviceId, p.mode, (unsigned)p.seq, SAMPLE_RATE,
               p.leadsOff ? "true" : "false", p.loPlus ? "true" : "false",
               p.loMinus ? "true" : "false");

  if (p.severity[0] != '\0')
  {
    pos +=
        snprintf(buf + pos, bufSize - pos, "\"severity\":\"%s\",", p.severity);
  }

  if (p.deviceResult[0] != '\0')
  {
    pos += snprintf(buf + pos, bufSize - pos, "\"device_result\":\"%s\",",
                    p.deviceResult);
  }

  if (p.hasMetrics)
  {
    pos += snprintf(buf + pos, bufSize - pos,
                    "\"metrics\":{"
                    "\"snrDb\":%.2f,"
                    "\"snrAccuracy\":%.1f,"
                    "\"rPeakAccuracy\":%.1f,"
                    "\"hrBpm\":%.1f,"
                    "\"hrAccuracy\":%.1f,"
                    "\"baselineWanderMv\":%.2f,"
                    "\"baselineAccuracy\":%.1f,"
                    "\"motionArtifactIndex\":%.2f,"
                    "\"motionAccuracy\":%.1f,"
                    "\"cmrrEstDb\":%.1f"
                    "},",
                    p.metrics.snrDb, p.metrics.snrAccuracy,
                    p.metrics.rPeakAccuracy, p.metrics.hrBpm,
                    p.metrics.hrAccuracy, p.metrics.baselineWanderMv,
                    p.metrics.baselineAccuracy, p.metrics.motionArtifactIndex,
                    p.metrics.motionAccuracy, p.metrics.cmrrEstDb);
  }

  // Output 2D Array of sample pairs: "data": [[raw0, filtered0], [raw1,
  // filtered1], ...]
  const int32_t RAW_MAX = 8000000;
  const int32_t RAW_MIN = -8000000;
  pos += snprintf(buf + pos, bufSize - pos, "\"data\":[");
  for (int i = 0; i < WINDOW_SIZE && pos < (int)(bufSize - 35); i++)
  {
    int32_t raw = p.raw_data[i];
    int32_t filt = p.filtered_data[i];
    // Clamp to valid ADC range
    if (raw > RAW_MAX)
      raw = RAW_MAX;
    if (raw < RAW_MIN)
      raw = RAW_MIN;
    // Replace true saturated rail spikes (> 7.5M counts)
    if (raw < -7500000 || raw > 7500000)
    {
      int32_t left = (i > 0) ? p.raw_data[i - 1] : 0;
      int32_t right = (i < WINDOW_SIZE - 1) ? p.raw_data[i + 1] : left;
      if (left < -7500000 || left > 7500000)
        left = 0;
      if (right < -7500000 || right > 7500000)
        right = left;
      raw = (left + right) / 2;
    }
    buf[pos++] = '[';
    pos += fast_itoa(raw, buf + pos);
    buf[pos++] = ',';
    pos += fast_itoa(filt, buf + pos);
    buf[pos++] = ']';
    if (i < WINDOW_SIZE - 1)
    {
      buf[pos++] = ',';
    }
  }
  buf[pos++] = ']';
  buf[pos++] = '}';
  buf[pos] = '\0';

  return (pos > 0 && pos < (int)bufSize) ? (size_t)pos : 0;
}

// -----------------------------------------------------------
// Raw persistent TLS socket POST
// Connects once, reuses socket for subsequent blocks.
// Falls back to fresh connect if server closed the connection.
// -----------------------------------------------------------
static WiFiClientSecure s_tlsClient;
static bool s_httpConnected = false;
static uint32_t s_lastConnectMs = 0;
static uint32_t s_lastTlsFailMs = 0;

static void closeHttpConnection()
{
  if (s_httpConnected)
  {
    s_tlsClient.stop();
    s_httpConnected = false;
    Serial.println(F("[NET] TLS socket closed."));
  }
}

static bool openHttpConnection()
{
  // Enforce at least 3s backoff after a failed TLS connect to prevent thrashing
  if (s_lastTlsFailMs > 0 && (millis() - s_lastTlsFailMs < 3000))
  {
    return false;
  }

  closeHttpConnection();
  s_tlsClient.setInsecure();
  s_tlsClient.setNoDelay(true); // Disable Nagle's algorithm for zero packet delay
  s_tlsClient.setTimeout(4);     // 4 seconds max timeout

  Serial.printf("[NET] Connecting to %s:443...\n", API_HOST);
  Serial.printf("Internal free: %u (largest block %u), PSRAM free: %u\n",
                (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (uint32_t)ESP.getFreePsram());

  if (!s_tlsClient.connect(API_HOST, 443))
  {
    char errBuf[128];
    int errCode = s_tlsClient.lastError(errBuf, sizeof(errBuf));
    Serial.printf("[NET ERROR] TLS connect failed! Error code: %d - %s\n", errCode, errBuf);
    s_lastTlsFailMs = millis();
    return false;
  }
  s_httpConnected = true;
  s_lastConnectMs = millis();
  s_lastTlsFailMs = 0;
  Serial.println(F("[NET] TLS socket open."));
  return true;
}

static bool postPayload(const char *buf, size_t len)
{
  if (!buf || len == 0)
    return false;

  for (int attempt = 0; attempt < 2; attempt++)
  {
    // Connect if not connected or after a retry
    if (!s_httpConnected || !s_tlsClient.connected())
    {
      if (!openHttpConnection())
        return false;
    }

    uint32_t t0 = millis();

    // Write raw HTTP/1.1 POST request header
    size_t headerLen =
        s_tlsClient.printf("POST %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %u\r\n"
                           "Connection: keep-alive\r\n"
                           "\r\n",
                           API_ENDPOINT, API_HOST, (unsigned)len);

    if (headerLen == 0)
    {
      closeHttpConnection();
      if (attempt == 0)
        continue; // retry with fresh socket
      return false;
    }

    // Write payload in 16KB TLS chunks
    size_t sent = 0;
    bool writeError = false;
    while (sent < len)
    {
      size_t chunkSize = (len - sent > 16384) ? 16384 : (len - sent);
      size_t w = s_tlsClient.write((const uint8_t *)(buf + sent), chunkSize);
      if (w == 0)
      {
        writeError = true;
        break;
      }
      sent += w;
    }
    s_tlsClient.flush();

    if (writeError || sent < len)
    {
      closeHttpConnection();
      if (attempt == 0)
      {
        Serial.println(F("[NET] Stale socket write failed. Retrying with fresh "
                         "TLS connection..."));
        continue; // Retry with fresh TLS socket
      }
      Serial.printf("[NET ERROR] TLS write failed at %u/%u bytes.\n",
                    (unsigned)sent, (unsigned)len);
      return false;
    }

    // Read HTTP response status line (max 2000ms deadline)
    uint32_t deadline = millis() + 2000;
    String statusLine = "";
    while (millis() < deadline)
    {
      if (s_tlsClient.available())
      {
        statusLine = s_tlsClient.readStringUntil('\n');
        break;
      }
      vTaskDelay(2 / portTICK_PERIOD_MS);
    }

    // Drain remaining response headers
    bool serverClosedConn = false;
    while (millis() < deadline)
    {
      if (!s_tlsClient.available())
      {
        vTaskDelay(2 / portTICK_PERIOD_MS);
        if (!s_tlsClient.connected())
          break;
        continue;
      }
      String hdr = s_tlsClient.readStringUntil('\n');
      hdr.trim();
      if (hdr.length() == 0)
        break;
      if (hdr.startsWith("Connection: close") ||
          hdr.startsWith("connection: close"))
      {
        serverClosedConn = true;
      }
    }

    uint32_t t1 = millis();
    UBaseType_t remaining =
        s_uploadQueue ? uxQueueMessagesWaiting(s_uploadQueue) : 0;

    int httpCode = -1;
    if (statusLine.startsWith("HTTP/"))
    {
      int sp1 = statusLine.indexOf(' ');
      if (sp1 > 0)
      {
        httpCode = statusLine.substring(sp1 + 1, sp1 + 4).toInt();
      }
    }

    bool success = (httpCode >= 200 && httpCode < 300);
    if (success)
    {
      Serial.printf("[NET] POST OK (HTTP %d, %u ms) | Queue remaining: %u/%d\n",
                    httpCode, t1 - t0, (unsigned)remaining, UPLOAD_QUEUE_DEPTH);
      if (serverClosedConn)
      {
        closeHttpConnection();
      }
      return true;
    }

    Serial.printf(
        "[NET ERROR] POST failed (HTTP %d, %u ms) | Queue remaining: %u/%d\n",
        httpCode, t1 - t0, (unsigned)remaining, UPLOAD_QUEUE_DEPTH);
    closeHttpConnection();

    if (attempt == 0)
    {
      Serial.println(
          F("[NET] POST failed. Retrying with fresh TLS connection..."));
      continue; // Retry with fresh TLS socket!
    }
    return false;
  }
  return false;
}

// -----------------------------------------------------------
// Exposed payload builder for BLE reuse
// -----------------------------------------------------------
void network_buildJsonPayload(const Block &blk, bool leadsOff, bool loPlus,
                              bool loMinus, const char *warning,
                              const char *severity, const char *mode,
                              String &out)
{
  UploadPayload *p = (UploadPayload *)ps_malloc(sizeof(UploadPayload));
  if (!p)
    p = new UploadPayload();
  if (!p)
  {
    out = "";
    return;
  }

  memcpy(p->raw_data, blk.data, sizeof(int32_t) * WINDOW_SIZE);
#if RAW_DATA_ONLY
  memcpy(p->filtered_data, blk.data, sizeof(int32_t) * WINDOW_SIZE);
#else
  memcpy(p->filtered_data, blk.filtered_data, sizeof(int32_t) * WINDOW_SIZE);
#endif
  p->seq = blk.seq;
  p->leadsOff = leadsOff;
  p->loPlus = loPlus;
  p->loMinus = loMinus;

  if (mode != nullptr)
  {
    strncpy(p->mode, mode, sizeof(p->mode) - 1);
    p->mode[sizeof(p->mode) - 1] = '\0';
  }
  else
  {
    strcpy(p->mode, "wifi");
  }

  if (warning != nullptr)
  {
    strncpy(p->warning, warning, sizeof(p->warning) - 1);
    p->warning[sizeof(p->warning) - 1] = '\0';
  }
  else
  {
    p->warning[0] = '\0';
  }

  if (severity != nullptr)
  {
    strncpy(p->severity, severity, sizeof(p->severity) - 1);
    p->severity[sizeof(p->severity) - 1] = '\0';
  }
  else
  {
    p->severity[0] = '\0';
  }

  char *buf = (char *)ps_malloc(JSON_BUF_SIZE);
  if (!buf)
    buf = (char *)malloc(JSON_BUF_SIZE);
  if (buf)
  {
    buildJsonBuffer(*p, buf, JSON_BUF_SIZE);
    out = String(buf);
    free(buf);
  }
  else
  {
    out = "";
  }
  free(p);
}
