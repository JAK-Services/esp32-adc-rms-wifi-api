// Shared HTTP server owner used by API and provisioning endpoints.
// Starts a single ESP-IDF httpd instance for all network interfaces.
// Allows modules to register routes without owning server lifetime.

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t WebSrv_Start(void);
httpd_handle_t WebSrv_GetHandle(void);
