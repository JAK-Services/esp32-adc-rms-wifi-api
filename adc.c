// Implements ADC sampling and signal processing for two input channels.
// Provides RMS measurement with filtering, DC removal, and attenuation selection.
// Caches last waveform window in volts (mV) for plotting without re-sampling.

#include "adc.h"

#include <math.h>
#include <string.h>
#include <limits.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#include "app_config.h"

static const char *gTag = "ADC";

// ======================== ADC internal state ========================
static adc_oneshot_unit_handle_t gsAdcHandleUnit1 = NULL;
static SemaphoreHandle_t gsAdcMutex = NULL;

static adc_result_t gsLatestResult;
static bool gbHasLatest = false;


// ======================== Last captured waveform cache (AC, mV) ========================
static int16_t gaiLastAcMilliVoltsChA[iSamples_PerCh];
static int16_t gaiLastAcMilliVoltsChB[iSamples_PerCh];
static int giLastSamplesCount = 0;
static int64_t gliLastSamplesTimestampUs = 0;
static adc_atten_t geLastSamplesAttenChA = ADC_ATTEN_DB_12;
static adc_atten_t geLastSamplesAttenChB = ADC_ATTEN_DB_12;
static bool gbHasLastSamples = false;



static void Dc_Remove(const uint16_t *puInput, int32_t *piOutput, int iCount)
{
    // Removes DC component from samples by subtracting the mean value
    // Produces signed, zero-centered samples for waveform display and RMS compute
    // Keeps units in ADC counts so conversion can be applied later

    // Compute mean value
    int64_t liSum = 0;
    for (int iIndex = 0; iIndex < iCount; iIndex++) {
        liSum += puInput[iIndex];
    }
    float fMean = (float)liSum / (float)iCount;

    // Subtract mean from every sample
    for (int iIndex = 0; iIndex < iCount; iIndex++) {
        piOutput[iIndex] = (int32_t)((float)puInput[iIndex] - fMean);
    }
}



static void Moving_Average_Filter(const uint16_t *puInput, uint16_t *puOutput, int iCount)
{
    // Applies moving average filter to reduce high frequency noise
    // Preserves sample count by clamping indices near edges
    // Keeps output values in ADC counts for later processing

    // Set half window for symmetric averaging
    int iTapHalf = iFilterTapCount / 2;

    // Filter each sample with a clamped moving window
    for (int iIndex = 0; iIndex < iCount; iIndex++) {

        uint32_t uiAccumulator = 0;

        // Accumulate taps around current index
        for (int iTap = -iTapHalf; iTap <= iTapHalf; iTap++) {
            int iSource = iIndex + iTap;
            if (iSource < 0) iSource = 0;
            if (iSource >= iCount) iSource = iCount - 1;
            uiAccumulator += puInput[iSource];
        }

        // Store averaged output sample
        puOutput[iIndex] = (uint16_t)(uiAccumulator / (uint32_t)iFilterTapCount);
    }
}



static float Adc_CountsToVolts(adc_atten_t eAttenChannel, int32_t iCounts)
{
    // Converts signed ADC counts to volts using attenuation full-scale assumption
    // Uses a simple full-scale approximation per ESP32 attenuation option
    // Returns AC-relative volts when used after DC removal

    // Select full-scale voltage based on attenuation setting
    float fFullScaleVolts = 1.1f;
    switch (eAttenChannel) {
        case ADC_ATTEN_DB_0:   fFullScaleVolts = 1.1f; break;
        case ADC_ATTEN_DB_2_5: fFullScaleVolts = 1.5f; break;
        case ADC_ATTEN_DB_6:   fFullScaleVolts = 2.2f; break;
        case ADC_ATTEN_DB_12:
        default:               fFullScaleVolts = 3.9f; break;
    }

    // Convert ADC counts to volts using the selected full-scale range
    float fVolts = ((float)iCounts * fFullScaleVolts) / (float)iAdcFullScaleCounts;
    return fVolts;
}



