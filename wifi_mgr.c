// Manages Wi-Fi connectivity and exposes a persistent AP alongside STA.
// Retries STA reconnects indefinitely while keeping the local AP available.
// Integrates with provisioning by preserving stored credentials across reboots.

#include "wifi_mgr.h"

#include <string.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "lwip/inet.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "app_config.h"
#include "dns_captive.h"
#include "storage.h"

#define WIFI_CONNECTED_BIT BIT0

static const char *gTag = "WIFI_MGR";

static EventGroupHandle_t gsWifiEventGroup = NULL;
static wifi_mgr_state_t geWifiState = WIFI_MGR_STATE_INIT;

static int32_t giRetryBackoffMs = 0;
static int64_t gliLastConnectedMs = 0;

static esp_netif_t *gsStaNetif = NULL;
static esp_netif_t *gsApNetif = NULL;

static bool gbStaConnectInProgress = false;
static bool gbStaConfigured = false;

static bool gbStaIpValid = false;
static char gsStaIpStr[16] = {0};

static int giApClientCount = 0;

static void WifiMgr_Task(void *pvArg);
static void WifiMgr_ApplyBackoffDelay(void);
static void WifiMgr_SetState(wifi_mgr_state_t eNewState);
static void WifiMgr_EventHandler(void *pvArg,
                                esp_event_base_t sEventBase,
                                int32_t iEventId,
                                void *pvEventData);
static esp_err_t WifiMgr_ConnectStaIfConfigured(void);
static void WifiMgr_EnsureApIp(void);
static void WifiMgr_BuildApSsid(char *psSsid, size_t stSsidLen);

static esp_err_t WifiMgr_InitWifiStack(void);
static esp_err_t WifiMgr_StartWifiApSta(void);
static esp_err_t WifiMgr_ConfigureStaIfValid(const wifi_creds_t *psCreds);


static void WifiMgr_SetState(wifi_mgr_state_t eNewState)
{
    // Updates internal state used for diagnostics and UI
    // Keeps state transitions independent from captive portal DNS behavior
    // Avoids toggling DNS based on STA connectivity to keep AP reprovisioning usable

    // Apply new state value
    geWifiState = eNewState;
}


wifi_mgr_state_t WifiMgr_GetState(void)
{
    // Returns the current Wi-Fi manager state for API/UI reporting
    // Avoids exposing internal variables directly to the rest of the application
    // Keeps state reads consistent even if implementation changes

    return geWifiState;
}


bool WifiMgr_IsConnected(void)
{
    // Indicates whether the STA interface is connected and has an IP address
    // Lets higher-level code gate network operations on connectivity
    // Avoids duplicating event-group checks elsewhere

    EventBits_t uiBits = 0;

    // Read event bits in a thread-safe way
    if (gsWifiEventGroup != NULL) {
        uiBits = xEventGroupGetBits(gsWifiEventGroup);
    }

    return ((uiBits & WIFI_CONNECTED_BIT) != 0);
}


bool WifiMgr_GetStaIp(char *psOutIp, size_t stOutLen)
{
    // Copies the last-known STA IPv4 address string to the caller buffer
    // Provides a stable API for reporting IP without exposing internal storage
    // Returns whether the IP is currently valid

    if (psOutIp == NULL || stOutLen == 0) {
        return false;
    }

    // Return empty string when IP is not yet available
    if (!gbStaIpValid) {
        psOutIp[0] = '\0';
        return false;
    }

    // Copy the cached IP string into output buffer
    (void)snprintf(psOutIp, stOutLen, "%s", gsStaIpStr);
    return true;
}


static void WifiMgr_BuildApSsid(char *psSsid, size_t stSsidLen)
{
    // Builds a stable AP SSID based on a fixed prefix and the device MAC suffix
    // Helps users identify the correct provisioning access point reliably
    // Keeps formatting centralized to avoid mismatched SSID names across modules

    uint8_t auMac[6] = {0};
    esp_read_mac(auMac, ESP_MAC_WIFI_SOFTAP);

    // Format: JAK_DEVICE_XXYYZZ
    (void)snprintf(psSsid, stSsidLen,
                   "%s_%02X%02X%02X",
                   sProvApSsidPrefix,
                   auMac[3], auMac[4], auMac[5]);
}


