/**
 * ecg_pipeline.cpp  — v2.0 for 500 SPS
 * -----------------------------------------------------------
 * Full DSP pipeline for ADS1292R @ 500 SPS:
 * 1. Median-of-3 spike filter (glitch rejection)
 * 2. 0.1 Hz IIR high-pass (DC block / baseline wander removal)
 * 3. 50 Hz IIR biquad notch (Q=10, narrow surgical notch)
 * 4. 100 Hz IIR biquad notch (Q=10, 2nd power-line harmonic)
 * 5. 3-tap FIR low-pass (~83 Hz cutoff @ 500 SPS)
 * -----------------------------------------------------------
 */

#include "ecg_pipeline.h"
#include <math.h>

#if RAW_DATA_ONLY

ECGPipeline::ECGPipeline() {}
void ECGPipeline::init() {
  Serial.println(
      F("[PIPELINE] RAW_DATA_ONLY mode active — DSP filters bypassed."));
}
void ECGPipeline::resetState() {}
bool ECGPipeline::validateSamples(const int32_t *samples, int count,
                                  String &reason) {
  reason = "OK";
  return false;
}
void ECGPipeline::processBlock(Block &blk) {}

#else

ECGPipeline::ECGPipeline() { resetState(); }

void ECGPipeline::init() {
  resetState();
  Serial.println(F("[PIPELINE] DSP filter pipeline initialized."));
}

void ECGPipeline::resetState() {
  _median_prev1 = 0;
  _blHistoryCount = 0;
  _blHistoryHead = 0;
  _blLastAnchor = 0.0f;
  _blInitialized = false;

  _notch_x1 = 0.0f;
  _notch_x2 = 0.0f;
  _notch_y1 = 0.0f;
  _notch_y2 = 0.0f;
  _notch2_x1 = 0.0f;
  _notch2_x2 = 0.0f;
  _notch2_y1 = 0.0f;
  _notch2_y2 = 0.0f;
}

// ==========================================================
// ADS1292R Artifact Rejection + Median-of-3 Spike Filter
// ==========================================================
// Pass 1: Clamp to valid 24-bit signed ADC range
// Pass 2: Replace SPI zero-fill drop-out runs (consecutive near-zero samples)
// Pass 3: Median-of-3 to reject residual single-sample spikes
// ==========================================================
void ECGPipeline::cleanRawSpikes(int32_t *data, int len) {
  _applyMedianSpike(data, len);
}

void ECGPipeline::_applyMedianSpike(int32_t *data, int len) {
  const int32_t ADC_MAX = 8000000;  // ~95% of 24-bit signed max (8,388,607)
  const int32_t ADC_MIN = -8000000; // ~95% of 24-bit signed min
  const int32_t ZERO_THR = 5000;    // |value| < 5000 = considered SPI zero-fill dropout

  // --- Pass 1: Hard clamp to valid 24-bit ADC range ---
  for (int i = 0; i < len; i++) {
    if (data[i] > ADC_MAX)
      data[i] = ADC_MAX;
    if (data[i] < ADC_MIN)
      data[i] = ADC_MIN;
  }

  // --- Pass 2: Detect and repair SPI zero-fill dropout runs ---
  int i = 0;
  while (i < len) {
    if (abs(data[i]) < ZERO_THR) {
      int runStart = i;
      while (i < len && abs(data[i]) < ZERO_THR)
        i++;
      int runEnd = i;
      int32_t leftVal = (runStart > 0) ? data[runStart - 1] : ((runEnd < len) ? data[runEnd] : 0);
      int32_t rightVal = (runEnd < len) ? data[runEnd] : leftVal;
      int runLen = runEnd - runStart;
      for (int j = 0; j < runLen; j++) {
        data[runStart + j] = leftVal + (int32_t)((float)(rightVal - leftVal) *
                                                 (j + 1) / (runLen + 1));
      }
    } else {
      i++;
    }
  }

  // --- Pass 3: Median-of-3 single-sample spike rejection ---
  int32_t prev = (_median_prev1 != 0) ? _median_prev1 : data[0];
  for (int i = 0; i < len; i++) {
    int32_t cur = data[i];
    int32_t next = (i < len - 1) ? data[i + 1] : cur;

    // Reject isolated spike: both neighbours differ by > 25k counts (physical slew rate limit)
    if (i > 0 && i < len - 1) {
      int32_t d1 = abs(cur - prev);
      int32_t d2 = abs(cur - next);
      if (d1 > 25000 && d2 > 25000) {
        cur = (prev + next) / 2;
      }
    }

    int32_t a = prev, b = cur, c = next;
    if (a > b) { int32_t t = a; a = b; b = t; }
    if (b > c) { int32_t t = b; b = c; c = t; }
    if (a > b) { int32_t t = a; a = b; b = t; }
    prev = data[i];
    data[i] = b;
  }
  _median_prev1 = prev;
}

