/**
 * led_status.cpp
 * -----------------------------------------------------------
 * SWET_IOT - Onboard Built-in LED Driver Implementation (XIAO ESP32-S3)
 * -----------------------------------------------------------
 * Onboard User LED is on GPIO 21 (Active-LOW: LOW = ON, HIGH = OFF)
 */

#include "led_status.h"
#include "pin_config.h"
#include "ble_transport.h"
#include <Arduino.h>
#include <WiFi.h>

static bool s_wifiConnected = false;
static bool s_bleConnected  = false;
static bool s_poweredOn     = true;

void ledStatus_init() {
    pinMode(LED_PIN, OUTPUT);
    s_poweredOn = true;
    ledStatus_update();
}

void ledStatus_powerOn() {
    s_poweredOn = true;
    ledStatus_update();
}

void ledStatus_powerOff() {
    s_poweredOn     = false;
    s_wifiConnected = false;
    s_bleConnected  = false;
    digitalWrite(LED_PIN, HIGH); // Active-LOW: HIGH = OFF (Inbuilt LED OFF when power down)
}

void ledStatus_setWifiConnected(bool connected) {
    s_wifiConnected = connected;
    ledStatus_update();
}

void ledStatus_setBleConnected(bool connected) {
    s_bleConnected = connected;
    ledStatus_update();
}

void ledStatus_update() {
    if (!s_poweredOn) {
        digitalWrite(LED_PIN, HIGH); // Inbuilt LED OFF when powered down
        return;
    }

    // Direct real-time hardware status check
    bool wifiConn = s_wifiConnected || (WiFi.status() == WL_CONNECTED);
    bool bleConn  = s_bleConnected  || ble_isConnected();

    // Inbuilt LED (GPIO 21) Behavior:
    // Solid ON (LOW) when WiFi is connected.
    // Blinking (every 500ms) when only Bluetooth is connected.
    // Solid OFF (HIGH) when nothing is connected or powered down.
    
    static uint32_t lastBlinkMs = 0;
    static bool blinkState = false;
    uint32_t now = millis();

    if (wifiConn) {
        digitalWrite(LED_PIN, LOW); // Active-LOW: LOW = ON (Solid for WiFi)
    } 
    else if (bleConn) {
        // Blink every 500ms for BLE
        if (now - lastBlinkMs >= 500) {
            lastBlinkMs = now;
            blinkState = !blinkState;
            digitalWrite(LED_PIN, blinkState ? LOW : HIGH);
        }
    } 
    else {
        digitalWrite(LED_PIN, HIGH); // Active-LOW: HIGH = OFF
    }
}