static void WifiMgr_EnsureApIp(void)
{
    // Ensures the SoftAP interface keeps the expected IP and DHCP server settings
    // Restores AP IP after STA events that may disturb routing on some phones
    // Ensures DHCP advertises AP IP as DNS for captive portal hostname redirection

    if (gsApNetif == NULL) {
        return;
    }

    // Prepare the expected AP IPv4 information
    esp_netif_ip_info_t sIpInfo = {0};
    ip4_addr_t sTmpIp = {0};

    // Set interface IP to provisioning AP IP
    (void)ip4addr_aton(PROV_AP_IP_ADDR, &sTmpIp);
    sIpInfo.ip.addr = sTmpIp.addr;

    // Set netmask to 255.255.255.0
    (void)ip4addr_aton("255.255.255.0", &sTmpIp);
    sIpInfo.netmask.addr = sTmpIp.addr;

    // Set gateway to provisioning AP IP
    (void)ip4addr_aton(PROV_AP_IP_ADDR, &sTmpIp);
    sIpInfo.gw.addr = sTmpIp.addr;

    // Restart DHCP server with the expected configuration
    (void)esp_netif_dhcps_stop(gsApNetif);
    (void)esp_netif_set_ip_info(gsApNetif, &sIpInfo);
    (void)esp_netif_dhcps_start(gsApNetif);

    // Advertise AP IP as DNS so clients resolve all hostnames to the captive portal
    esp_netif_dns_info_t sDnsInfo = {0};
    (void)ip4addr_aton(PROV_AP_IP_ADDR,
                       (ip4_addr_t *)&sDnsInfo.ip.u_addr.ip4);
    sDnsInfo.ip.type = ESP_IPADDR_TYPE_V4;
    (void)esp_netif_set_dns_info(gsApNetif, ESP_NETIF_DNS_MAIN, &sDnsInfo);

    // Ensure DHCP server offers the DNS option to connected clients
    uint8_t uiOfferDns = 1;
    (void)esp_netif_dhcps_option(gsApNetif,
                                 ESP_NETIF_OP_SET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &uiOfferDns,
                                 sizeof(uiOfferDns));
}


static esp_err_t WifiMgr_InitWifiStack(void)
{
    // Initializes netif and event loop required by Wi-Fi and HTTP services
    // Creates default AP and STA netifs used by the APSTA Wi-Fi mode
    // Registers Wi-Fi and IP event handlers for state tracking

    esp_err_t eResult = esp_netif_init();
    if (eResult != ESP_OK && eResult != ESP_ERR_INVALID_STATE) {
        return eResult;
    }

    eResult = esp_event_loop_create_default();
    if (eResult != ESP_OK && eResult != ESP_ERR_INVALID_STATE) {
        return eResult;
    }

    if (gsStaNetif == NULL) {
        gsStaNetif = esp_netif_create_default_wifi_sta();
    }

    if (gsApNetif == NULL) {
        gsApNetif = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t sCfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&sCfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &WifiMgr_EventHandler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &WifiMgr_EventHandler, NULL));

    return ESP_OK;
}


static esp_err_t WifiMgr_StartWifiApSta(void)
{
    // Starts Wi-Fi in APSTA mode with a persistent AP and optional STA
    // Ensures the AP is available even when STA is connected or reconnecting
    // Applies configured AP settings including SSID, password, and channel

    wifi_config_t sApConfig = {0};
    WifiMgr_BuildApSsid((char *)sApConfig.ap.ssid, sizeof(sApConfig.ap.ssid));

    (void)strncpy((char *)sApConfig.ap.password,
                  sProvApPassword,
                  sizeof(sApConfig.ap.password) - 1);

    sApConfig.ap.channel = (uint8_t)iProvApChannel;
    sApConfig.ap.max_connection = 4;
    sApConfig.ap.ssid_len = (uint8_t)strlen((char *)sApConfig.ap.ssid);

    if (strlen(sProvApPassword) == 0) {
        sApConfig.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        sApConfig.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &sApConfig));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Re-apply AP IP to avoid losing access after STA changes
    WifiMgr_EnsureApIp();

    ESP_LOGI(gTag, "AP SSID: %s", (char *)sApConfig.ap.ssid);
    ESP_LOGI(gTag, "AP IP: http://%s/", PROV_AP_IP_ADDR);
    return ESP_OK;
}


