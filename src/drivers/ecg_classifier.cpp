/**
 * ecg_classifier.cpp — ECG Diagnostic Classification
 * -----------------------------------------------------------
 * Migrated from ecg_esp32_ads1292r_v10_working_1.ino.
 * Contains: classifyWindow, detectPeaks, estimateQrsWidth,
 *           findQrsBoundaries, measurePWave, measureTWave,
 *           and supporting helpers.
 * -----------------------------------------------------------
 */

#include "ecg_classifier.h"
#include "ecg_pipeline.h"
#include <math.h>

// Scratch buffer used by detectPeaks() — allocated from PSRAM (40 KB)
static float *analysisCentered = nullptr;

// ==========================================================
// Validation helper text mappers
// ==========================================================

String validationConditionName(const String& reason) {
  if (reason.startsWith("LEADS_OFF")) return "Lead-Off Detected";
  return "Poor signal quality";
}

String validationDetailText(const String& reason) {
  if (reason.startsWith("LEADS_OFF")) return "Check electrode placement or connection";
  if (reason.startsWith("TOO_MANY_ZERO_SAMPLES=")) {
    String ratio = reason.substring(String("TOO_MANY_ZERO_SAMPLES=").length());
    return "zero ratio=" + ratio + " (threshold 15%)";
  }
  if (reason.startsWith("CLIPPED_SIGNAL=")) {
    String ratio = reason.substring(String("CLIPPED_SIGNAL=").length());
    return "clipping ratio=" + ratio + " (threshold 2%)";
  }
  if (reason.startsWith("LOW_AMPLITUDE=")) {
    String ptp = reason.substring(String("LOW_AMPLITUDE=").length());
    return "ptp=" + ptp + " (threshold 80)";
  }
  if (reason.startsWith("NOISY_SIGNAL=")) {
    String ratio = reason.substring(String("NOISY_SIGNAL=").length());
    return "noise ratio=" + ratio + " (threshold 12.5%)";
  }
  return "Check electrodes";
}

// ==========================================================
// detectPeaks — polarity-agnostic R-peak finder
// ==========================================================
int detectPeaks(const int32_t* samples, int sampleCount, int* peaks, int maxPeaks, float& maxAbs) {
  if (!analysisCentered) {
    analysisCentered = (float *)ps_malloc(sizeof(float) * ANALYSIS_WINDOW_SIZE);
    if (!analysisCentered) analysisCentered = (float *)malloc(sizeof(float) * ANALYSIS_WINDOW_SIZE);
  }

  long sum = 0;
  for (int i = 0; i < sampleCount; i++) {
    sum += samples[i];
  }

  float mean = (float)sum / (float)sampleCount;
  maxAbs = 0.0f;
  for (int i = 0; i < sampleCount; i++) {
    if (analysisCentered) analysisCentered[i] = (float)samples[i] - mean;
    float absVal = fabs((float)samples[i] - mean);
    if (absVal > maxAbs) maxAbs = absVal;
  }

  if (maxAbs < 30.0f) return 0;
  float threshold = maxAbs * 0.50f;
  int peakCount = 0;
  int minDistance = SAMPLE_RATE / 4;
  int lastPeak = -minDistance;

  // Polarity-agnostic: QRS direction depends on lead vector vs. an
  // individual's cardiac axis, so the same electrode placement can be
  // upright on one person and inverted on another (both are normal).
  for (int i = 1; i < sampleCount - 1; i++) {
    float v = analysisCentered[i];
    float av = fabs(v);
    if (av < threshold) continue;

    bool isLocalExtreme = (v > 0.0f)
      ? (v >= analysisCentered[i - 1] && v > analysisCentered[i + 1])
      : (v <= analysisCentered[i - 1] && v < analysisCentered[i + 1]);
    if (!isLocalExtreme) continue;
    if ((i - lastPeak) < minDistance) continue;

    if (peakCount < maxPeaks) {
      peaks[peakCount++] = i;
      lastPeak = i;
    }
  }

  return peakCount;
}

// ==========================================================
// estimateQrsWidth — half-amplitude crossing method
// ==========================================================
float estimateQrsWidth(const int32_t* samples, int sampleCount, int rIndex) {
  int searchRadius = (int)(0.07f * SAMPLE_RATE);
  int start = max(0, rIndex - searchRadius);
  int end = min(sampleCount - 1, rIndex + searchRadius);
  if (end <= start) return 90.0f;

  float localMax = 0.0f;
  float localMin = 1e9f;
  for (int i = start; i <= end; i++) {
    float v = (float)samples[i];
    if (v > localMax) localMax = v;
    if (v < localMin) localMin = v;
  }

  float halfLevel = localMin + 0.50f * (localMax - localMin);
  bool negativePeak = ((float)samples[rIndex] < halfLevel);

  int left = rIndex;
  int right = rIndex;
  if (!negativePeak) {
    while (left > start && (float)samples[left] > halfLevel) left--;
    while (right < end && (float)samples[right] > halfLevel) right++;
  } else {
    while (left > start && (float)samples[left] < halfLevel) left--;
    while (right < end && (float)samples[right] < halfLevel) right++;
  }

  float widthSamples = (float)max(1, right - left);
  return (widthSamples / (float)SAMPLE_RATE) * 1000.0f;
}

