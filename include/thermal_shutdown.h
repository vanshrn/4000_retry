/**
 * thermal_shutdown.h
 * -----------------------------------------------------------
 * SWET_IOT - Thermal Protection & Deep Sleep Shutdown
 * -----------------------------------------------------------
 */

#pragma once

/**
 * Triggers full hardware shutdown and puts ESP32-S3 into Deep Sleep
 * when MPU6050 die temperature exceeds DIE_TEMP_CRIT_C (55.0°C).
 * 
 * - Disconnects & disables Wi-Fi
 * - De-initializes Bluetooth (NimBLE)
 * - Powers down ADS1292R sensor (START=LOW, RESET=LOW, CS=HIGH)
 * - Turns off all LEDs
 * - Configures BOOT button (GPIO 0) as EXT0 active-LOW wakeup source
 * - Enters esp_deep_sleep_start()
 */
void system_thermalShutdown(float tempC);
