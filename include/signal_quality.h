#ifndef SIGNAL_QUALITY_H
#define SIGNAL_QUALITY_H

#include <Arduino.h>
#include "types.h"

PerformanceMetrics calculateBatchMetrics(
    const int32_t *rawSignal,
    const int32_t *cleanSignal,
    int length,
    float currentBpm,
    int peakCount
);

void printMetricsToSerial(unsigned long seq, const PerformanceMetrics &metrics);

#endif // SIGNAL_QUALITY_H
