// Implements a minimal DNS responder for captive portal support on the ESP32 SoftAP.
// Replies to all A queries with the SoftAP IP so clients resolve any hostname locally.
// Provides start/stop APIs used by the Wi-Fi manager during provisioning mode.

#pragma once

#include "esp_err.h"

esp_err_t DnsCaptive_Start(void);
esp_err_t DnsCaptive_Stop(void);