static esp_err_t WifiMgr_ConfigureStaIfValid(const wifi_creds_t *psCreds)
{
    // Applies STA configuration when stored credentials are available
    // Prepares Wi-Fi driver for a station connection attempt
    // Keeps provisioning AP enabled regardless of STA configuration

    if (psCreds == NULL || !psCreds->bValid) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t sStaConfig = {0};

    // Copy SSID and password into ESP-IDF station config
    (void)strncpy((char *)sStaConfig.sta.ssid, psCreds->sSsid,
                  sizeof(sStaConfig.sta.ssid) - 1);
    (void)strncpy((char *)sStaConfig.sta.password, psCreds->sPassword,
                  sizeof(sStaConfig.sta.password) - 1);

    // Apply STA configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sStaConfig));
    gbStaConfigured = true;

    // Mark state as connecting for UI/API
    WifiMgr_SetState(WIFI_MGR_STATE_CONNECTING);

    return ESP_OK;
}


static esp_err_t WifiMgr_ConnectStaIfConfigured(void)
{
    // Starts a station connection attempt if the STA interface is configured
    // Avoids repeated esp_wifi_connect calls while a connect attempt is running
    // Returns ESP_OK when connect is initiated or already in progress

    if (!gbStaConfigured) {
        return ESP_ERR_INVALID_STATE;
    }

    if (gbStaConnectInProgress) {
        return ESP_OK;
    }

    gbStaConnectInProgress = true;
    return esp_wifi_connect();
}


