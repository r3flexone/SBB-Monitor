#pragma once
#include <stdbool.h>
#include "esp_err.h"

// Konfigurationsstruktur — spiegelt das JSON des Web-Panels
typedef struct {
    int   startH;
    int   startM;
    int   endH;
    int   endM;
    int   buttonActiveS;   // Sekunden (Panel sendet Sekunden)
    int   refreshS;        // Sekunden
    char  ssid[64];
    char  password[64];
    int   ntpTimeoutS;     // Sekunden
    char  station[64];
    bool  sleepEnabled;
    int   sleepFallbackS;
    int   sleepAfterS;
    int   sleepMaxMin;
    int   ledGpio;
    int   sdaGpio;
    int   sclGpio;
    char  oledAddr[8];     // z.B. "0x3C"
    int   buttonGpio;
} blink_config_t;

/**
 * Lädt Konfiguration aus NVS.
 * Füllt *cfg mit gespeicherten Werten oder Defaults wenn leer.
 */
esp_err_t nvs_config_load(blink_config_t *cfg);

/**
 * Speichert Konfiguration in NVS.
 */
esp_err_t nvs_config_save(const blink_config_t *cfg);

/**
 * Setzt alle Werte auf Firmware-Defaults zurück.
 */
void nvs_config_defaults(blink_config_t *cfg);
