/**
 * ble_transport.cpp
 * -----------------------------------------------------------
 * BLE Transport — Raw Binary ECG Data + JSON Status Push (NimBLE)
 *
 * TWO characteristics on the same service:
 *  ① ECG Data Characteristic  (NOTIFY | READ)
 *     UUID: 12345678-1234-5678-1234-56789abcdef1
 *     508-byte raw binary packet per block (125 int32 samples).
 *
 *  ② Status Characteristic  (NOTIFY | READ | WRITE | WRITE_NR)
 *     UUID: 12345678-1234-5678-1234-56789abcdef2
 *     JSON status string updates + WiFi credentials provisioning.
 * -----------------------------------------------------------
 */

#include "ble_transport.h"
#include "network.h"
#include "config.h"
#include "buzzer.h"
#include "led_status.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>

#define BLE_DEVICE_NAME     "ECG_Setup"
#define SERVICE_UUID        "12345678-1234-5678-1234-56789abcdef0"
#define ECG_CHAR_UUID       "12345678-1234-5678-1234-56789abcdef1" // Raw ECG binary
#define STATUS_CHAR_UUID    "12345678-1234-5678-1234-56789abcdef2" // JSON status push

#define SEV_INFO     0
#define SEV_WARNING  1
#define SEV_CRITICAL 2

#define MAX_RESULT_STR_LEN 48

struct __attribute__((packed)) BleMetrics {
    float snrDb;
    float snrAccuracy;
    float rPeakAccuracy;
    float hrBpm;
    float hrAccuracy;
    float baselineWanderMv;
    float baselineAccuracy;
    float motionArtifactIndex;
    float motionAccuracy;
    float cmrrEstDb;
};

struct __attribute__((packed)) BleRawPacket {
    uint32_t   seq;                              // 4 bytes [0..3]
    uint16_t   sampleRate;                       // 2 bytes [4..5] (2000 Hz)
    uint8_t    flags;                            // 1 byte  [6] (bit0=lo, bit1=loPlus, bit2=loMinus)
    uint8_t    severity;                         // 1 byte  [7] (0=INFO, 1=WARNING, 2=CRITICAL)
    uint16_t   numSamples;                       // 2 bytes [8..9] (2000 samples)
    char       deviceResult[MAX_RESULT_STR_LEN]; // 48 bytes [10..57]
    BleMetrics metrics;                          // 40 bytes [58..97]
    int32_t    raw_samples[WINDOW_SIZE];         // Raw ADC samples (8000 bytes)
    int32_t    filtered_samples[WINDOW_SIZE];    // DSP filtered ECG samples (8000 bytes)
};
static_assert(sizeof(BleRawPacket) == 10 + MAX_RESULT_STR_LEN + sizeof(BleMetrics) + WINDOW_SIZE * 8, "BleRawPacket size mismatch");

static uint8_t parseSeverity(const char* s) {
    if (!s) return SEV_INFO;
    if (strcmp(s, "CRITICAL") == 0) return SEV_CRITICAL;
    if (strcmp(s, "WARNING")  == 0) return SEV_WARNING;
    return SEV_INFO;
}

static NimBLEServer*         pServer         = nullptr;
static NimBLEService*        pService        = nullptr;
static NimBLECharacteristic* pEcgChar       = nullptr;
static NimBLECharacteristic* pStatusChar    = nullptr;

static bool     s_bleConnected    = false;
static uint16_t s_bleMtu          = 23;
static bool     s_needReadvertise = false;

class BleServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pSrv, ble_gap_conn_desc* desc) override {
        s_bleConnected = true;
        s_bleMtu = pSrv->getPeerMTU(desc->conn_handle);
        pSrv->updateConnParams(desc->conn_handle, 12, 24, 0, 600); // 15-30ms interval, 6s supervision timeout to prevent disconnects
        NimBLEAddress peerAddr(desc->peer_ota_addr);
        String macStr = peerAddr.toString().c_str();
        macStr.toUpperCase();

        Serial.printf("[BLE] Client connected! MAC: %s, MTU: %u\n", macStr.c_str(), s_bleMtu);

        // Turn ON Orange Connectivity LED & sound BLE connect buzzer tone
        ledStatus_setBleConnected(true);
        buzzer_bleConnected();

        // Push a "ble_connected" status immediately so the app knows the link is up.
        String status = "{\"status\":\"ble_connected\",\"mac\":\"" + macStr + "\"}";
        ble_sendStatus(status);
    }

    void onDisconnect(NimBLEServer* pSrv) override {
        s_bleConnected = false;
        s_bleMtu = 23;
        Serial.println(F("[BLE] Client disconnected."));

        // Update Orange Connectivity LED & sound BLE disconnect buzzer tone
        ledStatus_setBleConnected(false);
        buzzer_bleDisconnected();

        s_needReadvertise = true;
    }

    void onMTUChange(uint16_t mtu, ble_gap_conn_desc* desc) override {
        s_bleMtu = mtu;
        Serial.printf("[BLE] MTU negotiated: %u bytes | ECG pkt %u bytes\n", mtu, (unsigned)sizeof(BleRawPacket));
    }
};

class BleCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar) override {
        std::string val = pChar->getValue();
        if (val.length() == 0) return;
        
        Serial.printf("[BLE] Received write (%d bytes): %s\n", (int)val.length(), val.c_str());
        
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, val);
        if (!err) {
            String ssid = doc["ssid"] | "";
            String pass = doc["password"] | "";
            String did  = doc["deviceId"] | "";
            String uid  = doc["userId"] | "";
            
            if (ssid.length() > 0) {
                Serial.printf("[BLE] WiFi credentials received over BLE: SSID=%s\n", ssid.c_str());
                network_saveCredentialsAndConnect(ssid, pass, did, uid);
                
                String resp = "{\"status\":\"ok\",\"message\":\"WiFi credentials saved via BLE\",\"ssid\":\"" + ssid + "\"}";
                ble_sendStatus(resp);
            }
        }
    }
};

bool ble_isConnected() { return s_bleConnected; }
uint16_t ble_getMtu()   { return s_bleMtu; }

void ble_init() {
    Serial.println(F("[BLE] Initializing NimBLE stack..."));

    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setMTU(517);

    // Disable forced bonding requirement so standard GATT connections succeed seamlessly
    NimBLEDevice::setSecurityAuth(false, false, false);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new BleServerCallbacks());

    pService = pServer->createService(SERVICE_UUID);

    // Characteristic 1: Raw ECG Binary (NOTIFY | READ)
    pEcgChar = pService->createCharacteristic(
        ECG_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
    );

    // Characteristic 2: Status JSON Push (NOTIFY | READ | WRITE | WRITE_NR)
    pStatusChar = pService->createCharacteristic(
        STATUS_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pStatusChar->setCallbacks(new BleCharacteristicCallbacks());

    pService->start();

    ble_startAdvertising();
    Serial.println(F("[BLE] Fast advertising active as 'ECG_Setup'. Waiting for connections..."));
}

void ble_startAdvertising() {
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->stop();

    pAdv->addServiceUUID(SERVICE_UUID);
    pAdv->setName(BLE_DEVICE_NAME);
    pAdv->setScanResponse(true);
    pAdv->setMinInterval(0x20); // 20 ms fast advertising interval
    pAdv->setMaxInterval(0x40); // 40 ms

    NimBLEDevice::startAdvertising();
    Serial.println(F("[BLE] General advertising started as 'ECG_Setup'."));
}

void ble_stopAdvertising() {
    NimBLEDevice::stopAdvertising();
}

void ble_update() {
    if (s_needReadvertise && !s_bleConnected) {
        s_needReadvertise = false;
        Serial.println(F("[BLE] Deferred restart of advertising..."));
        ble_startAdvertising();
    }
}

void ble_sendStatus(const String& json) {
    if (!pStatusChar) return;
    pStatusChar->setValue((uint8_t*)json.c_str(), json.length());
    if (s_bleConnected) {
        pStatusChar->notify();
        Serial.println("[BLE] Status push: " + json);
    }
}