// ==========================================================
// Median helper
// ==========================================================
float ECGPipeline::_medianOfFloats(float *tmp, int n) {
  for (int i = 1; i < n; i++) {
    float key = tmp[i];
    int j = i - 1;
    while (j >= 0 && tmp[j] > key) {
      tmp[j + 1] = tmp[j];
      j--;
    }
    tmp[j + 1] = key;
  }
  return (n % 2 == 1) ? tmp[n / 2] : 0.5f * (tmp[n / 2 - 1] + tmp[n / 2]);
}

// ==========================================================
// -----------------------------------------------------------
// Robust 1st-Order DC Blocker Filter (~0.48 Hz cutoff @ 4000 SPS)
// -----------------------------------------------------------
static float s_dc_x_prev = 0.0f;
static float s_dc_y_prev = 0.0f;
static bool s_dc_init = false;

void ECGPipeline::_removeBaselineWander(float *data, int len) {
  if (!s_dc_init && len > 0) {
    s_dc_x_prev = data[0];
    s_dc_y_prev = 0.0f;
    s_dc_init = true;
  }
  const float R = 0.99925f; // ~0.48 Hz cutoff @ 4000 SPS (AHA clinical standard)
  for (int i = 0; i < len; i++) {
    float x = data[i];
    float y = x - s_dc_x_prev + R * s_dc_y_prev;
    s_dc_x_prev = x;
    s_dc_y_prev = y;
    data[i] = y;
  }
}

// ==========================================================
// Surgical 50Hz Notch (Q=8 Biquad) + 100Hz Harmonic Notch
// ==========================================================
void ECGPipeline::_initNotch50Hz(float fs, float f0, float Q) {
  float w0 = 2.0f * M_PI * (f0 / fs);
  float alpha = sinf(w0) / (2.0f * Q);
  float a0 = 1.0f + alpha;
  _notch_b0 = 1.0f / a0;
  _notch_b1 = -2.0f * cosf(w0) / a0;
  _notch_b2 = 1.0f / a0;
  _notch_a1 = -2.0f * cosf(w0) / a0;
  _notch_a2 = (1.0f - alpha) / a0;
}

void ECGPipeline::_initNotch100Hz(float fs, float f0, float Q) {
  float w0 = 2.0f * M_PI * (f0 / fs);
  float alpha = sinf(w0) / (2.0f * Q);
  float a0 = 1.0f + alpha;
  _notch2_b0 = 1.0f / a0;
  _notch2_b1 = -2.0f * cosf(w0) / a0;
  _notch2_b2 = 1.0f / a0;
  _notch2_a1 = -2.0f * cosf(w0) / a0;
  _notch2_a2 = (1.0f - alpha) / a0;
}

void ECGPipeline::_applyIIRNotch(float *data, int len) {
  for (int i = 0; i < len; i++) {
    float x = data[i];
    float y = _notch_b0 * x + _notch_b1 * _notch_x1 + _notch_b2 * _notch_x2 -
              _notch_a1 * _notch_y1 - _notch_a2 * _notch_y2;
    _notch_x2 = _notch_x1;
    _notch_x1 = x;
    _notch_y2 = _notch_y1;
    _notch_y1 = y;
    data[i] = y;
  }
}

void ECGPipeline::_applyIIRNotch100(float *data, int len) {
  for (int i = 0; i < len; i++) {
    float x = data[i];
    float y = _notch2_b0 * x + _notch2_b1 * _notch2_x1 +
              _notch2_b2 * _notch2_x2 - _notch2_a1 * _notch2_y1 -
              _notch2_a2 * _notch2_y2;
    _notch2_x2 = _notch2_x1;
    _notch2_x1 = x;
    _notch2_y2 = _notch2_y1;
    _notch2_y1 = y;
    data[i] = y;
  }
}

