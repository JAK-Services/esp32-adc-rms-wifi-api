// Declares ADC measurement APIs and shared result structures used by the app.
// Exposes initialization and on-demand measurement functions for other modules.
// Defines data types for RMS results and access to last captured waveform samples.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"

typedef struct
{
    float fRmsVoltsChA;
    float fRmsVoltsChB;
    int64_t liTimestampUs;
    adc_atten_t eAttenChA;
    adc_atten_t eAttenChB;
    int iSamplesPerChannel;
} adc_result_t;

esp_err_t Adc_Init(void);


esp_err_t Adc_MeasureNow(void);


bool Adc_GetLatest(adc_result_t *psResultOut);


bool Adc_GetLastSamplesMilliVolts(int16_t *piChannelA_mV, int16_t *piChannelB_mV, int iMaxSamples,
                                  int *piSamplesReturned, int64_t *pliTimestampUs,
                                  adc_atten_t *peAttenChannelA, adc_atten_t *peAttenChannelB);
