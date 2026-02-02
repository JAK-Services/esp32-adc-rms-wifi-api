// Centralizes application-wide configuration constants and compile-time settings.
// Defines device identity, ADC measurement parameters, and Wi-Fi defaults.
// Keeps tuning values in one place to simplify calibration and maintenance.

#pragma once

// ======================== Device identity ========================
#define sDeviceName                     "esp32-adc-node"

// ======================== ADC hardware mapping (ADC1) ========================
// CH_A on ADC1
#define iChA_AdcChannel                 ADC_CHANNEL_6   // GPIO34 = ADC1_CH6
// CH_B on ADC1
#define iChB_AdcChannel                 ADC_CHANNEL_7   // GPIO35 = ADC1_CH7

// ======================== ADC acquisition tuning ========================
// Signal characteristics used to size capture window
#define iSignal_Hz                      50
#define iPeriods_ToCapture              3
#define iCapture_Ms                     (1000 * iPeriods_ToCapture / iSignal_Hz)

// Sample rate used for paired sampling
#define iPerChSampleRate_Hz             2000

// Derived sample count per channel
#define iSamples_PerCh                  ((iPerChSampleRate_Hz * iCapture_Ms) / 1000)

// Moving average filter taps (must be odd)
#define iFilterTapCount                 5

// ADC full scale for 12-bit
#define iAdcFullScaleCounts             4095

// ======================== Measurement schedule ========================
#define iMeasurePeriodSeconds           10

// ======================== Wi-Fi provisioning SoftAP ========================
#define sProvApSsidPrefix               "JAK_DEVICE"
#define sProvApPassword                 "configureme" // Default provisioning password – change before deployment
#define iProvApChannel                  6
#define PROV_AP_IP_ADDR                 "192.168.4.1"

// ======================== Wi-Fi retry behavior ========================
#define iWifiConnectTimeoutMs           45000
#define iWifiRetryBackoffMinMs          500
#define iWifiRetryBackoffMaxMs          10000

// ======================== HTTP server ========================
#define iHttpServerPort                 80
