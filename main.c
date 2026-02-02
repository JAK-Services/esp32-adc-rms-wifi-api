// Application entry point that initializes subsystems and starts runtime tasks.
// Brings up storage, Wi-Fi, web services, and measurement components in order.
// Owns the top-level startup sequence and overall application lifetime.

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "adc.h"
#include "wifi_mgr.h"
#include "api.h"
#include "wifi_prov.h"
#include "storage.h"
#include "app_config.h"

static const char *gTag = "MAIN";

static void AdcScheduler_Task(void *pvArg)
{
    // Runs periodic ADC measurements at a coarse interval
    // Executes measurement and leaves results cached for API reads
    // Continues regardless of Wi-Fi state to keep data fresh

    (void)pvArg;

    // Delay before first measurement to allow boot services to start
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {

        // Perform one measurement cycle
        (void)Adc_MeasureNow();

        // Sleep until next scheduled measurement time
        vTaskDelay(pdMS_TO_TICKS(iMeasurePeriodSeconds * 1000));
    }
}

void app_main(void)
{
    // Initializes storage, ADC subsystem, Wi-Fi manager, and HTTP API
    // Starts periodic measurement task for cached RMS values
    // Provides provisioning fallback when Wi-Fi credentials are missing

    // Initialize storage early for Wi-Fi credential access
    ESP_ERROR_CHECK(Storage_Init());

    // Initialize ADC subsystem
    ESP_ERROR_CHECK(Adc_Init());

    // Start Wi-Fi manager (connect or provisioning)
    ESP_ERROR_CHECK(WifiMgr_Start());

    // Start API server (works in STA or AP mode)
    ESP_ERROR_CHECK(Api_Start());

    // Register provisioning endpoints on the shared HTTP server
    ESP_ERROR_CHECK(WifiProv_RegisterHandlers(Api_GetHttpServer()));

    // Start periodic measurement task
    BaseType_t bOk = xTaskCreate(AdcScheduler_Task, "adc_sched", 4096, NULL, 5, NULL);
    if (bOk != pdPASS) {
        ESP_LOGE(gTag, "Failed to start adc scheduler task");
    }

    ESP_LOGI(gTag, "Boot complete");
}
