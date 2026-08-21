/**
 * ecg_pipeline.h  — v2.0 for 500 SPS
 * -----------------------------------------------------------
 * Full DSP pipeline: DC-block + 50Hz notch(Q=10) + 100Hz notch(Q=10) + FIR-LP
 * -----------------------------------------------------------
 */

#pragma once
#include <Arduino.h>
#include "config.h"
#include "types.h"

class ECGPipeline {
public:
    ECGPipeline();

    void init();
    void resetState();

    // Applies filters in-place to blk.data[]
    void processBlock(Block& blk);

    // Clean SPI dropouts and rail spikes from raw ADC data array
    void cleanRawSpikes(int32_t* data, int len);

    // Runs before processBlock to flag blocks that are pure noise or flatlines.
    static bool validateSamples(const int16_t* samples, int count, String& reason);
    static bool validateSamples(const int32_t* samples, int count, String& reason);

private:
    int32_t _median_prev1;

    // Baseline wander state
    int _blHistoryCount;
    int _blHistoryHead;
    float _blLastAnchor;
    bool _blInitialized;
    float _blStage1History[WINDOW_SIZE];

    // 50 Hz notch state (IIR biquad)
    float _notch_x1, _notch_x2;
    float _notch_y1, _notch_y2;
    float _notch_b0, _notch_b1, _notch_b2;
    float _notch_a1, _notch_a2;

    // 100 Hz notch state (IIR biquad — 2nd power-line harmonic)
    float _notch2_x1, _notch2_x2;
    float _notch2_y1, _notch2_y2;
    float _notch2_b0, _notch2_b1, _notch2_b2;
    float _notch2_a1, _notch2_a2;

    float _workBuf[WINDOW_SIZE];

    // Internal DSP passes
    void _applyMedianSpike(int32_t* data, int len);
    float _medianOfFloats(float* tmp, int n);
    void _removeBaselineWander(float* data, int len);

    void _initNotch50Hz(float fs, float f0, float Q);
    void _initNotch100Hz(float fs, float f0, float Q);
    void _applyIIRNotch(float* data, int len);
    void _applyIIRNotch100(float* data, int len);

    void _applyFIRNotch(float* data, int len);
    void _applyFIRLowPass(float* data, int len);
};
