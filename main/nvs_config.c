#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nvs_config";
#define NVS_NS "blink_cfg"

// ===== DEFAULTS =====
void nvs_config_defaults(blink_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    // Zeitfenster: 1 Fenster 06:45–06:55
    cfg->timeWindowCount    = 1;
    cfg->timeWindows[0]     = (time_window_t){6, 45, 7, 0};

    // Button
    cfg->buttonActiveMin    = 2;
    cfg->buttonLongPressMs  = 3000;
    cfg->buttonLongActiveMin= 10;
    cfg->buttonGpio         = 0;

    // Netzwerk
    strncpy(cfg->ssid,     "",             sizeof(cfg->ssid));
    strncpy(cfg->password, "",             sizeof(cfg->password));
    cfg->ntpTimeoutS        = 5;
    strncpy(cfg->station,  "Gelterkinden", sizeof(cfg->station));

    // Ziel-Filter
    cfg->destFilterCount    = 0;

    // Schlaf
    cfg->sleepEnabled       = true;
    cfg->sleepFallbackS     = 300;
    cfg->sleepAfterS        = 300;
    cfg->sleepMaxMin        = 120;
    cfg->weekendSleepEnabled= true;
    cfg->weekendStartDay    = 5;     // Fr
    cfg->weekendStartH      = 18;
    cfg->weekendStartM      = 0;
    cfg->weekendEndDay      = 1;     // Mo
    cfg->weekendEndH        = 5;
    cfg->weekendEndM        = 0;

    // Hardware
    cfg->ledGpio            = 48;
    cfg->sdaGpio            = 4;
    cfg->sclGpio            = 5;
    strncpy(cfg->oledAddr, "0x3C",         sizeof(cfg->oledAddr));
    cfg->oledInvertMin      = 5;

    // LED-Farben
    cfg->ledOkRgb[0]        = 0;   cfg->ledOkRgb[1]        = 255; cfg->ledOkRgb[2]        = 0;
    cfg->ledDelaySmallRgb[0]= 0;   cfg->ledDelaySmallRgb[1]= 255; cfg->ledDelaySmallRgb[2]= 255;
    cfg->ledDelayBigRgb[0]  = 128; cfg->ledDelayBigRgb[1]  = 0;   cfg->ledDelayBigRgb[2]  = 255;
    cfg->ledCancelledRgb[0] = 255; cfg->ledCancelledRgb[1] = 0;   cfg->ledCancelledRgb[2] = 0;
    cfg->ledLoadingRgb[0]   = 255; cfg->ledLoadingRgb[1]   = 128; cfg->ledLoadingRgb[2]   = 0;
    cfg->ledErrorBlinkMs    = 500;

    // Verspätungs-Schwellen
    cfg->delaySmallMin      = 2;
    cfg->delayBigMin        = 6;

    // Adaptiver Refresh
    cfg->refreshNearSec     = 30;
    cfg->refreshMidSec      = 120;
    cfg->refreshFarSec      = 300;
    cfg->refreshVeryfarSec  = 600;
    cfg->refreshNearMin     = 5;
    cfg->refreshMidMin      = 10;
    cfg->refreshFarMin      = 30;

    // API
    cfg->apiRetryCount      = 3;
    cfg->apiRetryDelayS     = 5;
    cfg->staleMaxMin        = 10;

    // Verhalten
    cfg->weekdaysOnly       = true;
}

