// Adds Wi-Fi provisioning endpoints to the shared HTTP server.
// Lets a user enter SSID and password from any browser.
// Persists credentials and triggers a reboot so STA reconnects.

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t WifiProv_RegisterHandlers(httpd_handle_t sHttpServer);
