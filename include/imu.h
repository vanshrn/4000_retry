/**
 * imu.h
 * -----------------------------------------------------------
 * SWET_IOT - MPU6050 IMU Driver & Motion State Classifier
 * -----------------------------------------------------------
 */

#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "pin_config.h"
#include "types.h"

class IMUDriver {
public:
    IMUDriver();

    bool begin();
    void update(); // Non-blocking sample & variance calculation
    MotionState getMotionState() const;
    const char* getMotionStateString() const;

    float getAccelX() const;
    float getAccelY() const;
    float getAccelZ() const;
    float getAccelMag() const;
    float getGyroMag() const;
    float getDieTempC() const;

private:
    bool     _initialized;
    float    _ax, _ay, _az; // Acceleration in g
    float    _mag;          // Magnitude in g (~1.0g at rest)
    float    _gx, _gy, _gz; // Gyroscope in deg/s
    float    _gyroMag;      // Gyroscope magnitude in deg/s
    float    _dieTempC;     // MPU6050 die temperature in deg C
    int16_t  _gyroBiasX, _gyroBiasY, _gyroBiasZ; // Calibrated hardware zero-rate bias (LSBs)

    static const uint8_t BUF_SIZE = 25;
    float    _magHistory[BUF_SIZE];
    uint8_t  _historyIdx;
    uint8_t  _historyCount;

    MotionState _currentState;
    uint32_t    _lastSampleMs;
    uint32_t    _lastTempLogMs; // Rate-limits die temp debug serial output to ~5s
    uint8_t     _updateCounter;  // Counts updates for periodic log output
    float       _lastVariance;   // Last computed variance (for debug logging)
    float       _lastMean;       // Last computed mean magnitude (for debug logging)

    // I2C freeze detection & loose pin resilience
    static const uint8_t FREEZE_COUNT_MAX = 75; // 75 x 40ms = 3.0s of 6-axis identical reads
    int16_t  _lastRawAx, _lastRawAy, _lastRawAz;
    int16_t  _lastRawGx, _lastRawGy, _lastRawGz;
    uint8_t  _frozenCount;
    uint32_t _lastReconnectMs;

    void calculateMotionState();
    bool recoverI2C(); // Issues Wire.end/begin + mpu.initialize() to recover bus
};