// ===== LOAD =====
esp_err_t nvs_config_load(blink_config_t *cfg) {
    nvs_config_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Kein NVS-Config — Defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    int32_t v; uint8_t bv; size_t len;

    #define LI(key, field) if (nvs_get_i32(h, key, &v) == ESP_OK) cfg->field = (int)v;
    #define LB(key, field) if (nvs_get_u8(h,  key, &bv) == ESP_OK) cfg->field = (bool)bv;
    #define LS(key, field) len = sizeof(cfg->field); nvs_get_str(h, key, cfg->field, &len);

    // Zeitfenster-Anzahl
    LI("twCount", timeWindowCount)
    if (cfg->timeWindowCount < 1) cfg->timeWindowCount = 1;
    if (cfg->timeWindowCount > 8) cfg->timeWindowCount = 8;

    // Zeitfenster-Einträge
    for (int i = 0; i < cfg->timeWindowCount; i++) {
        char key[16];
        snprintf(key, sizeof(key), "tw%d_sh", i); LI(key, timeWindows[i].startH)
        snprintf(key, sizeof(key), "tw%d_sm", i); LI(key, timeWindows[i].startM)
        snprintf(key, sizeof(key), "tw%d_eh", i); LI(key, timeWindows[i].endH)
        snprintf(key, sizeof(key), "tw%d_em", i); LI(key, timeWindows[i].endM)
    }

    // Button
    LI("btnActiveMin",    buttonActiveMin)
    LI("btnLongMs",     buttonLongPressMs)
    LI("btnLongMin",    buttonLongActiveMin)
    LI("btnGpio",       buttonGpio)

    // Netzwerk
    LS("ssid",          ssid)
    LS("password",      password)
    LI("ntpTimeoutS",   ntpTimeoutS)
    LS("station",       station)

    // Ziel-Filter
    LI("filtCount",     destFilterCount)
    if (cfg->destFilterCount < 0) cfg->destFilterCount = 0;
    if (cfg->destFilterCount > 4) cfg->destFilterCount = 4;
    for (int i = 0; i < cfg->destFilterCount; i++) {
        char key[16];
        snprintf(key, sizeof(key), "filt%d", i);
        len = sizeof(cfg->destFilters[i]);
        nvs_get_str(h, key, cfg->destFilters[i], &len);
    }

    // Schlaf
    LB("sleepEn",       sleepEnabled)
    LI("sleepFbS",      sleepFallbackS)
    LI("sleepAfterS",   sleepAfterS)
    LI("sleepMaxMin",   sleepMaxMin)
    LB("weSlpEn",       weekendSleepEnabled)
    LI("weStartDay",    weekendStartDay)
    LI("weStartH",      weekendStartH)
    LI("weStartM",      weekendStartM)
    LI("weEndDay",      weekendEndDay)
    LI("weEndH",        weekendEndH)
    LI("weEndM",        weekendEndM)

    // Hardware
    LI("ledGpio",       ledGpio)
    LI("sdaGpio",       sdaGpio)
    LI("sclGpio",       sclGpio)
    LS("oledAddr",      oledAddr)
    LI("oledInvMin",    oledInvertMin)

    // LED-Farben als blob (3 bytes je Farbe)
    #define LRGB(key, arr) { size_t sz = 3; nvs_get_blob(h, key, cfg->arr, &sz); }
    LRGB("ledOk",       ledOkRgb)
    LRGB("ledDlySm",    ledDelaySmallRgb)
    LRGB("ledDlyBig",   ledDelayBigRgb)
    LRGB("ledCancel",   ledCancelledRgb)
    LRGB("ledLoad",     ledLoadingRgb)
    LI("ledErrBlMs",    ledErrorBlinkMs)

    // Verspätungs-Schwellen
    LI("dlySmlMin",     delaySmallMin)
    LI("dlyBigMin",     delayBigMin)

    // Adaptiver Refresh
    LI("rfNearSec",     refreshNearSec)
    LI("rfMidSec",      refreshMidSec)
    LI("rfFarSec",      refreshFarSec)
    LI("rfVfarSec",     refreshVeryfarSec)
    LI("rfNearMin",     refreshNearMin)
    LI("rfMidMin",      refreshMidMin)
    LI("rfFarMin",      refreshFarMin)

    // API
    LI("apiRetry",      apiRetryCount)
    LI("apiRetDlyS",    apiRetryDelayS)
    LI("staleMaxMin",   staleMaxMin)

    // Verhalten
    LB("wdOnly",        weekdaysOnly)

    nvs_close(h);
    ESP_LOGI(TAG, "Config geladen: %d Zeitfenster, Station=%s",
             cfg->timeWindowCount, cfg->station);
    return ESP_OK;
}

