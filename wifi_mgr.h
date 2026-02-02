// Declares Wi-Fi manager APIs used across the application.
// Exposes connection state and cached STA addressing to other modules.
// Keeps Wi-Fi implementation details private to the manager source file.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum
{
    WIFI_MGR_STATE_INIT = 0,
    WIFI_MGR_STATE_CONNECTING,
    WIFI_MGR_STATE_CONNECTED,
    WIFI_MGR_STATE_PROVISIONING
} wifi_mgr_state_t;

esp_err_t WifiMgr_Start(void);
wifi_mgr_state_t WifiMgr_GetState(void);
bool WifiMgr_IsConnected(void);


// Returns current STA IPv4 address as dotted string. Returns true if valid.
bool WifiMgr_GetStaIp(char *psOutIp, size_t stOutLen);