static float Compute_RmsVolts(const int32_t *piAcCounts, int iCount, adc_atten_t eAtten)
{
    // Computes RMS value from zero-centered ADC counts
    // Converts each sample to volts then calculates RMS in volts
    // Uses double accumulation to reduce rounding noise on long sums

    // Accumulate squared voltage samples
    double dSumSq = 0.0;
    for (int iIndex = 0; iIndex < iCount; iIndex++) {
        float fVolts = Adc_CountsToVolts(eAtten, piAcCounts[iIndex]);
        dSumSq += (double)fVolts * (double)fVolts;
    }

    // Convert sum into RMS
    double dMeanSq = dSumSq / (double)iCount;
    float fRms = (float)sqrt(dMeanSq);
    return fRms;
}



static bool Capture_PairedSamples(uint16_t *puChA, uint16_t *puChB, int iCount)
{
    // Captures paired samples from ADC1 channels with a fixed time base
    // Uses esp_rom_delay_us to approximate uniform sampling interval
    // Returns false if any ADC read fails during the capture window

    // Compute sample interval in microseconds
    const int64_t liSamplePeriodUs = (1000000LL / (int64_t)iPerChSampleRate_Hz);

    // Initialize capture loop timing
    int iSampleIndex = 0;
    int64_t liNextSampleTimeUs = esp_timer_get_time();

    // Capture paired samples for the requested count
    while (iSampleIndex < iCount) {

        // Wait until the next scheduled sample time
        int64_t liNowUs = esp_timer_get_time();
        if (liNowUs < liNextSampleTimeUs) {
            esp_rom_delay_us((uint32_t)(liNextSampleTimeUs - liNowUs));
        }

        // Read CH_A from ADC1
        int iRawChA = 0;
        esp_err_t eErrA = adc_oneshot_read(gsAdcHandleUnit1, iChA_AdcChannel, &iRawChA);
        if (eErrA != ESP_OK) {
            ESP_LOGE(gTag, "adc_oneshot_read CH_A failed: %s", esp_err_to_name(eErrA));
            return false;
        }

        // Read CH_B from ADC1
        int iRawChB = 0;
        esp_err_t eErrB = adc_oneshot_read(gsAdcHandleUnit1, iChB_AdcChannel, &iRawChB);
        if (eErrB != ESP_OK) {
            ESP_LOGE(gTag, "adc_oneshot_read CH_B failed: %s", esp_err_to_name(eErrB));
            return false;
        }

        // Store paired samples
        puChA[iSampleIndex] = (uint16_t)iRawChA;
        puChB[iSampleIndex] = (uint16_t)iRawChB;

        // Advance to the next index and time slot
        iSampleIndex++;
        liNextSampleTimeUs += liSamplePeriodUs;
    }

    return true;
}



static adc_atten_t Step_AttenuationMoreSensitive(adc_atten_t eCurrent)
{
    // Steps attenuation one level toward more sensitivity
    // Uses the ESP32 attenuation ordering from lowest range to highest range
    // Returns current value if already at the most sensitive setting

    // Define ordered attenuation levels
    const adc_atten_t aeLevels[] = { ADC_ATTEN_DB_0, ADC_ATTEN_DB_2_5, ADC_ATTEN_DB_6, ADC_ATTEN_DB_12 };
    const int iLevelCount = (int)(sizeof(aeLevels) / sizeof(aeLevels[0]));

    // Find current index and step down if possible
    for (int iIndex = 0; iIndex < iLevelCount; iIndex++) {
        if (aeLevels[iIndex] == eCurrent) {
            if (iIndex > 0) {
                return aeLevels[iIndex - 1];
            }
            return eCurrent;
        }
    }

    return eCurrent;
}