// ==========================================================
// findQrsBoundaries — onset/offset sample indices
// ==========================================================
void findQrsBoundaries(const int32_t* samples, int sampleCount, int rIndex, int& qOnsetIdx, int& sOffsetIdx) {
  int searchRadius = (int)(0.07f * SAMPLE_RATE);
  int start = max(0, rIndex - searchRadius);
  int end = min(sampleCount - 1, rIndex + searchRadius);
  qOnsetIdx = start;
  sOffsetIdx = end;
  if (end <= start) return;

  float localMax = 0.0f, localMin = 1e9f;
  for (int i = start; i <= end; i++) {
    float v = (float)samples[i];
    if (v > localMax) localMax = v;
    if (v < localMin) localMin = v;
  }
  float halfLevel = localMin + 0.50f * (localMax - localMin);
  bool negativePeak = ((float)samples[rIndex] < halfLevel);

  int left = rIndex;
  int right = rIndex;
  if (!negativePeak) {
    while (left > start && (float)samples[left] > halfLevel) left--;
    while (right < end && (float)samples[right] > halfLevel) right++;
  } else {
    while (left > start && (float)samples[left] < halfLevel) left--;
    while (right < end && (float)samples[right] < halfLevel) right++;
  }

  qOnsetIdx = left;
  sOffsetIdx = right;
}

// ==========================================================
// Local averaging helper
// ==========================================================
static float localAverage(const int32_t* samples, int sampleCount, int idx, int halfWin) {
  int lo = max(0, idx - halfWin);
  int hi = min(sampleCount - 1, idx + halfWin);
  float sum = 0.0f; int n = 0;
  for (int i = lo; i <= hi; i++) { sum += (float)samples[i]; n++; }
  return sum / (float)n;
}

// ==========================================================
// measureIsoBaseline — TP segment reference
// ==========================================================
static float measureIsoBaseline(const int32_t* samples, int sampleCount, int rIndex) {
  int idx = max(0, rIndex - (int)(0.28f * SAMPLE_RATE));
  return localAverage(samples, sampleCount, idx, 2);
}

// ==========================================================
// measurePWave — TP segment before QRS onset
// ==========================================================
WaveMeasurement measurePWave(const int32_t* samples, int sampleCount, int rIndex, int qOnsetIdx) {
  WaveMeasurement m = {false, 0.0f, 0.0f};

  int winStart = max(0, rIndex - (int)(0.28f * SAMPLE_RATE));
  int winEnd   = qOnsetIdx - (int)(0.02f * SAMPLE_RATE);
  if (winEnd <= winStart || winStart >= sampleCount) return m;

  float iso = localAverage(samples, sampleCount, winStart, 2);
  float peakVal = iso;
  float maxDev = 0.0f;
  int peakIdx = -1;
  for (int i = winStart; i <= winEnd && i < sampleCount; i++) {
    float v = (float)samples[i];
    float dev = v - iso;
    if (fabs(dev) > maxDev) { maxDev = fabs(dev); peakVal = v; peakIdx = i; }
  }
  if (peakIdx < 0) return m;

  m.valid = true;
  m.amplitude = peakVal - iso;
  m.timeMsFromR = ((float)(peakIdx - rIndex) / (float)SAMPLE_RATE) * 1000.0f;
  return m;
}

// ==========================================================
// measureTWave — ST segment after QRS offset
// ==========================================================
WaveMeasurement measureTWave(const int32_t* samples, int sampleCount, int rIndex, int sOffsetIdx, float baselineIso) {
  WaveMeasurement m = {false, 0.0f, 0.0f};

  int winStart = sOffsetIdx + (int)(0.06f * SAMPLE_RATE);
  int winEnd   = min(sampleCount - 1, rIndex + (int)(0.44f * SAMPLE_RATE));
  if (winEnd <= winStart) return m;

  float iso = baselineIso;
  float peakVal = iso;
  float maxDev = 0.0f;
  int peakIdx = -1;
  for (int i = winStart; i <= winEnd; i++) {
    float v = (float)samples[i];
    float dev = v - iso;
    if (fabs(dev) > maxDev) { maxDev = fabs(dev); peakVal = v; peakIdx = i; }
  }
  if (peakIdx < 0) return m;

  m.valid = true;
  m.amplitude = peakVal - iso;
  m.timeMsFromR = ((float)(peakIdx - rIndex) / (float)SAMPLE_RATE) * 1000.0f;
  return m;
}

