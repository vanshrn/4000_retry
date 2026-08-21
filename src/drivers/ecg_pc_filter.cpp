#include "ecg_pc_filter.h"

ProtoCentralFilter::ProtoCentralFilter() {
    ECG_bufStart = 0;
    ECG_bufCur = FILTERORDER - 1;
    ECG_Pvev_DC_Sample = 0;
    ECG_Pvev_Sample = 0;

    for (int i = 0; i < FILTERORDER * 2; i++) {
        ECG_WorkingBuff[i] = 0;
    }

    int16_t coeffs[FILTERORDER] = {
        -72,    122,    -31,    -99,    117,      0,   -121,    105,     34,
        -137,     84,     70,   -146,     55,    104,   -147,     20,    135,
        -137,    -21,    160,   -117,    -64,    177,    -87,   -108,    185,
         -48,   -151,    181,      0,   -188,    164,     54,   -218,    134,
         112,   -238,     90,    171,   -244,     33,    229,   -235,    -36,
         280,   -208,   -115,    322,   -161,   -203,    350,    -92,   -296,
         361,      0,   -391,    348,    117,   -486,    305,    264,   -577,
         225,    445,   -660,     93,    676,   -733,   -119,    991,   -793,
        -480,   1486,   -837,  -1226,   2561,   -865,  -4018,   9438,  20972,
        9438,  -4018,   -865,   2561,  -1226,   -837,   1486,   -480,   -793,
         991,   -119,   -733,    676,     93,   -660,    445,    225,   -577,
         264,    305,   -486,    117,    348,   -391,      0,    361,   -296,
         -92,    350,   -203,   -161,    322,   -115,   -208,    280,    -36,
        -235,    229,     33,   -244,    171,     90,   -238,    112,    134,
        -218,     54,    164,   -188,      0,    181,   -151,    -48,    185,
        -108,    -87,    177,    -64,   -117,    160,    -21,   -137,    135,
          20,   -147,    104,     55,   -146,     70,     84,   -137,     34,
         105,   -121,      0,    117,    -99,    -31,    122,    -72
    };
    for (int i = 0; i < FILTERORDER; i++) {
        CoeffBuf_40Hz_LowPass[i] = coeffs[i];
    }
}

int32_t ProtoCentralFilter::processSample(int32_t currSample) {
    // IIR High Pass (DC removal)
    float temp1 = NRCOEFF * (float)ECG_Pvev_DC_Sample;
    ECG_Pvev_DC_Sample = (currSample - ECG_Pvev_Sample) + (int32_t)temp1;
    ECG_Pvev_Sample = currSample;

    int32_t ecgData = ECG_Pvev_DC_Sample >> 2;

    // FIR Filter
    ECG_WorkingBuff[ECG_bufCur] = ecgData;
    
    int64_t acc = 0;
    int16_t* coeffPtr = CoeffBuf_40Hz_LowPass;
    int32_t* workingPtr = &ECG_WorkingBuff[ECG_bufCur];
    
    for (int k = 0; k < FILTERORDER; k++) {
        acc += (int64_t)(*coeffPtr++) * (int64_t)(*workingPtr--);
    }

    if (acc > 0x3fffffff) acc = 0x3fffffff;
    else if (acc < -0x40000000) acc = -0x40000000;

    int32_t filtOut = (int32_t)(acc >> 15);

    ECG_WorkingBuff[ECG_bufStart] = ecgData;

    ECG_bufCur++;
    ECG_bufStart++;

    if (ECG_bufStart == (FILTERORDER - 1)) {
        ECG_bufStart = 0;
        ECG_bufCur = FILTERORDER - 1;
    }

    return filtOut;
}