static void AutoRange_Attenuations(adc_atten_t *peAttenChA, adc_atten_t *peAttenChB)
{
    // Auto-ranges channels to the most sensitive attenuation that does not saturate
    // Starts from least sensitive and steps toward more sensitive until saturation
    // Leaves each channel at the last non-saturating attenuation level found

    // Start from least sensitive to avoid immediate clipping
    adc_atten_t eAttenA = ADC_ATTEN_DB_12;
    adc_atten_t eAttenB = ADC_ATTEN_DB_12;
    adc_atten_t ePrevA = eAttenA;
    adc_atten_t ePrevB = eAttenB;

    bool bDoneA = false;
    bool bDoneB = false;

    // Try a bounded number of attempts to avoid infinite loops
    for (int iAttempt = 0; iAttempt < 12 && !(bDoneA && bDoneB); iAttempt++) {

        // Apply current attenuation settings
        adc_oneshot_chan_cfg_t sChanCfgA = { .atten = eAttenA, .bitwidth = ADC_BITWIDTH_12 };
        adc_oneshot_chan_cfg_t sChanCfgB = { .atten = eAttenB, .bitwidth = ADC_BITWIDTH_12 };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(gsAdcHandleUnit1, iChA_AdcChannel, &sChanCfgA));
        ESP_ERROR_CHECK(adc_oneshot_config_channel(gsAdcHandleUnit1, iChB_AdcChannel, &sChanCfgB));

        // Capture one analysis frame
        static uint16_t auRawChA[iSamples_PerCh];
        static uint16_t auRawChB[iSamples_PerCh];
        if (!Capture_PairedSamples(auRawChA, auRawChB, iSamples_PerCh)) {
            break;
        }

        // Filter samples for stable saturation detection
        static uint16_t auFiltChA[iSamples_PerCh];
        static uint16_t auFiltChB[iSamples_PerCh];
        Moving_Average_Filter(auRawChA, auFiltChA, iSamples_PerCh);
        Moving_Average_Filter(auRawChB, auFiltChB, iSamples_PerCh);

        // Compute saturation flags per channel
        int iFullScaleHitsA = 0;
        int iFullScaleHitsB = 0;
        for (int iIndex = 0; iIndex < iSamples_PerCh; iIndex++) {
            if ((int)auFiltChA[iIndex] >= iAdcFullScaleCounts) iFullScaleHitsA++;
            if ((int)auFiltChB[iIndex] >= iAdcFullScaleCounts) iFullScaleHitsB++;
        }
        bool bSaturatedA = (iFullScaleHitsA > 0);
        bool bSaturatedB = (iFullScaleHitsB > 0);

        // Update channel A attenuation choice
        if (!bDoneA) {
            if (bSaturatedA) {
                eAttenA = ePrevA;
                bDoneA = true;
            } else if (eAttenA == ADC_ATTEN_DB_0) {
                bDoneA = true;
            } else {
                ePrevA = eAttenA;
                eAttenA = Step_AttenuationMoreSensitive(eAttenA);
            }
        }

        // Update channel B attenuation choice
        if (!bDoneB) {
            if (bSaturatedB) {
                eAttenB = ePrevB;
                bDoneB = true;
            } else if (eAttenB == ADC_ATTEN_DB_0) {
                bDoneB = true;
            } else {
                ePrevB = eAttenB;
                eAttenB = Step_AttenuationMoreSensitive(eAttenB);
            }
        }
    }

    // Return chosen values
    if (peAttenChA != NULL) *peAttenChA = eAttenA;
    if (peAttenChB != NULL) *peAttenChB = eAttenB;
}



esp_err_t Adc_Init(void)
{
    // Initializes the ADC unit and channel configuration
    // Creates the mutex used to guard cached measurement results
    // Prepares the module for periodic or on-demand measurements

    // Create ADC mutex for shared state
    if (gsAdcMutex == NULL) {
        gsAdcMutex = xSemaphoreCreateMutex();
    }
    if (gsAdcMutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Create ADC oneshot unit
    adc_oneshot_unit_init_cfg_t sInitCfg = {
        .unit_id = ADC_UNIT_1
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&sInitCfg, &gsAdcHandleUnit1));

    // Default channel configuration; attenuation will be reconfigured dynamically
    adc_oneshot_chan_cfg_t sChanCfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(gsAdcHandleUnit1, iChA_AdcChannel, &sChanCfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(gsAdcHandleUnit1, iChB_AdcChannel, &sChanCfg));

    ESP_LOGI(gTag, "ADC initialized (samples=%d)", iSamples_PerCh);
    return ESP_OK;
}



