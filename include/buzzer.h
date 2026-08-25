/**
 * buzzer.h
 * -----------------------------------------------------------
 * SWET_IOT - Passive Piezo Electric Buzzer Driver & Audio Signals
 * -----------------------------------------------------------
 */

#pragma once
#include <Arduino.h>

void buzzer_init();
void buzzer_on(uint32_t freq);
void buzzer_off();

// Audio Alerts
void buzzer_wifiConnected();
void buzzer_wifiDisconnected();
void buzzer_bleConnected();
void buzzer_bleDisconnected();
void buzzer_thermalShutdown();
void start_thermal_warning();
void buzzer_update();
