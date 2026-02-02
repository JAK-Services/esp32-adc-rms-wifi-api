// Exposes the HTTP API and dashboard entry points.
// Provides access to the shared HTTP server handle.
// Keeps web-facing interfaces stable for other modules.

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t Api_Start(void);

httpd_handle_t Api_GetHttpServer(void);
