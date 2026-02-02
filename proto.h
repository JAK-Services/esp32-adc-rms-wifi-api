// Declares JSON builder functions used by HTTP API endpoints.
// Defines interfaces for serializing status and measurement structures to JSON.
// Keeps protocol formatting separated from web server and business logic.

#pragma once

#include <stddef.h>
#include "adc.h"
#include "wifi_mgr.h"

int Proto_BuildStatusJson(char *psBuffer, size_t szBuffer, wifi_mgr_state_t eState);
int Proto_BuildRmsJson(char *psBuffer, size_t szBuffer, const adc_result_t *psResult, bool bHasResult);
