#pragma once
#include "esp_err.h"

/**
 * Startet den HTTP-Server.
 * Aufrufen NACH sbb_wifi_init() und NTP-Sync.
 */
esp_err_t http_server_start(void);

/**
 * Stoppt den HTTP-Server (optional beim Schlafen).
 */
void http_server_stop(void);