// 40-tap FIR Comb Notch Filter with Inter-Block History
static float s_fir_notch_prev[40];
static bool s_fir_notch_has_prev = false;

void ECGPipeline::_applyFIRNotch(float *data, int len) {
  static float *temp = nullptr;
  if (!temp) {
    temp = (float *)ps_malloc(sizeof(float) * WINDOW_SIZE);
    if (!temp) temp = (float *)malloc(sizeof(float) * WINDOW_SIZE);
  }
  if (!temp) return;
  for (int i = 0; i < len; i++) {
    float ma50 = 0.0f;
    for (int k = -19; k <= 20; k++) {
      int idx = i + k;
      float val;
      if (idx < 0) {
        val = s_fir_notch_has_prev ? s_fir_notch_prev[40 + idx] : data[0];
      } else if (idx >= len) {
        val = data[len - 1];
      } else {
        val = data[idx];
      }
      ma50 += val;
    }
    ma50 /= 40.0f;
    temp[i] = data[i] - (data[i] - ma50) * 0.95f;
  }
  if (len >= 40) {
    memcpy(s_fir_notch_prev, data + len - 40, 40 * sizeof(float));
    s_fir_notch_has_prev = true;
  }
  memcpy(data, temp, len * sizeof(float));
}

// Zero-Phase FIR Low-Pass (9-tap Gaussian @ 4000 SPS, ~60 Hz cutoff) with Inter-Block History
static float s_fir_lp_prev[10];
static bool s_fir_lp_has_prev = false;

void ECGPipeline::_applyFIRLowPass(float *data, int len) {
  static float *temp = nullptr;
  if (!temp) {
    temp = (float *)ps_malloc(sizeof(float) * WINDOW_SIZE);
    if (!temp) temp = (float *)malloc(sizeof(float) * WINDOW_SIZE);
  }
  if (!temp) return;
  for (int i = 0; i < len; i++) {
    float sum = 0.0f;
    float weightSum = 0.0f;
    for (int k = -4; k <= 4; k++) {
      int idx = i + k;
      float val;
      if (idx < 0) {
        val = s_fir_lp_has_prev ? s_fir_lp_prev[10 + idx] : data[0];
      } else if (idx >= len) {
        val = data[len - 1];
      } else {
        val = data[idx];
      }
      float w = expf(-0.5f * (k * k) / (2.2f * 2.2f));
      sum += val * w;
      weightSum += w;
    }
    temp[i] = sum / weightSum;
  }
  if (len >= 10) {
    memcpy(s_fir_lp_prev, data + len - 10, 10 * sizeof(float));
    s_fir_lp_has_prev = true;
  }
  memcpy(data, temp, len * sizeof(float));
}

// 11-point 3rd-Order Savitzky-Golay FIR Smoother with Inter-Block History
static float s_savgol_prev[12];
static bool s_savgol_has_prev = false;

static void _applySavitzkyGolayFirmware(float *data, int len) {
  static float *temp = nullptr;
  if (!temp) {
    temp = (float *)ps_malloc(sizeof(float) * WINDOW_SIZE);
    if (!temp) temp = (float *)malloc(sizeof(float) * WINDOW_SIZE);
  }
  if (!temp) return;
  static const float coeffs[11] = {-36.0f, 9.0f,  44.0f, 69.0f, 84.0f, 89.0f,
                                   84.0f,  69.0f, 44.0f, 9.0f,  -36.0f};
  for (int i = 0; i < len; i++) {
    float sum = 0.0f;
    for (int k = -5; k <= 5; k++) {
      int idx = i + k;
      float val;
      if (idx < 0) {
        val = s_savgol_has_prev ? s_savgol_prev[12 + idx] : data[0];
      } else if (idx >= len) {
        val = data[len - 1];
      } else {
        val = data[idx];
      }
      sum += val * coeffs[k + 5];
    }
    temp[i] = sum / 429.0f;
  }
  if (len >= 12) {
    memcpy(s_savgol_prev, data + len - 12, 12 * sizeof(float));
    s_savgol_has_prev = true;
  }
  memcpy(data, temp, len * sizeof(float));
}

