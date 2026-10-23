#pragma once

#include "esp_err.h"

/*
 * Processes one KeePassHTTP JSON request. The caller owns the returned
 * heap-allocated response and must release it with free().
 */
esp_err_t keepass_http_protocol_handle(const char *request, char **response);

