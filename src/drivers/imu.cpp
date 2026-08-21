/**
 * imu.cpp
 * -----------------------------------------------------------
 * SWET_IOT - MPU6050 IMU Driver & Motion State Classifier
 *
 * Fixes applied:
 *  1. Relaxed variance thresholds (0.001 / 0.01 was too tight)
 *  2. Added per-sample debug logs: raw ax/ay/az, magnitude, variance, state
 *  3. Added I2C scan diagnostic if testConnection() fails
 * -----------------------------------------------------------
 */

#include "imu.h"
#include "config.h"
#include "thermal_shutdown.h"
#include "ble_transport.h"
#include "buzzer.h"
#include <MPU6050.h>
#include <math.h>

static MPU6050 mpu(0x68); // AD0 tied to GND -> address 0x68

// ---- Tunable thresholds -------------------------------------------
// Based on live hardware test results:
//   - Idle sensor was showing LIGHT  -> THRESH_STILL was too tight
//   - Shaking correctly showed HIGH  -> THRESH_LIGHT was correct
//
// MOTION_STILL : variance < THRESH_STILL  AND  mean within THRESH_STILL_G of 1g
// MOTION_LIGHT : variance < THRESH_LIGHT
// MOTION_HIGH  : everything else
// -------------------------------------------------------------------
static constexpr float THRESH_STILL   = 0.0040f;  // 0.0040g^2 (accommodates resting hand tremor & breathing)
static constexpr float THRESH_STILL_G = 0.20f;    // max deviation from 1g for STILL
static constexpr float THRESH_LIGHT   = 0.045f;   // 0.045g^2 threshold for walking/running activities


// IMU debug log rate: print accel + state every N updates (~25Hz each update)
// 25 updates * 40ms = ~1000ms -> print roughly every 1 second
static constexpr uint8_t LOG_EVERY_N_UPDATES = 25;

IMUDriver::IMUDriver()
    : _initialized(false),
      _ax(0.0f), _ay(0.0f), _az(1.0f),
      _mag(1.0f),
      _gx(0.0f), _gy(0.0f), _gz(0.0f),
      _gyroMag(0.0f),
      _dieTempC(0.0f),
      _gyroBiasX(0), _gyroBiasY(0), _gyroBiasZ(0),
      _historyIdx(0),
      _historyCount(0),
      _currentState(MotionState::MOTION_STILL),
      _lastSampleMs(0),
      _lastTempLogMs(0),
      _updateCounter(0),
      _lastVariance(0.0f),
      _lastMean(1.0f),
      _lastRawAx(0), _lastRawAy(0), _lastRawAz(0),
      _frozenCount(0)
{
    for (uint8_t i = 0; i < BUF_SIZE; i++) {
        _magHistory[i] = 1.0f;
    }
}



// ------------------------------------------------------------------
// I2C bus scan — prints all found addresses (helps diagnose wrong addr)
// ------------------------------------------------------------------
static void scanI2C() {
    Serial.println(F("#[IMU] Scanning I2C bus for devices..."));
    uint8_t found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("#[IMU]  Device found at address 0x%02X\n", addr);
            found++;
        }
    }
    if (found == 0) {
        Serial.println(F("#[IMU]  No I2C devices found! Check SDA/SCL/VCC/GND wiring."));
    } else {
        Serial.printf("#[IMU]  Total devices found: %u\n", (unsigned)found);
    }
}

