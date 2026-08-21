/**
 * pin_config.h
 * -----------------------------------------------------------
 * SWET_IOT - Hardware Pin Mapping
 * Board   : Seeed Studio XIAO ESP32S3
 * Module  : ProtoCentral ADS1292R Breakout v3.1
 * SPI Bus : FSPI (Hardware SPI)
 * -----------------------------------------------------------
 */

#pragma once

// ==========================================================
// Hardware SPI (XIAO ESP32S3 Pins D8, D9, D10)
// ==========================================================
#define ADS1292_SCK_PIN     7   // D8 (SCK)
#define ADS1292_MISO_PIN    8   // D9 (MISO)
#define ADS1292_MOSI_PIN    9   // D10 (MOSI)

// ==========================================================
// ADS1292R Control / GPIO Pins (XIAO Pins D0 - D3)
// ==========================================================
#define ADS1292_CS_PIN       1   // D0 Chip Select (active LOW)
#define ADS1292_START_PIN    2   // D1 START (active HIGH -> begins conversions)
#define ADS1292_RESET_PIN   3   // D2 RESET (active LOW pulse to reset device)
#define ADS1292_DRDY_PIN    4   // D3 DRDY  (active LOW -> new data ready, input)

// ==========================================================
// MPU6050 IMU I2C Pins (XIAO Pins D4, D5)
// ==========================================================
#define MPU6050_SDA_PIN     5   // D4 (I2C SDA)
#define MPU6050_SCL_PIN     6   // D5 (I2C SCL)

// ==========================================================
// Passive Piezo Electric Buzzer Pin (XIAO Pin D6 / GPIO 43)
// ==========================================================
#define BUZZER_PIN          43  // D6 (GPIO 43) — Signal/Positive to D6, Negative to GND

// ==========================================================
// Onboard Built-in LED Pin Mapping (XIAO ESP32-S3)
// ==========================================================
#define POWER_RED_LED_PIN        21  // GPIO 21 (Onboard User LED — Active-LOW: LOW = ON, HIGH = OFF)
#define LED_PIN                  21  // Onboard User LED (GPIO 21)