// ===== SAVE =====
esp_err_t nvs_config_save(const blink_config_t *cfg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    #define SI(key, field) nvs_set_i32(h, key, (int32_t)cfg->field);
    #define SB(key, field) nvs_set_u8(h,  key, (uint8_t)cfg->field);
    #define SS(key, field) nvs_set_str(h, key, cfg->field);
    #define SRGB(key, arr) nvs_set_blob(h, key, cfg->arr, 3);

    // Zeitfenster
    SI("twCount",       timeWindowCount)
    for (int i = 0; i < 8; i++) {
        char key[16];
        snprintf(key, sizeof(key), "tw%d_sh", i); SI(key, timeWindows[i].startH)
        snprintf(key, sizeof(key), "tw%d_sm", i); SI(key, timeWindows[i].startM)
        snprintf(key, sizeof(key), "tw%d_eh", i); SI(key, timeWindows[i].endH)
        snprintf(key, sizeof(key), "tw%d_em", i); SI(key, timeWindows[i].endM)
    }

    // Button
    SI("btnActiveMin",    buttonActiveMin)
    SI("btnLongMs",     buttonLongPressMs)
    SI("btnLongMin",    buttonLongActiveMin)
    SI("btnGpio",       buttonGpio)

    // Netzwerk
    SS("ssid",          ssid)
    SS("password",      password)
    SI("ntpTimeoutS",   ntpTimeoutS)
    SS("station",       station)

    // Ziel-Filter
    SI("filtCount",     destFilterCount)
    for (int i = 0; i < 4; i++) {
        char key[16];
        snprintf(key, sizeof(key), "filt%d", i);
        nvs_set_str(h, key, cfg->destFilters[i]);
    }

    // Schlaf
    SB("sleepEn",       sleepEnabled)
    SI("sleepFbS",      sleepFallbackS)
    SI("sleepAfterS",   sleepAfterS)
    SI("sleepMaxMin",   sleepMaxMin)
    SB("weSlpEn",       weekendSleepEnabled)
    SI("weStartDay",    weekendStartDay)
    SI("weStartH",      weekendStartH)
    SI("weStartM",      weekendStartM)
    SI("weEndDay",      weekendEndDay)
    SI("weEndH",        weekendEndH)
    SI("weEndM",        weekendEndM)

    // Hardware
    SI("ledGpio",       ledGpio)
    SI("sdaGpio",       sdaGpio)
    SI("sclGpio",       sclGpio)
    SS("oledAddr",      oledAddr)
    SI("oledInvMin",    oledInvertMin)

    // LED-Farben
    SRGB("ledOk",       ledOkRgb)
    SRGB("ledDlySm",    ledDelaySmallRgb)
    SRGB("ledDlyBig",   ledDelayBigRgb)
    SRGB("ledCancel",   ledCancelledRgb)
    SRGB("ledLoad",     ledLoadingRgb)
    SI("ledErrBlMs",    ledErrorBlinkMs)

    // Verspätungs-Schwellen
    SI("dlySmlMin",     delaySmallMin)
    SI("dlyBigMin",     delayBigMin)

    // Adaptiver Refresh
    SI("rfNearSec",     refreshNearSec)
    SI("rfMidSec",      refreshMidSec)
    SI("rfFarSec",      refreshFarSec)
    SI("rfVfarSec",     refreshVeryfarSec)
    SI("rfNearMin",     refreshNearMin)
    SI("rfMidMin",      refreshMidMin)
    SI("rfFarMin",      refreshFarMin)

    // API
    SI("apiRetry",      apiRetryCount)
    SI("apiRetDlyS",    apiRetryDelayS)
    SI("staleMaxMin",   staleMaxMin)

    // Verhalten
    SB("wdOnly",        weekdaysOnly)

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Config gespeichert");
    return err;
}
