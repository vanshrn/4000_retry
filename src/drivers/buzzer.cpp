/**
 * buzzer.cpp
 * -----------------------------------------------------------
 * SWET_IOT - Passive Piezo Electric Buzzer Driver Implementation
 * -----------------------------------------------------------
 */

#include "buzzer.h"
#include "pin_config.h"

#define BUZZER_LEDC_CHANNEL          0
#define BUZZER_LEDC_RESOLUTION_BITS  8
#define BUZZER_VOLUME_DUTY           255  // 50% Square Wave Duty Cycle -> MAXIMUM Piezo Sound Loudness (0-255 scale)

void buzzer_init() {
    if (BUZZER_PIN < 0) return;
    ledcSetup(BUZZER_LEDC_CHANNEL, 2700, BUZZER_LEDC_RESOLUTION_BITS);
    ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CHANNEL);
    buzzer_off();
}

void buzzer_on(uint32_t freq) {
    if (BUZZER_PIN < 0) return;
    ledcWriteTone(BUZZER_LEDC_CHANNEL, freq);
    ledcWrite(BUZZER_LEDC_CHANNEL, BUZZER_VOLUME_DUTY);
}

void buzzer_off() {
    if (BUZZER_PIN < 0) return;
    ledcWrite(BUZZER_LEDC_CHANNEL, 0);
    ledcWriteTone(BUZZER_LEDC_CHANNEL, 0);
}

void buzzer_wifiConnected() {
    if (BUZZER_PIN < 0) return;
    buzzer_on(2700);
    delay(120);
    buzzer_off();
    delay(60);
    buzzer_on(3500);
    delay(120);
    buzzer_off();
}

void buzzer_wifiDisconnected() {
    if (BUZZER_PIN < 0) return;
    buzzer_on(2200);
    delay(250);
    buzzer_off();
}

void buzzer_bleConnected() {
    if (BUZZER_PIN < 0) return;
    buzzer_on(3000);
    delay(120);
    buzzer_off();
    delay(60);
    buzzer_on(3800);
    delay(120);
    buzzer_off();
}

void buzzer_bleDisconnected() {
    if (BUZZER_PIN < 0) return;
    buzzer_on(2400);
    delay(250);
    buzzer_off();
}

void buzzer_thermalShutdown() {
    if (BUZZER_PIN < 0) return;
    for (int i = 0; i < 3; i++) {
        buzzer_on(3600);
        delay(180);
        buzzer_off();
        delay(100);
    }
}
