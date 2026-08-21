#pragma once

#include <stdint.h>

#define FILTERORDER 161
#define NRCOEFF 0.992f

class ProtoCentralFilter {
public:
    ProtoCentralFilter();
    int32_t processSample(int32_t currSample);

private:
    int16_t CoeffBuf_40Hz_LowPass[FILTERORDER];
    int32_t ECG_WorkingBuff[FILTERORDER * 2];
    uint16_t ECG_bufStart;
    uint16_t ECG_bufCur;
    int32_t ECG_Pvev_DC_Sample;
    int32_t ECG_Pvev_Sample;
};