static void WifiMgr_ApplyBackoffDelay(void)
{
    // Implements exponential backoff between STA reconnect attempts
    // Reduces Wi-Fi driver load and avoids tight reconnect loops
    // Caps delay to keep eventual recovery reasonable

    if (giRetryBackoffMs == 0) {
        giRetryBackoffMs = 500;
    } else {
        giRetryBackoffMs *= 2;
        if (giRetryBackoffMs > 10000) {
            giRetryBackoffMs = 10000;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(giRetryBackoffMs));
}


static void WifiMgr_EventHandler(void *pvArg,
                                esp_event_base_t sEventBase,
                                int32_t iEventId,
                                void *pvEventData)
{
    // Handles Wi-Fi and IP events to maintain connectivity state and cached IP
    // Starts DNS hijack only while at least one AP client is connected
    // Keeps AP reachable by re-applying AP IP if needed after STA transitions

    (void)pvArg;

    // Handle Wi-Fi events
    if (sEventBase == WIFI_EVENT) {

        // AP station connected: enable DNS captive for portal detection
        if (iEventId == WIFI_EVENT_AP_STACONNECTED) {

            // Increment AP client count and start DNS captive when first client joins
            giApClientCount += 1;
            if (giApClientCount == 1) {
                (void)DnsCaptive_Start();
                ESP_LOGI(gTag, "AP client joined, captive DNS enabled");
            }
        }

        // AP station disconnected: disable DNS captive when last client leaves
        if (iEventId == WIFI_EVENT_AP_STADISCONNECTED) {

            // Decrement AP client count with underflow protection
            if (giApClientCount > 0) {
                giApClientCount -= 1;
            }

            if (giApClientCount == 0) {
                (void)DnsCaptive_Stop();
                ESP_LOGI(gTag, "AP client left, captive DNS disabled");
            }
        }

        // STA started: attempt connect if configured
        if (iEventId == WIFI_EVENT_STA_START) {
            (void)WifiMgr_ConnectStaIfConfigured();
        }

        // STA disconnected: clear state and allow reconnect attempts
        if (iEventId == WIFI_EVENT_STA_DISCONNECTED) {
            gbStaConnectInProgress = false;
            gbStaIpValid = false;
            gsStaIpStr[0] = '\0';

            if (gsWifiEventGroup != NULL) {
                xEventGroupClearBits(gsWifiEventGroup, WIFI_CONNECTED_BIT);
            }

            WifiMgr_SetState(gbStaConfigured ? WIFI_MGR_STATE_CONNECTING : WIFI_MGR_STATE_PROVISIONING);

            // Keep AP access stable after disconnection
            WifiMgr_EnsureApIp();
        }
    }

    // Handle "got IP" events for the station interface
    if (sEventBase == IP_EVENT && iEventId == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *psEvent = (ip_event_got_ip_t *)pvEventData;

        // Cache the assigned IP address for API/UI
        (void)snprintf(gsStaIpStr, sizeof(gsStaIpStr),
                       IPSTR, IP2STR(&psEvent->ip_info.ip));
        gbStaIpValid = true;

        // Mark connected state for other modules
        if (gsWifiEventGroup != NULL) {
            xEventGroupSetBits(gsWifiEventGroup, WIFI_CONNECTED_BIT);
        }

        WifiMgr_SetState(WIFI_MGR_STATE_CONNECTED);

        // Preserve AP access while STA is connected
        WifiMgr_EnsureApIp();
    }
}


esp_err_t WifiMgr_Start(void)
{
    // Initializes Wi-Fi manager state and starts APSTA networking
    // Loads stored credentials and configures STA if available
    // Spawns the retry task that keeps STA attempting reconnects indefinitely

    // Initialize state and synchronization primitives
    if (gsWifiEventGroup == NULL) {
        gsWifiEventGroup = xEventGroupCreate();
    }

    giRetryBackoffMs = 0;
    gliLastConnectedMs = esp_timer_get_time() / 1000;
    giApClientCount = 0;

    // Ensure captive DNS is disabled until an AP client connects
    (void)DnsCaptive_Stop();

    WifiMgr_SetState(WIFI_MGR_STATE_INIT);

    // Initialize Wi-Fi stack and start persistent AP
    ESP_ERROR_CHECK(WifiMgr_InitWifiStack());
    ESP_ERROR_CHECK(WifiMgr_StartWifiApSta());

    // Load credentials and configure STA if present
    wifi_creds_t sCreds = {0};
    esp_err_t eLoadResult = Storage_LoadWifiCreds(&sCreds);

    if (eLoadResult != ESP_OK) {
        ESP_LOGW(gTag, "Creds load failed (%s), treating as no creds",
                 esp_err_to_name(eLoadResult));
        sCreds.bValid = false;
    }

    if (sCreds.bValid) {
        ESP_LOGI(gTag, "Loaded SSID: '%s' (valid=1)", sCreds.sSsid);
        ESP_ERROR_CHECK(WifiMgr_ConfigureStaIfValid(&sCreds));
        WifiMgr_SetState(WIFI_MGR_STATE_CONNECTING);
    } else {
        ESP_LOGW(gTag, "No creds, AP only (use /provision to set WiFi)");
        WifiMgr_SetState(WIFI_MGR_STATE_PROVISIONING);
    }

    // Start retry task
    xTaskCreate(WifiMgr_Task, "wifi_mgr", 4096, NULL, 5, NULL);
    return ESP_OK;
}


static void WifiMgr_Task(void *pvArg)
{
    // Runs STA retry logic while keeping AP available at all times
    // Avoids redundant connect calls when the driver is already connecting
    // Remains resident so disconnect events can be recovered without reboot

    (void)pvArg;

    while (1) {

        // Idle when connected to keep CPU usage low
        if (WifiMgr_IsConnected()) {

            // Reset backoff while connected
            giRetryBackoffMs = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));

        } else if (gbStaConfigured) {

            // Retry station connection with exponential backoff
            ESP_LOGI(gTag, "Retry connect");
            gbStaConnectInProgress = true;
            (void)esp_wifi_connect();
            WifiMgr_ApplyBackoffDelay();

        } else {

            // Wait when connecting or when link exists but IP is not yet assigned
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        // Maintain last-connected timestamp for future diagnostics
        (void)gliLastConnectedMs;
    }
}
