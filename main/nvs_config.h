#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// ===== Zeitfenster-Eintrag =====
typedef struct {
    int startH;
    int startM;
    int endH;
    int endM;
} time_window_t;

// ===== Haupt-Konfigurationsstruktur =====
typedef struct {

    // --- Zeitfenster (bis zu 8) ---
    time_window_t timeWindows[8];
    int           timeWindowCount;   // 1–8

    // --- Button ---
    int  buttonActiveMin;            // Minuten nach kurzem Druck
    int  buttonLongPressMs;          // Long-press Schwelle (ms)
    int  buttonLongActiveMin;        // Dauer nach long press (min)
    int  buttonGpio;

    // --- Netzwerk ---
    char ssid[64];
    char password[64];
    int  ntpTimeoutS;
    char station[64];
    char panelPass[32];              // Web-Panel-Login (leer = kein Login, Default)

    // --- Ziel-Filter (Substring, case-insensitive) ---
    char destFilters[4][32];
    int  destFilterCount;            // 0 = alle Züge

    // --- Schlaf ---
    bool sleepEnabled;
    int  sleepFallbackS;
    int  sleepAfterS;
    int  sleepMaxMin;

    // --- Hardware ---
    int  ledGpio;
    int  sdaGpio;
    int  sclGpio;
    char oledAddr[8];                // z.B. "0x3C"

    // --- OLED Burn-in ---
    int  oledInvertMin;              // 0 = aus

    // --- LED-Farben (RGB, je 0–255) ---
    uint8_t ledOkRgb[3];            // pünktlich         → grün
    uint8_t ledDelaySmallRgb[3];    // leicht verspätet  → cyan
    uint8_t ledDelayBigRgb[3];      // stark verspätet   → lila
    uint8_t ledCancelledRgb[3];     // Ausfall           → rot
    uint8_t ledLoadingRgb[3];       // Verbinden         → orange
    int     ledErrorBlinkMs;        // Blink-Periode bei Fehler (ms)

    // --- Verspätungs-Schwellen ---
    int  delaySmallMin;             // ab wann cyan (min)
    int  delayBigMin;               // ab wann lila (min)

    // --- Adaptiver Refresh ---
    int  refreshNearSec;            // < refreshNearMin  → alle N sec
    int  refreshMidSec;             // < refreshMidMin   → alle N sec
    int  refreshFarSec;             // < refreshFarMin   → alle N sec
    int  refreshVeryfarSec;         // > refreshFarMin   → alle N sec
    int  refreshNearMin;            // Schwelle near (min)
    int  refreshMidMin;             // Schwelle mid  (min)
    int  refreshFarMin;             // Schwelle far  (min)

    // --- API ---
    int  apiRetryCount;             // Versuche bei Fehler
    int  apiRetryDelayS;            // Wartezeit zwischen Versuchen (s)
    int  staleMaxMin;               // Cache-Gültigkeit (min)

    // --- Verhalten ---
    bool weekdaysOnly;              // Nur Mo–Fr

    // --- Wochenend-Schlaf-Fenster ---
    bool weekendSleepEnabled;        // Wochenend-Schlaf aktiv (unabhängig von weekdaysOnly)
    int  weekendStartDay;            // Wochenend-Schlaf ab Wochentag (0=So,1=Mo,...,5=Fr,6=Sa)
    int  weekendStartH;              // Startzeit Stunde
    int  weekendStartM;              // Startzeit Minute
    int  weekendEndDay;              // Wochenend-Schlaf bis Wochentag
    int  weekendEndH;                // Endzeit Stunde
    int  weekendEndM;                // Endzeit Minute

} blink_config_t;

/**
 * Füllt *cfg mit Firmware-Defaults.
 */
void nvs_config_defaults(blink_config_t *cfg);

/**
 * Lädt Konfiguration aus NVS (füllt fehlende Werte mit Defaults).
 */
esp_err_t nvs_config_load(blink_config_t *cfg);

/**
 * Speichert vollständige Konfiguration in NVS.
 */
esp_err_t nvs_config_save(const blink_config_t *cfg);
