/**
 * thermal_shutdown.cpp
 * -----------------------------------------------------------
 * SWET_IOT - Thermal Protection & Deep Sleep Shutdown
 * -----------------------------------------------------------
 */

#include "thermal_shutdown.h"
#include "ble_transport.h"
#include "config.h"
#include "buzzer.h"
#include "led_status.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "network.h"
#include "pin_config.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>

void system_thermalShutdown(float tempC) {
  Serial.println(
      F("=========================================================="));
  Serial.printf("[THERMAL] CRITICAL: MPU6050 die temperature %.1fC exceeds "
                "DIE_TEMP_CRIT_C (%.1fC)!\n",
                tempC, DIE_TEMP_CRIT_C);
  Serial.println(F("[THERMAL] Initiating Thermal Protection Shutdown..."));
  Serial.println(
      F("=========================================================="));
  Serial.flush();

  // 1. Transmit Emergency Thermal Shutdown Alert Payload to Backend API over Wi-Fi
  network_sendThermalShutdownAlert(tempC);

  // 2. Transmit Emergency Thermal Shutdown JSON status over BLE (if connected)
  if (ble_isConnected()) {
    String bleJson = "{";
    bleJson += "\"dieTempC\":" + String(tempC, 1) + ",";
    bleJson += "\"status\":\"thermal_shutdown\",";
    bleJson += "\"thermalShutdown\":true,";
    bleJson += "\"warning\":\"THERMAL_SHUTDOWN\",";
    bleJson += "\"severity\":\"CRITICAL\"";
    bleJson += "}";
    Serial.println(F("[THERMAL] Sending emergency BLE status notification..."));
    ble_sendStatus(bleJson);
    delay(150);
  }

  // 3. Sound 3 Loud High-Pitch Emergency Alarm Beeps on Piezo Buzzer BEFORE shutdown
  Serial.println(F("[THERMAL] Sounding 3 emergency thermal shutdown buzzer beeps..."));
  buzzer_thermalShutdown();

  // 4. Turn OFF Wi-Fi
  Serial.println(F("[THERMAL] Shutting down Wi-Fi..."));
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // 5. Turn OFF Bluetooth NimBLE stack
  Serial.println(F("[THERMAL] Shutting down Bluetooth..."));
  NimBLEDevice::deinit(true);

  // 6. Turn OFF ADS1292R Sensor (Power down / Standby)
  Serial.println(F("[THERMAL] Powering down ADS1292R sensor..."));
  pinMode(ADS1292_START_PIN, OUTPUT);
  digitalWrite(ADS1292_START_PIN, LOW);
  pinMode(ADS1292_RESET_PIN, OUTPUT);
  digitalWrite(ADS1292_RESET_PIN, LOW);
  pinMode(ADS1292_CS_PIN, OUTPUT);
  digitalWrite(ADS1292_CS_PIN, HIGH);

  // 7. Turn OFF Onboard Status LED
  Serial.println(F("[THERMAL] Turning OFF status LEDs..."));
  ledStatus_powerOff();

  // 8. Configure BOOT Button (GPIO 0) RTC Wakeup for ESP32-S3
  Serial.println(F("[THERMAL] Configuring BOOT button (GPIO 0) wakeup..."));
  rtc_gpio_init(GPIO_NUM_0);
  rtc_gpio_set_direction(GPIO_NUM_0, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(GPIO_NUM_0);
  rtc_gpio_pulldown_dis(GPIO_NUM_0);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // Active-LOW wakeup on BOOT button (GPIO 0)

  Serial.println(F("[THERMAL] Entering ESP32 Deep Sleep now. Press BOOT button to restart."));
  Serial.flush();
  delay(200);

  esp_deep_sleep_start();
}