bool IMUDriver::begin() {
    Serial.println(F("#[IMU] Initializing MPU6050 over I2C..."));
    Serial.printf("#[IMU]  SDA=GPIO%d  SCL=GPIO%d  Speed=400kHz\n",
                  MPU6050_SDA_PIN, MPU6050_SCL_PIN);

    Wire.begin(MPU6050_SDA_PIN, MPU6050_SCL_PIN);
    Wire.setClock(400000); // 400kHz Fast Mode
    Wire.setTimeOut(100);  // 100ms I2C timeout prevents ESP32-S3 bus lockups

    // Run I2C scan first so any wiring problem is immediately obvious
    scanI2C();

    mpu.initialize();

    Wire.beginTransmission(0x68);
    Wire.write(0x75); // WHO_AM_I register address
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)0x68, (uint8_t)1);
    uint8_t whoAmI = Wire.available() ? Wire.read() : 0x00;

    Serial.printf("#[IMU] WHO_AM_I register = 0x%02X ", whoAmI);
    if (whoAmI == 0x68)      Serial.println(F("(genuine MPU6050)"));
    else if (whoAmI == 0x72) Serial.println(F("(GY-521 clone variant A)"));
    else if (whoAmI == 0x98) Serial.println(F("(GY-521 clone variant B)"));
    else if (whoAmI == 0x00 || whoAmI == 0xFF) {
        Serial.println(F("-> INVALID! Device not responding properly."));
        Serial.println(F("#[IMU] ERROR: WHO_AM_I=0x00 or 0xFF. Check wiring."));
        _initialized = false;
        return false;
    } else {
        Serial.println(F("(unknown clone — proceeding anyway)"));
    }

    // Set full scale range to +/- 2g (16384 LSB/g) and +/- 250 deg/s (131 LSB/dps)
    mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_2);
    mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_250);
    uint8_t range = mpu.getFullScaleAccelRange();
    Serial.printf("#[IMU] MPU6050 OK! Address=0x68  AccelFS=%ug  GyroFS=250dps\n",
                  (unsigned)(1 << range) * 2);

    // Auto-calibrate gyro zero-rate bias (50 samples over ~200ms)
    int32_t gxSum = 0, gySum = 0, gzSum = 0;
    for (int i = 0; i < 50; i++) {
        int16_t ax, ay, az, gx, gy, gz;
        mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
        gxSum += gx;
        gySum += gy;
        gzSum += gz;
        delay(4);
    }
    _gyroBiasX = (int16_t)(gxSum / 50);
    _gyroBiasY = (int16_t)(gySum / 50);
    _gyroBiasZ = (int16_t)(gzSum / 50);
    Serial.printf("#[IMU] Gyro zero bias calibrated: X=%d Y=%d Z=%d LSB\n",
                  _gyroBiasX, _gyroBiasY, _gyroBiasZ);

    int16_t ax, ay, az, gx, gy, gz;
    mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
    Serial.printf("#[IMU] First read: raw ax=%d ay=%d az=%d gx=%d gy=%d gz=%d\n", ax, ay, az, gx, gy, gz);

    _initialized = true;
    Serial.println(F("#[IMU] MPU6050 initialized and ready."));
    Serial.printf("#[IMU] Motion thresholds: STILL (accelVar<%.4f, gyro<%.1fdps), LIGHT (accelVar<%.4f, gyro<%.1fdps)\n",
                  THRESH_STILL, GYRO_STILL_THRESHOLD_DPS, THRESH_LIGHT, GYRO_LIGHT_THRESHOLD_DPS);
    return true;
}

