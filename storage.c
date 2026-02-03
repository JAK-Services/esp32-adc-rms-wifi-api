// Implements NVS-backed persistent storage for device configuration data.
// Stores and retrieves Wi-Fi credentials with simple validation and defaults.
// Provides init, save, load, and clear operations for configuration keys.

#include "storage.h"

#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *gTag = "STORAGE";

static const char *gsNamespace = "cfg";
static const char *gsKeySsid = "wifi_ssid";
static const char *gsKeyPass = "wifi_pass";


esp_err_t Storage_Init(void)
{
    // Initializes NVS flash storage for configuration persistence
    // Ensures NVS is ready for wifi credential reads and writes
    // Repairs NVS partition if version mismatch or no free pages

    // Initialize NVS flash
    esp_err_t eErr = nvs_flash_init();
    if (eErr == ESP_ERR_NVS_NO_FREE_PAGES || eErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(gTag, "NVS init issue, erasing and retrying");
        ESP_ERROR_CHECK(nvs_flash_erase());
        eErr = nvs_flash_init();
    }

    return eErr;
}


esp_err_t Storage_LoadWifiCreds(wifi_creds_t *psCredsOut)
{
    // Loads stored Wi-Fi SSID and password from NVS
    // Marks creds invalid if keys do not exist
    // Avoids partially valid results by requiring both keys present

    // Validate output pointer
    if (psCredsOut == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Reset output defaults
    memset(psCredsOut, 0, sizeof(*psCredsOut));
    psCredsOut->bValid = false;

    // Open namespace for read
    nvs_handle_t sHandle = 0;
    esp_err_t eErr = nvs_open(gsNamespace, NVS_READONLY, &sHandle);
    if (eErr != ESP_OK) {
        return eErr;
    }

    // Read SSID value
    size_t szSsidLen = sizeof(psCredsOut->sSsid);
    eErr = nvs_get_str(sHandle, gsKeySsid, psCredsOut->sSsid, &szSsidLen);
    if (eErr != ESP_OK) {
        nvs_close(sHandle);
        return ESP_OK;
    }

    // Read password value
    size_t szPassLen = sizeof(psCredsOut->sPassword);
    eErr = nvs_get_str(sHandle, gsKeyPass, psCredsOut->sPassword, &szPassLen);
    nvs_close(sHandle);

    // Mark valid only if both reads succeeded
    if (eErr == ESP_OK) {
        psCredsOut->bValid = true;
    }

    return ESP_OK;
}


esp_err_t Storage_SaveWifiCreds(const wifi_creds_t *psCreds)
{
    // Saves Wi-Fi SSID and password into NVS
    // Commits changes to flash to survive reboot
    // Overwrites existing values atomically within namespace

    // Validate input pointer and fields
    if (psCreds == NULL || psCreds->sSsid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    // Open namespace for write
    nvs_handle_t sHandle = 0;
    esp_err_t eErr = nvs_open(gsNamespace, NVS_READWRITE, &sHandle);
    if (eErr != ESP_OK) {
        return eErr;
    }

    // Write SSID and password
    eErr = nvs_set_str(sHandle, gsKeySsid, psCreds->sSsid);
    if (eErr == ESP_OK) {
        eErr = nvs_set_str(sHandle, gsKeyPass, psCreds->sPassword);
    }

    // Commit changes
    if (eErr == ESP_OK) {
        eErr = nvs_commit(sHandle);
    }

    nvs_close(sHandle);
    return eErr;
}


esp_err_t Storage_ClearWifiCreds(void)
{
    // Removes stored Wi-Fi credential keys from NVS
    // Commits erase operations to ensure removal persists
    // Leaves other configuration values in namespace untouched

    // Open namespace for write
    nvs_handle_t sHandle = 0;
    esp_err_t eErr = nvs_open(gsNamespace, NVS_READWRITE, &sHandle);
    if (eErr != ESP_OK) {
        return eErr;
    }

    // Erase SSID and password keys
    (void)nvs_erase_key(sHandle, gsKeySsid);
    (void)nvs_erase_key(sHandle, gsKeyPass);

    // Commit erase operations
    eErr = nvs_commit(sHandle);
    nvs_close(sHandle);

    return eErr;
}
