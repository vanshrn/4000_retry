/**
 * ble_transport.h
 * -----------------------------------------------------------
 * BLE Transport — Raw Binary ECG + JSON Status Push (NimBLE)
 *
 * TWO characteristics on the same service:
 *
 *  ① ECG Data Characteristic  (NOTIFY + READ)
 *    UUID: 12345678-1234-5678-1234-56789abcdef1
 *    Sends raw binary ECG packets (508 bytes each, 1 notification/block).
 *    Packet layout (little-endian, packed):
 *      [0..3]     uint32_t  seq
 *      [4..5]     uint16_t  sampleRate  (500)
 *      [6]        uint8_t   flags       bit0=lo, bit1=loPlus, bit2=loMinus
 *      [7]        uint8_t   severity    0=INFO, 1=WARNING, 2=CRITICAL
 *      [8..9]     uint16_t  numSamples  (500)
 *      [10..2009] int32_t   samples[500]
 *
 *  ② Status Characteristic  (NOTIFY + READ)
 *    UUID: 12345678-1234-5678-1234-56789abcdef2
 *    Sends plain JSON status strings, e.g.:
 *      {"status":"wifi_connected","ip":"192.168.1.10","deviceId":"ESP_ECG_123"}
 *      {"status":"wifi_failed"}
 *      {"status":"wifi_connecting"}
 *      {"status":"ble_connected"}
 *      {"status":"wifi_disconnected"}
 * -----------------------------------------------------------
 */
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "types.h"

// Initialize BLE stack, server, service, and both characteristics.
void ble_init();

// Pack the ECG block + performance metrics into a raw binary packet and notify via ECG characteristic.
void ble_uploadBlock(const Block& blk, bool leadsOff, bool loPlus, bool loMinus,
                     const char* warning, const char* severity,
                     const int32_t* dspData = nullptr,
                     const PerformanceMetrics* metrics = nullptr,
                     const char* deviceResult = nullptr);

// Push a plain JSON status string to the Status characteristic.
void ble_sendStatus(const String& json);

// Advertising controls
void ble_startAdvertising();
void ble_stopAdvertising();

// MUST be called every loop() — handles deferred advertising restart after disconnect safely
void ble_update();

// State queries
bool     ble_isConnected();
uint16_t ble_getMtu();