void IMUDriver::update() {
    uint32_t now = millis();
    if (now - _lastSampleMs < 40) return;
    _lastSampleMs = now;

    if (!_initialized) {
        // Periodic auto-reconnect every 2 seconds if unsoldered pins lose contact
        if (now - _lastReconnectMs >= 2000) {
            _lastReconnectMs = now;
            if (recoverI2C()) {
                Serial.println(F("#[IMU] Unsoldered pin re-connected! MPU6050 resumed."));
            }
        }
        // Fallback while disconnected: allow ECG data and calibration to stream cleanly
        _currentState = MotionState::MOTION_STILL;
        return;
    }

    uint8_t buf[14];
    I2Cdev::readBytes(0x68, 0x3B, 14, buf);
    int16_t rawAx   = (int16_t)((buf[0] << 8) | buf[1]);
    int16_t rawAy   = (int16_t)((buf[2] << 8) | buf[3]);
    int16_t rawAz   = (int16_t)((buf[4] << 8) | buf[5]);
    int16_t tempRaw = (int16_t)((buf[6] << 8) | buf[7]);
    int16_t rawGx   = (int16_t)((buf[8] << 8) | buf[9]);
    int16_t rawGy   = (int16_t)((buf[10] << 8) | buf[11]);
    int16_t rawGz   = (int16_t)((buf[12] << 8) | buf[13]);

    _dieTempC = ((float)tempRaw / 340.0f) + 36.53f;

    // ---- I2C Freeze Detection (75 reads = 3 seconds of 6-axis identical data) ----
    bool isIdentical6Axis = (rawAx == _lastRawAx && rawAy == _lastRawAy && rawAz == _lastRawAz &&
                             rawGx == _lastRawGx && rawGy == _lastRawGy && rawGz == _lastRawGz);
    bool isAllZero = (rawAx == 0 && rawAy == 0 && rawAz == 0 && rawGx == 0 && rawGy == 0 && rawGz == 0);

    if (isIdentical6Axis || isAllZero) {
        _frozenCount++;
        if (_frozenCount >= FREEZE_COUNT_MAX) {
            Serial.printf("#[IMU] FROZEN detected (%u identical 6-axis reads). Recovering I2C bus...\n",
                          (unsigned)_frozenCount);
            _currentState = MotionState::MOTION_STILL; // Default still during recovery
            if (!recoverI2C()) {
                _initialized = false; // Triggers auto-reconnect loop
            }
            _frozenCount = 0;
            return;
        }
    } else {
        _frozenCount = 0;
    }
    _lastRawAx = rawAx; _lastRawAy = rawAy; _lastRawAz = rawAz;
    _lastRawGx = rawGx; _lastRawGy = rawGy; _lastRawGz = rawGz;

    // Convert raw counts to g (16384 LSB/g at +/- 2g range)
    _ax = (float)rawAx / 16384.0f;
    _ay = (float)rawAy / 16384.0f;
    _az = (float)rawAz / 16384.0f;
    _mag = sqrtf(_ax * _ax + _ay * _ay + _az * _az);

    // Convert raw gyro to deg/s with zero-bias subtraction (131.0 LSB/dps at +/- 250 dps range)
    _gx = (float)(rawGx - _gyroBiasX) / 131.0f;
    _gy = (float)(rawGy - _gyroBiasY) / 131.0f;
    _gz = (float)(rawGz - _gyroBiasZ) / 131.0f;
    _gyroMag = sqrtf(_gx * _gx + _gy * _gy + _gz * _gz);

    // Add to rolling history buffer
    _magHistory[_historyIdx] = _mag;
    _historyIdx = (_historyIdx + 1) % BUF_SIZE;
    if (_historyCount < BUF_SIZE) {
        _historyCount++;
    }

    calculateMotionState();

    // ---- Thermal Protection Threshold Checks ----
    if (_dieTempC >= DIE_TEMP_CRIT_C) {
        system_thermalShutdown(_dieTempC);
    } else if (_dieTempC >= DIE_TEMP_WARN_C) {
        static uint32_t lastWarnMs = 0;
        if (now - lastWarnMs >= 10000) {
            lastWarnMs = now;
            Serial.printf("[IMU] DIE_TEMP WARNING: %.1fC exceeds DIE_TEMP_WARN_C (%.1fC)\n", _dieTempC, DIE_TEMP_WARN_C);
            // Sound thermal warning tone
            buzzer_on(2800);
            delay(150);
            buzzer_off();
            if (ble_isConnected()) {
                String warnJson = "{\"dieTempC\":" + String(_dieTempC, 1) + ",\"status\":\"thermal_warning\",\"warning\":\"THERMAL_WARNING\"}";
                ble_sendStatus(warnJson);
            }
        }
    }

    // ---- Die Temperature Logging & BLE Status Push (~5s rate-limited) ----
    if (now - _lastTempLogMs >= 5000) {
        _lastTempLogMs = now;
        Serial.printf("[IMU] DIE_TEMP: %.1fC\n", _dieTempC);
        if (ble_isConnected()) {
            String bleTempJson = "{\"dieTempC\":" + String(_dieTempC, 1) + ",\"status\":\"ok\"}";
            ble_sendStatus(bleTempJson);
        }
    }

    // ---- Per-sample debug logging (Accel & Gyro detailed values) ----
    _updateCounter++;
    if (_updateCounter >= LOG_EVERY_N_UPDATES) {
        _updateCounter = 0;
        Serial.printf("[IMU] ACCEL: ax=%.3fg ay=%.3fg az=%.3fg |a|=%.3fg var=%.5f | GYRO: gx=%.1f gy=%.1f gz=%.1f |w|=%.1fdps -> %s\n",
                      _ax, _ay, _az, _mag, _lastVariance,
                      _gx, _gy, _gz, _gyroMag,
                      getMotionStateString());
    }
}