esp_err_t Adc_MeasureNow(void)
{
    // Captures one window, computes RMS, and caches last waveform in volts
    // Uses filtering and DC removal so the cached waveform is centered at 0 V
    // Stores results under mutex so API reads are consistent

    // Validate initialization state
    if (gsAdcHandleUnit1 == NULL || gsAdcMutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Choose attenuations using auto-ranging
    adc_atten_t eChosenAttenA = ADC_ATTEN_DB_12;
    adc_atten_t eChosenAttenB = ADC_ATTEN_DB_12;
    AutoRange_Attenuations(&eChosenAttenA, &eChosenAttenB);

    // Apply chosen attenuations before capture
    adc_oneshot_chan_cfg_t sChanCfgA = { .atten = eChosenAttenA, .bitwidth = ADC_BITWIDTH_12 };
    adc_oneshot_chan_cfg_t sChanCfgB = { .atten = eChosenAttenB, .bitwidth = ADC_BITWIDTH_12 };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(gsAdcHandleUnit1, iChA_AdcChannel, &sChanCfgA));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(gsAdcHandleUnit1, iChB_AdcChannel, &sChanCfgB));

    // Capture paired raw samples
    static uint16_t auRawChA[iSamples_PerCh];
    static uint16_t auRawChB[iSamples_PerCh];
    if (!Capture_PairedSamples(auRawChA, auRawChB, iSamples_PerCh)) {
        return ESP_FAIL;
    }

    // Filter raw samples for stable waveform and RMS
    static uint16_t auFiltChA[iSamples_PerCh];
    static uint16_t auFiltChB[iSamples_PerCh];
    Moving_Average_Filter(auRawChA, auFiltChA, iSamples_PerCh);
    Moving_Average_Filter(auRawChB, auFiltChB, iSamples_PerCh);

    // Remove DC component per channel to get AC counts around 0
    static int32_t aiAcCountsChA[iSamples_PerCh];
    static int32_t aiAcCountsChB[iSamples_PerCh];
    Dc_Remove(auFiltChA, aiAcCountsChA, iSamples_PerCh);
    Dc_Remove(auFiltChB, aiAcCountsChB, iSamples_PerCh);

    // Compute RMS values in volts from DC-removed waveform
    float fRmsA = Compute_RmsVolts(aiAcCountsChA, iSamples_PerCh, eChosenAttenA);
    float fRmsB = Compute_RmsVolts(aiAcCountsChB, iSamples_PerCh, eChosenAttenB);

    // Convert AC counts to signed millivolts for caching and plotting
    static int16_t aiAcMilliVoltsChA[iSamples_PerCh];
    static int16_t aiAcMilliVoltsChB[iSamples_PerCh];

    for (int iIndex = 0; iIndex < iSamples_PerCh; iIndex++) {

        float fVoltsA = Adc_CountsToVolts(eChosenAttenA, aiAcCountsChA[iIndex]);
        float fVoltsB = Adc_CountsToVolts(eChosenAttenB, aiAcCountsChB[iIndex]);

        int32_t iMilliVoltsA = (int32_t)lroundf(fVoltsA * 1000.0f);
        int32_t iMilliVoltsB = (int32_t)lroundf(fVoltsB * 1000.0f);

        if (iMilliVoltsA > INT16_MAX) iMilliVoltsA = INT16_MAX;
        if (iMilliVoltsA < INT16_MIN) iMilliVoltsA = INT16_MIN;
        if (iMilliVoltsB > INT16_MAX) iMilliVoltsB = INT16_MAX;
        if (iMilliVoltsB < INT16_MIN) iMilliVoltsB = INT16_MIN;

        aiAcMilliVoltsChA[iIndex] = (int16_t)iMilliVoltsA;
        aiAcMilliVoltsChB[iIndex] = (int16_t)iMilliVoltsB;
    }

    // Store latest results and last waveform atomically
    int64_t liNowTimestampUs = esp_timer_get_time();

    xSemaphoreTake(gsAdcMutex, portMAX_DELAY);

    gsLatestResult.fRmsVoltsChA = fRmsA;
    gsLatestResult.fRmsVoltsChB = fRmsB;
    gsLatestResult.liTimestampUs = liNowTimestampUs;
    gsLatestResult.eAttenChA = eChosenAttenA;
    gsLatestResult.eAttenChB = eChosenAttenB;
    gsLatestResult.iSamplesPerChannel = iSamples_PerCh;
    gbHasLatest = true;

    memcpy(gaiLastAcMilliVoltsChA, aiAcMilliVoltsChA, sizeof(gaiLastAcMilliVoltsChA));
    memcpy(gaiLastAcMilliVoltsChB, aiAcMilliVoltsChB, sizeof(gaiLastAcMilliVoltsChB));
    giLastSamplesCount = iSamples_PerCh;
    gliLastSamplesTimestampUs = liNowTimestampUs;
    geLastSamplesAttenChA = eChosenAttenA;
    geLastSamplesAttenChB = eChosenAttenB;
    gbHasLastSamples = true;

    xSemaphoreGive(gsAdcMutex);

    ESP_LOGI(gTag, "RMS A=%.6f V, B=%.6f V (atten %d,%d)", fRmsA, fRmsB, (int)eChosenAttenA, (int)eChosenAttenB);
    return ESP_OK;
}



