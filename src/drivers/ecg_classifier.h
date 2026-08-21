/**
 * ecg_classifier.h — ECG Diagnostic Classification
 * -----------------------------------------------------------
 * Migrated from ecg_esp32_ads1292r_v10_working_1.ino.
 * Analyses a 5-second (625-sample) window and returns a
 * diagnostic condition name with severity and detail strings.
 * -----------------------------------------------------------
 */

#pragma once
#include <Arduino.h>
#include "types.h"

// Primary entry point: classify a 5-second ECG window.
String classifyWindow(const int32_t* samples, int sampleCount, String& severity, String& detail);

// Expose R-peak detector for template extraction
int detectPeaks(const int32_t* samples, int sampleCount, int* peaks, int maxPeaks, float& maxAbs);

// Helper: map a validation failure reason string to a user-friendly condition name.
String validationConditionName(const String& reason);

// Helper: map a validation failure reason string to a user-friendly detail string.
String validationDetailText(const String& reason);