// ==========================================================
// processBlock()
// ==========================================================
void ECGPipeline::processBlock(Block &blk) {
  if (blk.lo) {
    resetState();
    for (int i = 0; i < WINDOW_SIZE; i++)
      blk.filtered_data[i] = 0;
    return;
  }

  // Step 1: Clamp to valid ADC range + SPI zero-fill repair + median-of-3 spike
  // rejection
  _applyMedianSpike(blk.filtered_data, WINDOW_SIZE);

  // Step 2: Float conversion
  for (int i = 0; i < WINDOW_SIZE; i++)
    _workBuf[i] = (float)blk.filtered_data[i];

  // Step 3: 0.5 Hz High-pass DC block — removes electrode DC offset + motion
  // baseline drift
  _removeBaselineWander(_workBuf, WINDOW_SIZE);

  // Step 4: Zero-Phase FIR 50 Hz & 100 Hz Comb Notch Filter (40-tap)
  _applyFIRNotch(_workBuf, WINDOW_SIZE);

  // Step 5: Zero-Phase FIR Low-Pass (~60 Hz cutoff @ 4000 SPS)
  _applyFIRLowPass(_workBuf, WINDOW_SIZE);

  // Step 6: 11-point Savitzky-Golay Polynomial Smoother
  _applySavitzkyGolayFirmware(_workBuf, WINDOW_SIZE);

  // Step 7: Write back to int32 with Polarity Correction
  for (int i = 0; i < WINDOW_SIZE; i++) {
#if ECG_INVERT_CH2
    blk.filtered_data[i] = -(int32_t)roundf(_workBuf[i]);
#else
    blk.filtered_data[i] = (int32_t)roundf(_workBuf[i]);
#endif
  }
}

// ==========================================================
// validateSamples()
// ==========================================================
bool ECGPipeline::validateSamples(const int16_t *samples, int count,
                                  String &reason) {
  reason = "";
  int16_t minV = INT16_MAX, maxV = INT16_MIN;
  uint32_t zeroCount = 0, spikeCount = 0;

  for (int i = 0; i < count; i++) {
    int16_t v = samples[i];
    if (v == 0)
      zeroCount++;
    if (v < minV)
      minV = v;
    if (v > maxV)
      maxV = v;
    if (i > 0) {
      int32_t delta = (int32_t)v - (int32_t)samples[i - 1];
      if (delta < 0)
        delta = -delta;
      if (delta > VALIDATE_SPIKE_DELTA_MAX)
        spikeCount++;
    }
  }

  float zeroRatio = (float)zeroCount / (float)count;
  float spikeRatio = (float)spikeCount / (float)count;

  if (zeroRatio > 0.15f) {
    reason = "TOO_MANY_ZERO_SAMPLES=" + String(zeroRatio, 2);
  } else if (spikeRatio > VALIDATE_NOISE_RATIO_MAX) {
    reason = "NOISY_SIGNAL=" + String(spikeRatio, 2);
  }
  return reason.length() > 0;
}

bool ECGPipeline::validateSamples(const int32_t *samples, int count,
                                  String &reason) {
  reason = "";
  int32_t minV = INT32_MAX, maxV = INT32_MIN;
  uint32_t zeroCount = 0, spikeCount = 0;

  for (int i = 0; i < count; i++) {
    int32_t v = samples[i];
    if (v == 0)
      zeroCount++;
    if (v < minV)
      minV = v;
    if (v > maxV)
      maxV = v;
    if (i > 0) {
      int64_t delta = (int64_t)v - (int64_t)samples[i - 1];
      if (delta < 0)
        delta = -delta;
      if (delta > VALIDATE_SPIKE_DELTA_MAX)
        spikeCount++;
    }
  }

  float zeroRatio = (float)zeroCount / (float)count;
  float spikeRatio = (float)spikeCount / (float)count;

  if (zeroRatio > 0.15f) {
    reason = "TOO_MANY_ZERO_SAMPLES=" + String(zeroRatio, 2);
  } else if (spikeRatio > VALIDATE_NOISE_RATIO_MAX) {
    reason = "NOISY_SIGNAL=" + String(spikeRatio, 2);
  }
  return reason.length() > 0;
}

#endif