bool Adc_GetLatest(adc_result_t *psResultOut)
{
    // Copies latest ADC result into caller buffer safely
    // Returns false if no measurement has been taken yet
    // Allows API layer to serve cached values without blocking ADC

    // Validate output pointer
    if (psResultOut == NULL || gsAdcMutex == NULL) {
        return false;
    }

    // Copy cached result if available
    xSemaphoreTake(gsAdcMutex, portMAX_DELAY);
    bool bHasValue = gbHasLatest;
    if (bHasValue) {
        *psResultOut = gsLatestResult;
    }
    xSemaphoreGive(gsAdcMutex);

    return bHasValue;
}



bool Adc_GetLastSamplesMilliVolts(int16_t *piChannelA_mV, int16_t *piChannelB_mV, int iMaxSamples,
                                  int *piSamplesReturned, int64_t *pliTimestampUs,
                                  adc_atten_t *peAttenChannelA, adc_atten_t *peAttenChannelB)
{
    // Copies the last cached AC waveform window as signed millivolts
    // Keeps the waveform centered around 0 so both channels share a common zero axis
    // Provides metadata to let the UI annotate captures consistently

    // Validate module state
    if (gsAdcMutex == NULL || iMaxSamples <= 0) {
        return false;
    }

    // Copy cached samples under mutex
    xSemaphoreTake(gsAdcMutex, portMAX_DELAY);

    bool bHasValue = gbHasLastSamples;
    int iCopyCount = giLastSamplesCount;
    if (iCopyCount > iMaxSamples) {
        iCopyCount = iMaxSamples;
    }

    // Copy waveform and metadata when available
    if (bHasValue && iCopyCount > 0) {

        // Copy waveform arrays when provided
        if (piChannelA_mV != NULL) {
            memcpy(piChannelA_mV, gaiLastAcMilliVoltsChA, (size_t)iCopyCount * sizeof(int16_t));
        }
        if (piChannelB_mV != NULL) {
            memcpy(piChannelB_mV, gaiLastAcMilliVoltsChB, (size_t)iCopyCount * sizeof(int16_t));
        }

        // Copy metadata fields when provided
        if (piSamplesReturned != NULL) {
            *piSamplesReturned = iCopyCount;
        }
        if (pliTimestampUs != NULL) {
            *pliTimestampUs = gliLastSamplesTimestampUs;
        }
        if (peAttenChannelA != NULL) {
            *peAttenChannelA = geLastSamplesAttenChA;
        }
        if (peAttenChannelB != NULL) {
            *peAttenChannelB = geLastSamplesAttenChB;
        }
    }

    xSemaphoreGive(gsAdcMutex);

    return (bHasValue && iCopyCount > 0);
}