// ==========================================================
// classifyWindow — main diagnostic classifier
// ==========================================================
String classifyWindow(const int32_t* samples, int sampleCount, String& severity, String& detail) {
  severity = "";
  detail = "";

  String reason;
  if (ECGPipeline::validateSamples(samples, sampleCount, reason)) {
    severity = reason.startsWith("LEADS_OFF") ? "CRITICAL" : "INFO";
    detail = validationDetailText(reason);
    return validationConditionName(reason);
  }

  int peaks[32];
  float maxAbs = 0.0f;
  int peakCount = detectPeaks(samples, sampleCount, peaks, 32, maxAbs);

  if (peakCount < 4) {
    severity = "INFO";
    detail = "Need more data";
    return "Insufficient beats";
  }

  float rrIntervals[31];
  int rrCount = 0;
  float rrSum = 0.0f;
  float rrSqSum = 0.0f;
  float maxGapMs = 0.0f;

  for (int i = 1; i < peakCount; i++) {
    float gapSamples = (float)(peaks[i] - peaks[i - 1]);
    float gapMs = (gapSamples / (float)SAMPLE_RATE) * 1000.0f;
    rrIntervals[rrCount++] = gapMs;
    rrSum += gapSamples;
    rrSqSum += gapSamples * gapSamples;
    if (gapMs > maxGapMs) maxGapMs = gapMs;
  }

  float meanSamples = rrSum / (float)rrCount;
  if (meanSamples <= 0.0f) {
    severity = "CRITICAL";
    detail = "No detectable beats";
    return "Asystole";
  }

  float bpm = 60.0f * (float)SAMPLE_RATE / meanSamples;
  float rrVariance = (rrSqSum / (float)rrCount) - (meanSamples * meanSamples);
  if (rrVariance < 0.0f) rrVariance = 0.0f;
  float cv = sqrt(rrVariance) / (meanSamples + 1e-9f);

  float sdSum = 0.0f;
  int sdCount = 0;
  float midMean = 0.0f;
  
  for (int i = 0; i < rrCount; i++) midMean += rrIntervals[i];
  midMean /= (float)rrCount;
  
  for (int i = 1; i < rrCount; i++) {
    float diff = rrIntervals[i] - rrIntervals[i - 1];
    sdSum += diff * diff;
    sdCount++;
  }
  
  float rmssd = sdCount > 0 ? sqrt(sdSum / (float)sdCount) : 0.0f;
  int over50 = 0;
  for (int i = 1; i < rrCount; i++) {
    if (fabs(rrIntervals[i] - rrIntervals[i - 1]) > 50.0f) over50++;
  }
  float pnn50 = sdCount > 0 ? (float)over50 / (float)sdCount : 0.0f;

  float widths[32];
  int widthCount = 0;
  float pAmpSum = 0.0f, pTimeSum = 0.0f;
  float tAmpSum = 0.0f, tTimeSum = 0.0f;
  float prIntervalSum = 0.0f, qtIntervalSum = 0.0f;
  int pCount = 0, tCount = 0, prCount = 0, qtCount = 0;

  for (int i = 0; i < peakCount && widthCount < 32; i++) {
    float w = estimateQrsWidth(samples, sampleCount, peaks[i]);
    if (w >= 40.0f && w <= 200.0f) {
      widths[widthCount++] = w;
    }

    int qOnsetIdx, sOffsetIdx;
    findQrsBoundaries(samples, sampleCount, peaks[i], qOnsetIdx, sOffsetIdx);
    float isoBaseline = measureIsoBaseline(samples, sampleCount, peaks[i]);

    WaveMeasurement pWave = measurePWave(samples, sampleCount, peaks[i], qOnsetIdx);
    if (pWave.valid) {
      pAmpSum += pWave.amplitude;
      pTimeSum += pWave.timeMsFromR;
      pCount++;
      float prMs = ((float)(qOnsetIdx - peaks[i]) / (float)SAMPLE_RATE) * 1000.0f - pWave.timeMsFromR;
      if (prMs > 80.0f && prMs < 320.0f) {
        prIntervalSum += prMs;
        prCount++;
      }
    }

    WaveMeasurement tWave = measureTWave(samples, sampleCount, peaks[i], sOffsetIdx, isoBaseline);
    if (tWave.valid) {
      tAmpSum += tWave.amplitude;
      tTimeSum += tWave.timeMsFromR;
      tCount++;
      float qtMs = tWave.timeMsFromR - (((float)(qOnsetIdx - peaks[i]) / (float)SAMPLE_RATE) * 1000.0f);
      if (qtMs > 200.0f && qtMs < 600.0f) {
        qtIntervalSum += qtMs;
        qtCount++;
      }
    }
  }

  if (pCount > 0 || tCount > 0) {
    String pStr = pCount > 0
      ? "amp=" + String(pAmpSum / pCount, 0) + "cts t=" + String(pTimeSum / pCount, 0) + "ms"
      : "not detected";
    String tStr = tCount > 0
      ? "amp=" + String(tAmpSum / tCount, 0) + "cts t=" + String(tTimeSum / tCount, 0) + "ms"
      : "not detected";
    String prStr = prCount > 0 ? String(prIntervalSum / prCount, 0) + "ms" : "n/a";
    String qtStr = qtCount > 0 ? String(qtIntervalSum / qtCount, 0) + "ms" : "n/a";
    Serial.println("[PQRST] P: " + pStr + " (" + String(pCount) + "/" + String(peakCount) + " beats) | "
                    + "T: " + tStr + " (" + String(tCount) + "/" + String(peakCount) + " beats) | "
                    + "PR=" + prStr + " QT=" + qtStr);
  }

  float qrsWidth = widthCount > 0 ? widths[0] : 90.0f;
  for (int i = 1; i < widthCount; i++) qrsWidth += widths[i];
  if (widthCount > 0) qrsWidth /= (float)widthCount;
  
  float pctWide = 0.0f;
  if (widthCount > 0) {
    int wideCount = 0;
    for (int i = 0; i < widthCount; i++) {
      if (widths[i] > 130.0f) wideCount++;
    }
    pctWide = (float)wideCount / (float)widthCount;
  }

  float meanSample = 0.0f;
  for (int i = 0; i < sampleCount; i++) meanSample += (float)samples[i];
  meanSample /= (float)sampleCount;
  
  float ampSum = 0.0f;
  float ampSqSum = 0.0f;
  int ampCount = 0;
  for (int i = 0; i < peakCount; i++) {
    int idx = peaks[i];
    if (idx >= 0 && idx < sampleCount) {
      float amp = fabs((float)samples[idx] - meanSample);
      ampSum += amp;
      ampSqSum += amp * amp;
      ampCount++;
    }
  }
  
  float avcv = 0.0f;
  if (ampCount > 1 && ampSum > 0.0f) {
    float ampMean = ampSum / (float)ampCount;
    float ampVar = (ampSqSum / (float)ampCount) - (ampMean * ampMean);
    if (ampVar < 0.0f) ampVar = 0.0f;
    avcv = sqrt(ampVar) / (ampMean + 1e-9f);
  }

  // ---- Classification rules (ordered by severity) ----

  if (bpm < 10.0f) {
    severity = "CRITICAL";
    detail = "No detectable beats";
    return "Asystole";
  }

  if (maxGapMs > 3000.0f) {
    severity = "CRITICAL";
    detail = "Gap=" + String(maxGapMs, 0) + "ms";
    return "Sinus Arrest";
  }

  if (bpm > 100.0f && pctWide > 0.5f && cv < 0.15f && peakCount >= 6) {
    severity = "CRITICAL";
    detail = "Wide QRS=" + String(qrsWidth, 0) + "ms, HR=" + String(bpm, 0) + "bpm";
    return "Ventricular Tachycardia";
  }

  if (maxGapMs > 2.2f * midMean && maxGapMs > 2000.0f) {
    severity = "WARNING";
    detail = "Gap=" + String(maxGapMs, 0) + "ms";
    return "Sinus Pause";
  }

  if (cv > 0.15f && rmssd > 75.0f && pnn50 > 0.32f && avcv < 0.10f) {
    severity = "WARNING";
    detail = "CV=" + String(cv, 3) + ", RMSSD=" + String(rmssd, 0) + "ms";
    return "Atrial Fibrillation";
  }

  if (bpm >= 150.0f && bpm <= 250.0f && pctWide < 0.3f && cv < 0.05f) {
    severity = "WARNING";
    detail = "HR=" + String(bpm, 0) + "bpm";
    return "SVT";
  }

  if (bpm > 100.0f && cv < 0.12f && pctWide < 0.4f) {
    severity = "WARNING";
    detail = "HR=" + String(bpm, 0) + "bpm";
    return "Sinus Tachycardia";
  }

  if (bpm < 60.0f && cv < 0.15f && pctWide < 0.4f) {
    severity = "WARNING";
    detail = "HR=" + String(bpm, 0) + "bpm";
    return "Sinus Bradycardia";
  }

  if (bpm >= 50.0f && bpm <= 105.0f && cv >= 0.05f && cv <= 0.18f) {
    severity = "INFO";
    detail = "CV=" + String(cv, 3);
    return "Sinus Arrhythmia";
  }

  severity = "NORMAL";
  detail = "HR=" + String(bpm, 0) + "bpm";
  return "Normal Sinus Rhythm";
}
