// Declares persistent storage APIs for configuration and Wi-Fi credentials.
// Defines credential structures and function prototypes used across the app.
// Keeps NVS details private to the storage implementation file.

#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct
{
    char sSsid[33];
    char sPassword[65];
    bool bValid;
} wifi_creds_t;

esp_err_t Storage_Init(void);
esp_err_t Storage_LoadWifiCreds(wifi_creds_t *psCredsOut);
esp_err_t Storage_SaveWifiCreds(const wifi_creds_t *psCreds);
esp_err_t Storage_ClearWifiCreds(void);