// ==========================================================
// recoverI2C()
// ==========================================================
bool IMUDriver::recoverI2C() {
    Wire.end();
    delay(10);
    Wire.begin(MPU6050_SDA_PIN, MPU6050_SCL_PIN);
    Wire.setClock(400000);
    Wire.setTimeOut(100);
    delay(10);

    mpu.initialize();

    uint8_t whoAmI = 0x00;
    for (int retry = 0; retry < 5; retry++) {
        Wire.beginTransmission(0x68);
        Wire.write(0x75);
        Wire.endTransmission(false);
        Wire.requestFrom((uint8_t)0x68, (uint8_t)1);
        whoAmI = Wire.available() ? Wire.read() : 0x00;
        if (whoAmI != 0x00 && whoAmI != 0xFF) break;
        delay(20);
    }

    if (whoAmI == 0x00 || whoAmI == 0xFF) {
        Serial.printf("#[IMU] I2C recovery FAILED. WHO_AM_I=0x%02X. Loose pins / offline.\n", whoAmI);
        _initialized = false;
        _currentState = MotionState::MOTION_STILL; // Default still so ECG stream & calibration are unimpeded
        return false;
    }

    mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_2);
    mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_250);
    _initialized = true;
    Serial.printf("#[IMU] I2C recovery OK. WHO_AM_I=0x%02X. Resuming 6-axis sampling.\n", whoAmI);
    return true;
}


void IMUDriver::calculateMotionState() {
    if (_historyCount == 0) return;

    // Calculate mean magnitude
    float sum = 0.0f;
    for (uint8_t i = 0; i < _historyCount; i++) {
        sum += _magHistory[i];
    }
    float mean = sum / (float)_historyCount;

    // Calculate variance
    float sumSqDiff = 0.0f;
    for (uint8_t i = 0; i < _historyCount; i++) {
        float diff = _magHistory[i] - mean;
        sumSqDiff += diff * diff;
    }
    float variance = sumSqDiff / (float)_historyCount;

    // Store for debug logging
    _lastVariance = variance;
    _lastMean     = mean;

    MotionState prev = _currentState;

    // ---- Classification with combined Accel + Gyro STILL check ----
    bool accelStill = (variance < THRESH_STILL && fabsf(mean - 1.0f) < THRESH_STILL_G);
    bool gyroStill  = (_gyroMag < GYRO_STILL_THRESHOLD_DPS);

    if (accelStill && gyroStill) {
        _currentState = MotionState::MOTION_STILL;
    } else if (variance < THRESH_LIGHT && _gyroMag < GYRO_LIGHT_THRESHOLD_DPS) {
        _currentState = MotionState::MOTION_LIGHT;
    } else {
        _currentState = MotionState::MOTION_HIGH;
    }

    // Log every state transition immediately
    if (_currentState != prev) {
        Serial.printf("[IMU] STATE CHANGE: %s -> %s  (var=%.5f gyro=%.1fdps)\n",
                      prev == MotionState::MOTION_STILL ? "STILL" :
                      prev == MotionState::MOTION_LIGHT ? "LIGHT" : "HIGH",
                      getMotionStateString(), variance, _gyroMag);
    }
}

MotionState IMUDriver::getMotionState() const {
    return _currentState;
}

const char* IMUDriver::getMotionStateString() const {
    switch (_currentState) {
        case MotionState::MOTION_STILL: return "STILL";
        case MotionState::MOTION_LIGHT: return "LIGHT";
        case MotionState::MOTION_HIGH:  return "HIGH";
        default:                        return "UNKNOWN";
    }
}

float IMUDriver::getAccelX() const   { return _ax; }
float IMUDriver::getAccelY() const   { return _ay; }
float IMUDriver::getAccelZ() const   { return _az; }
float IMUDriver::getAccelMag() const { return _mag; }
float IMUDriver::getGyroMag() const  { return _gyroMag; }
float IMUDriver::getDieTempC() const { return _dieTempC; }

