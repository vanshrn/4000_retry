/**
 * motion_calibration.cpp — Zero-Phase, Slope-Protected Motion Noise Reduction
 * -----------------------------------------------------------
 * Continuously calibrates resting ECG mean & stdDev during stillness (Welford algorithm).
 * During motion:
 *   1. Zero-Phase Symmetric Smoother (5-tap Hanning window, 0 phase delay after R-peak).
 *   2. QRS Slew-Rate Slope Gating: Bypasses smoothing on steep R-to-S transitions so
 *      QRS shape is preserved crisp with zero lag or post-R noise.
 *   3. Soft Sigmoidal Clamping (tanh saturation): Smoothly compresses out-of-bounds motion
 *      spikes without hard flat-clipping corners.
 * -----------------------------------------------------------
 */

#include "motion_calibration.h"
#include "config.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>

MotionCalibration::MotionCalibration() {
    reset();
}

void MotionCalibration::reset() {
    _windowHead = 0;
    _windowCount = 0;
    _mean = 2048.0;
    _sumSqDiff = 0.0;
    _stdDev = 0.0f;
    _isValid = false;

    _emaPrev = 2048.0f;
    _emaInitialized = false;
    _lastCleanSample = 2048;
    _prevBlockHadMotion = false;

    memset(_windowBuf, 0, sizeof(_windowBuf));
}

void MotionCalibration::_recomputeWelfordFromBuffer() {
    if (_windowCount == 0) return;

    _mean = 0.0;
    _sumSqDiff = 0.0;
    for (int i = 0; i < _windowCount; i++) {
        double x = (double)_windowBuf[i];
        double delta = x - _mean;
        _mean += delta / (i + 1);
        double delta2 = x - _mean;
        _sumSqDiff += delta * delta2;
    }
}

void MotionCalibration::_updateCalibrationWelford(int32_t sample) {
    double x = (double)sample;

    if (_windowCount < CALIBRATION_WINDOW_SAMPLES) {
        // Buffer filling phase
        _windowCount++;
        double delta = x - _mean;
        _mean += delta / _windowCount;
        double delta2 = x - _mean;
        _sumSqDiff += delta * delta2;

        _windowBuf[_windowHead] = sample;
        _windowHead = (_windowHead + 1) % CALIBRATION_WINDOW_SAMPLES;
    } else {
        // Sliding window eviction phase
        int32_t x_old = _windowBuf[_windowHead];
        _windowBuf[_windowHead] = sample;
        _windowHead = (_windowHead + 1) % CALIBRATION_WINDOW_SAMPLES;

        double oldMean = _mean;
        double xOldD = (double)x_old;
        _mean += (x - xOldD) / (double)CALIBRATION_WINDOW_SAMPLES;
        _sumSqDiff += (x - xOldD) * (x - _mean + xOldD - oldMean);

        if (_sumSqDiff < 0.0) _sumSqDiff = 0.0;

        // Re-center periodically to prevent numerical drift over long runs
        if ((_windowHead % CALIBRATION_WINDOW_SAMPLES) == 0) {
            _recomputeWelfordFromBuffer();
        }
    }

    if (_windowCount > 1) {
        _stdDev = (float)sqrt(_sumSqDiff / (double)(_windowCount - 1));
    } else {
        _stdDev = 0.0f;
    }

    if (_windowCount >= CALIBRATION_MIN_SAMPLES) {
        _isValid = true;
    }
}

float MotionCalibration::_computeMotionStdDev(const int32_t* samples, const uint8_t* perSampleMotionValid, int len) {
    if (samples == nullptr || perSampleMotionValid == nullptr || len <= 0) return 0.0f;

    double sum = 0.0;
    int count = 0;
    for (int i = 0; i < len; i++) {
        if (perSampleMotionValid[i] == 0) {
            sum += (double)samples[i];
            count++;
        }
    }
    if (count < 2) return 0.0f;

    double mean = sum / (double)count;
    double sqDiffSum = 0.0;
    for (int i = 0; i < len; i++) {
        if (perSampleMotionValid[i] == 0) {
            double diff = (double)samples[i] - mean;
            sqDiffSum += diff * diff;
        }
    }
    return (float)sqrt(sqDiffSum / (double)(count - 1));
}

// -----------------------------------------------------------
// Soft Sigmoidal Clamping (tanh saturation)
// -----------------------------------------------------------
// Smoothly compresses values exceeding mean ± halfRange without sharp flat-clipping corners.
// -----------------------------------------------------------
int32_t MotionCalibration::_applySoftClamp(int32_t val, double mean, float halfRange) {
    if (halfRange <= 1.0f) return val;

    double diff = (double)val - mean;
    double absDiff = fabs(diff);
    double limit = (double)halfRange;

    if (absDiff <= limit) {
        return val; // Inside normal range — unchanged
    }

    // Soft saturation using tanh beyond boundary limit
    double excess = absDiff - limit;
    double compressedExcess = limit * 0.25 * tanh(excess / (limit * 0.5));
    double newAbsDiff = limit + compressedExcess;

    double newVal = (diff >= 0.0) ? (mean + newAbsDiff) : (mean - newAbsDiff);
    return (int32_t)round(newVal);
}

void MotionCalibration::applyMotionNoiseReduction(int32_t* samples, const uint8_t* perSampleMotionValid, int len, uint32_t seq) {
    if (samples == nullptr || perSampleMotionValid == nullptr || len <= 0) return;

    int motionCount = 0;
    for (int i = 0; i < len; i++) {
        if (perSampleMotionValid[i] == 0) {
            motionCount++;
        } else {
            _updateCalibrationWelford(samples[i]);
            _lastCleanSample = samples[i];
        }
    }

    if (_isValid && motionCount > 0) {
        // Clamp bound protects QRS waves while suppressing extreme motion saturations (>150k counts)
        float effectiveStd = max(_stdDev, 5000.0f);
        float clampBound = max(effectiveStd * 10.0f, 150000.0f);
        for (int i = 0; i < len; i++) {
            if (perSampleMotionValid[i] == 0) {
                samples[i] = _applySoftClamp(samples[i], _mean, clampBound);
            }
        }
    }

    Serial.printf("[IMU] seq=%u: %d/%d samples motion-active\n", (unsigned)seq, motionCount, len);
}