// ==========================================================
// WORKFLOW EXPLANATION: DATA TRAVEL PIPELINE - STEP 5 (BLE Transmission)
// How data is sent to the mobile app:
// 1. Like network_uploadBlock, this function receives the fully processed block.
// 2. Instead of a queue, it packs the raw data, filtered data, and metrics into 
//    a binary packet (BleRawPacket).
// 3. It then pushes this packet to connected BLE clients via pEcgChar->notify().
// 4. If the packet is larger than the negotiated MTU (Maximum Transmission Unit), 
//    it chunks the packet and sends the chunks sequentially.
// To change main things: You can change the BleRawPacket struct definition at the top 
// of this file to add or remove fields sent to the app.
// ==========================================================
void ble_uploadBlock(const Block& blk, bool leadsOff, bool loPlus, bool loMinus,
                     const char* warning, const char* severity,
                     const int32_t* dspData,
                     const PerformanceMetrics* metrics,
                     const char* deviceResult) {
    if (!s_bleConnected || !pEcgChar) return;

    // Allocate BleRawPacket from PSRAM once to prevent task stack overflow (16 KB struct)
    static BleRawPacket* s_pkt = nullptr;
    if (!s_pkt) {
        s_pkt = (BleRawPacket*)ps_malloc(sizeof(BleRawPacket));
        if (!s_pkt) s_pkt = (BleRawPacket*)malloc(sizeof(BleRawPacket));
    }
    if (!s_pkt) return;

    s_pkt->seq = blk.seq;
    s_pkt->sampleRate = SAMPLE_RATE;
    s_pkt->flags = (leadsOff ? 0x01 : 0) | (loPlus ? 0x02 : 0) | (loMinus ? 0x04 : 0);
    s_pkt->severity = parseSeverity(severity);
    s_pkt->numSamples = WINDOW_SIZE;

    // 1. Device Result String (Diagnosis)
    memset(s_pkt->deviceResult, 0, sizeof(s_pkt->deviceResult));
    if (deviceResult != nullptr && strlen(deviceResult) > 0) {
        strncpy(s_pkt->deviceResult, deviceResult, sizeof(s_pkt->deviceResult) - 1);
    } else if (warning != nullptr && strlen(warning) > 0) {
        strncpy(s_pkt->deviceResult, warning, sizeof(s_pkt->deviceResult) - 1);
    }

    // 2. Performance Metrics
    if (metrics != nullptr) {
        s_pkt->metrics.snrDb               = metrics->snrDb;
        s_pkt->metrics.snrAccuracy         = metrics->snrAccuracy;
        s_pkt->metrics.rPeakAccuracy       = metrics->rPeakAccuracy;
        s_pkt->metrics.hrBpm               = metrics->hrBpm;
        s_pkt->metrics.hrAccuracy          = metrics->hrAccuracy;
        s_pkt->metrics.baselineWanderMv    = metrics->baselineWanderMv;
        s_pkt->metrics.baselineAccuracy    = metrics->baselineAccuracy;
        s_pkt->metrics.motionArtifactIndex = metrics->motionArtifactIndex;
        s_pkt->metrics.motionAccuracy      = metrics->motionAccuracy;
        s_pkt->metrics.cmrrEstDb           = metrics->cmrrEstDb;
    } else {
        memset(&s_pkt->metrics, 0, sizeof(BleMetrics));
        s_pkt->metrics.cmrrEstDb = 86.0f;
    }

    // 3. Raw ADC samples
    memcpy(s_pkt->raw_samples, blk.data, sizeof(int32_t) * WINDOW_SIZE);

    // 4. DSP-filtered ECG samples
    if (dspData != nullptr) {
        memcpy(s_pkt->filtered_samples, dspData, sizeof(int32_t) * WINDOW_SIZE);
    } else {
#if RAW_DATA_ONLY
        memcpy(s_pkt->filtered_samples, blk.data, sizeof(int32_t) * WINDOW_SIZE);
#else
        memcpy(s_pkt->filtered_samples, blk.filtered_data, sizeof(int32_t) * WINDOW_SIZE);
#endif
    }

    uint16_t mtu = ble_getMtu();
    size_t payloadSize = sizeof(BleRawPacket);

    // 1. Single-shot notification if negotiated MTU >= payloadSize + 3
    if (mtu >= payloadSize + 3) {
        pEcgChar->notify((uint8_t*)s_pkt, payloadSize, true);
        Serial.printf("[BLE] Sent block seq=%u (%d samples raw+filtered+metrics, MTU=%u, %u bytes)\n", blk.seq, (int)s_pkt->numSamples, mtu, (unsigned)payloadSize);
        return;
    }

    // 2. Chunking Fallback: Transmit payload across MTU notifications with pacing
    size_t maxNotifyPayload = (mtu < 23) ? 20 : mtu - 3;
    if (maxNotifyPayload > 480) {
        maxNotifyPayload = 480; // Safe cap below 512 bytes to guarantee single mbuf allocation in NimBLE
    }
    
    struct __attribute__((packed)) ChunkHeader {
        uint32_t seq;
        uint8_t  chunkIdx;
        uint8_t  totalChunks;
    };
    
    size_t headerSize = sizeof(ChunkHeader);
    if (maxNotifyPayload <= headerSize) return;

    size_t maxDataPerChunk = maxNotifyPayload - headerSize;
    size_t totalChunks = (payloadSize + maxDataPerChunk - 1) / maxDataPerChunk;
    if (totalChunks > 255) return;

    uint8_t* rawBytes = (uint8_t*)s_pkt;

    for (size_t i = 0; i < totalChunks && s_bleConnected; i++) {
        size_t offset = i * maxDataPerChunk;
        size_t len = maxDataPerChunk;
        if (offset + len > payloadSize) {
            len = payloadSize - offset;
        }

        uint8_t buffer[500];
        if (headerSize + len > sizeof(buffer)) return;

        ChunkHeader header;
        header.seq = s_pkt->seq;
        header.chunkIdx = (uint8_t)i;
        header.totalChunks = (uint8_t)totalChunks;

        memcpy(buffer, &header, headerSize);
        memcpy(buffer + headerSize, rawBytes + offset, len);

        pEcgChar->notify((const uint8_t*)buffer, headerSize + len, true);

        // 20ms pacing delay ensures each chunk lands in its own BLE connection event on Android
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    Serial.printf("[BLE] Sent block seq=%u in %u chunks (%d DSP samples, MTU=%u)\n", blk.seq, (unsigned)totalChunks, (int)s_pkt->numSamples, mtu);
}
