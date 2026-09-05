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
  if (len <= 0 || !data) return;

  const int32_t ADC_MAX = 8000000;  // ~95% of 24-bit signed max (8,388,607)
  const int32_t ADC_MIN = -8000000; // ~95% of 24-bit signed min
  const int32_t ZERO_THR = 5000;    // |value| < 5000 = considered SPI zero-fill dropout

  // --- Pass 1: Hard clamp to valid 24-bit ADC range ---
  for (int i = 0; i < len; i++) {
    if (data[i] > ADC_MAX) data[i] = ADC_MAX;
    if (data[i] < ADC_MIN) data[i] = ADC_MIN;
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

  // --- Pass 3: True 3-Point Median Filter (Rejects all 1-sample SPI bit flips regardless of amplitude) ---
  int32_t prev = (_median_prev1 != 0) ? _median_prev1 : data[0];
  for (int i = 0; i < len; i++) {
    int32_t cur = data[i];
    int32_t next = (i < len - 1) ? data[i + 1] : cur;

    // Fast median of 3: (prev, cur, next)
    int32_t a = prev, b = cur, c = next;
    if (a > b) { int32_t t = a; a = b; b = t; }
    if (b > c) { int32_t t = b; b = c; c = t; }
    if (a > b) { int32_t t = a; a = b; b = t; }
    
    prev = cur;
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
// Continuous 0.50 Hz High-Pass DC Filter (AHA Standard — Zero ST Distortion)
// -----------------------------------------------------------
static double s_dc_x_prev = 0.0;
static double s_dc_y_prev = 0.0;
static bool s_dc_init = false;

void ECGPipeline::_removeBaselineWander(float *data, int len) {
  if (len <= 0 || !data) return;

  if (!s_dc_init) {
    s_dc_x_prev = (double)data[0];
    s_dc_y_prev = 0.0;
    s_dc_init = true;
  }

  const double R = 0.998948; // ~0.67 Hz cutoff @ 4000 SPS (AHA/IEC Clinical Standard — holds baseline flat between beats)
  for (int i = 0; i < len; i++) {
    double x = (double)data[i];
    double y = x - s_dc_x_prev + R * s_dc_y_prev;
    if (y > 200000.0) y = 200000.0;
    if (y < -200000.0) y = -200000.0;
    s_dc_x_prev = x;
    s_dc_y_prev = y;
    data[i] = (float)y;
  }
}

// -----------------------------------------------------------
// Clinical Zero-Phase 50 Hz Notch Filter (Biquad FiltFilt @ 4000 SPS, Q=6)
// Eliminates 100% of 50 Hz powerline hum with ZERO phase distortion and ZERO ST ripple.
// -----------------------------------------------------------
static void _applyZeroPhaseNotch50Hz(float *data, int len) {
  if (len <= 0 || !data) return;

  const double b0 = 0.993497481341;
  const double b1 = -1.980869720338;
  const double b2 = 0.993497481341;
  const double a1 = -1.980869720338;
  const double a2 = 0.986994962682;

  static float *temp = nullptr;
  if (!temp) {
    temp = (float *)ps_malloc(sizeof(float) * WINDOW_SIZE);
    if (!temp) temp = (float *)malloc(sizeof(float) * WINDOW_SIZE);
  }
  if (!temp) return;

  // Pass 1: Forward Filter
  double x1 = data[0], x2 = data[0];
  double y1 = data[0], y2 = data[0];
  for (int i = 0; i < len; i++) {
    double x = (double)data[i];
    double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    temp[i] = (float)y;
  }

  // Pass 2: Backward Filter (Exact Zero-Phase)
  x1 = temp[len - 1]; x2 = temp[len - 1];
  y1 = temp[len - 1]; y2 = temp[len - 1];
  for (int i = len - 1; i >= 0; i--) {
    double x = (double)temp[i];
    double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    data[i] = (float)y;
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

// 80-tap FIR Comb Notch Filter for 4000 SPS (4000 / 50 = 80 samples) with Inter-Block History
static float s_fir_notch_prev[80];
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
    for (int k = -39; k <= 40; k++) {
      int idx = i + k;
      float val;
      if (idx < 0) {
        val = s_fir_notch_has_prev ? s_fir_notch_prev[80 + idx] : data[0];
      } else if (idx >= len) {
        val = data[len - 1];
      } else {
        val = data[idx];
      }
      ma50 += val;
    }
    ma50 /= 80.0f;
    temp[i] = data[i] - (data[i] - ma50) * 0.98f;
  }

  if (len >= 80) {
    memcpy(s_fir_notch_prev, data + len - 80, 80 * sizeof(float));
    s_fir_notch_has_prev = true;
  }
  memcpy(data, temp, len * sizeof(float));
}

// Zero-Phase FIR Low-Pass (37-tap Gaussian @ 4000 SPS, ~60 Hz cutoff) with Inter-Block History
static float s_fir_lp_prev[37];
static bool s_fir_lp_has_prev = false;

void ECGPipeline::_applyFIRLowPass(float *data, int len) {
  static float *temp = nullptr;
  if (!temp) {
    temp = (float *)ps_malloc(sizeof(float) * WINDOW_SIZE);
    if (!temp) temp = (float *)malloc(sizeof(float) * WINDOW_SIZE);
  }
  if (!temp) return;

  // Pre-computed Gaussian weights for sigma = 12.0 samples @ 4000 SPS (fc ≈ 60 Hz)
  static const float gWeights[19] = {
      1.000000f, 0.996534f, 0.986211f, 0.969233f, 0.945959f, 0.916891f,
      0.882497f, 0.843332f, 0.800000f, 0.753175f, 0.703511f, 0.651680f,
      0.598375f, 0.544283f, 0.490074f, 0.436384f, 0.383794f, 0.332837f,
      0.283997f};
  static const float totalWeight = 27.13470f;

  for (int i = 0; i < len; i++) {
    float sum = data[i] * gWeights[0];
    for (int k = 1; k <= 18; k++) {
      int idxL = i - k;
      int idxR = i + k;
      float valL = (idxL < 0) ? (s_fir_lp_has_prev ? s_fir_lp_prev[37 + idxL] : data[0])
                              : data[idxL];
      float valR = (idxR >= len) ? data[len - 1] : data[idxR];
      sum += (valL + valR) * gWeights[k];
    }
    temp[i] = sum / totalWeight;
  }

  if (len >= 37) {
    memcpy(s_fir_lp_prev, data + len - 37, 37 * sizeof(float));
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

// -----------------------------------------------------------
// Clinical 24 Hz Zero-Phase Low-Pass Filter (Butterworth 2nd-order FiltFilt @ 4000 SPS)
// Eliminates 50Hz/100Hz powerline hum, muscle tremor, and uneven baseline bumps with ZERO phase distortion.
// -----------------------------------------------------------
static void _applyZeroPhaseLowPass24Hz(float *data, int len) {
  if (len <= 0 || !data) return;

  const double b0 = 0.0003460431;
  const double b1 = 0.0006920862;
  const double b2 = 0.0003460431;
  const double a1 = -1.9466975439;
  const double a2 = 0.9480817163;

  static float *temp = nullptr;
  if (!temp) {
    temp = (float *)ps_malloc(sizeof(float) * WINDOW_SIZE);
    if (!temp) temp = (float *)malloc(sizeof(float) * WINDOW_SIZE);
  }
  if (!temp) return;

  // Pass 1: Forward Filter
  double x1 = data[0], x2 = data[0];
  double y1 = data[0], y2 = data[0];
  for (int i = 0; i < len; i++) {
    double x = (double)data[i];
    double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    temp[i] = (float)y;
  }

  // Pass 2: Backward Filter (reverses phase shift -> EXACT zero phase distortion)
  x1 = temp[len - 1]; x2 = temp[len - 1];
  y1 = temp[len - 1]; y2 = temp[len - 1];
  for (int i = len - 1; i >= 0; i--) {
    double x = (double)temp[i];
    double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    if (y > 200000.0) y = 200000.0;
    if (y < -200000.0) y = -200000.0;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    data[i] = (float)y;
  }
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

  // Step 1: Clean raw spikes on both raw ADC and filtered buffers
  _applyMedianSpike(blk.data, WINDOW_SIZE);
  _applyMedianSpike(blk.filtered_data, WINDOW_SIZE);

  // Step 2: Float conversion
  for (int i = 0; i < WINDOW_SIZE; i++)
    _workBuf[i] = (float)blk.filtered_data[i];

  // Step 3: Continuous 0.67 Hz High-pass DC filter (AHA/IEC Standard — Zero ST distortion, holds isoelectric flat)
  _removeBaselineWander(_workBuf, WINDOW_SIZE);

  // Step 4: Clinical Zero-Phase 50 Hz Notch Filter (Q=6) — completely removes 50 Hz mains hum
  _applyZeroPhaseNotch50Hz(_workBuf, WINDOW_SIZE);

  // Step 5: Clinical Zero-Phase 24 Hz Low-Pass Filter (eliminates EMG muscle tremor and uneven bumps)
  _applyZeroPhaseLowPass24Hz(_workBuf, WINDOW_SIZE);

  // Step 6: Zero-Phase Gaussian FIR Smoother (eradicates micro-ripples while preserving crisp R-peaks)
  _applyFIRLowPass(_workBuf, WINDOW_SIZE);

  // Step 7: Write back to int32 with Polarity Correction & NaN Protection
  for (int i = 0; i < WINDOW_SIZE; i++) {
    float v = _workBuf[i];
    if (isnan(v) || isinf(v)) v = 0.0f;
#if ECG_INVERT_CH2
    blk.filtered_data[i] = -(int32_t)roundf(v);
#else
    blk.filtered_data[i] = (int32_t)roundf(v);
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

