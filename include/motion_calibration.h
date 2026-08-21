/**
 * motion_calibration.h — Calibration-Range-Based Motion Noise Reduction
 * -----------------------------------------------------------
 * Continuously calibrates resting ECG mean & stdDev during stillness (Welford algorithm).
 * During motion, applies adaptive EMA smoothing followed by hard range clamping.
 * -----------------------------------------------------------
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <Arduino.h>
#include "config.h"

class MotionCalibration {
public:
    MotionCalibration();
    void reset();

    // Main entry point: updates calibration during stillness, applies adaptive EMA + clamping during motion.
    // Must be called AFTER pipeline & classifier processing in main.cpp.
    void applyMotionNoiseReduction(int32_t* samples, const uint8_t* perSampleMotionValid, int len, uint32_t seq);

    bool isValid() const { return _isValid; }
    float getMean() const { return (float)_mean; }
    float getStdDev() const { return (float)_stdDev; }

private:
    int32_t  _windowBuf[CALIBRATION_WINDOW_SAMPLES];
    int      _windowHead;
    int      _windowCount;

    double   _mean;
    double   _sumSqDiff;
    float    _stdDev;
    bool     _isValid;

    float    _emaPrev;
    bool     _emaInitialized;
    int32_t  _lastCleanSample;      // Last clean still sample — used to anchor EMA at motion transitions
    bool     _prevBlockHadMotion;   // True if the previous block contained any motion samples

    void _updateCalibrationWelford(int32_t sample);
    void _recomputeWelfordFromBuffer();
    float _computeMotionStdDev(const int32_t* samples, const uint8_t* perSampleMotionValid, int len);
    int32_t _applySoftClamp(int32_t val, double mean, float halfRange);
};

