// Builds compact JSON payloads used by HTTP API endpoints.
// Encodes device status and measurement results for browser and client parsing.
// Keeps formatting logic isolated from transport and measurement modules.

#include "proto.h"

#include <stdio.h>
#include <inttypes.h>

int Proto_BuildStatusJson(char *psBuffer, size_t szBuffer, wifi_mgr_state_t eState)
{
    // Builds JSON payload for device status endpoint
    // Encodes Wi-Fi state as integer for simple client parsing
    // Keeps output compact for small heap usage

    // Format JSON output
    int iWritten = snprintf(psBuffer, szBuffer,
                            "{"
                            "\"wifiState\":%d"
                            "}",
                            (int)eState);
    return iWritten;
}

int Proto_BuildRmsJson(char *psBuffer, size_t szBuffer, const adc_result_t *psResult, bool bHasResult)
{
    // Builds JSON payload for RMS endpoint
    // Includes last measurement values and timestamp when available
    // Returns a valid JSON object even when no measurement exists

    // Handle missing measurement case
    if (!bHasResult || psResult == NULL) {
        int iWritten = snprintf(psBuffer, szBuffer,
                                "{"
                                "\"hasValue\":false"
                                "}");
        return iWritten;
    }

    // Format JSON output with RMS values
    int iWritten = snprintf(psBuffer, szBuffer,
                            "{"
                            "\"hasValue\":true,"
                            "\"rmsA\":%.6f,"
                            "\"rmsB\":%.6f,"
                            "\"timestampUs\":%" PRId64 ","
                            "\"attenA\":%d,"
                            "\"attenB\":%d,"
                            "\"samples\":%d"
                            "}",
                            psResult->fRmsVoltsChA,
                            psResult->fRmsVoltsChB,
                            psResult->liTimestampUs,
                            (int)psResult->eAttenChA,
                            (int)psResult->eAttenChB,
                            psResult->iSamplesPerChannel);
    return iWritten;
}
