/**
 * led_status.h
 * -----------------------------------------------------------
 * SWET_IOT - Onboard Built-in LED Controller (XIAO ESP32-S3)
 * -----------------------------------------------------------
 *  - Power Active / Booted: Built-in LED ON (LOW = ON)
 *  - Wi-Fi or BLE Connected: Built-in LED Solid ON (LOW = ON)
 *  - Disconnected (Setup / Searching): Built-in LED Blinks
 *  - Power Down / Deep Sleep: Built-in LED OFF (HIGH = OFF)
 * -----------------------------------------------------------
 */

#pragma once
#include <Arduino.h>

void ledStatus_init();
void ledStatus_powerOn();
void ledStatus_powerOff();

void ledStatus_setWifiConnected(bool connected);
void ledStatus_setBleConnected(bool connected);
void ledStatus_update();
