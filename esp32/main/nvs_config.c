#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nvs_config";
#define NVS_NAMESPACE "blink_cfg"

void nvs_config_defaults(blink_config_t *cfg) {
    cfg->startH         = 6;
    cfg->startM         = 45;
    cfg->endH           = 7;
    cfg->endM           = 0;
    cfg->buttonActiveS  = 600;
    cfg->refreshS       = 60;
    cfg->ntpTimeoutS    = 5;
    cfg->sleepEnabled   = true;
    cfg->sleepFallbackS = 300;
    cfg->sleepAfterS    = 300;
    cfg->sleepMaxMin    = 120;
    cfg->ledGpio        = 48;
    cfg->sdaGpio        = 4;
    cfg->sclGpio        = 5;
    cfg->buttonGpio     = 0;
    strncpy(cfg->ssid,     "",            sizeof(cfg->ssid));
    strncpy(cfg->password, "",            sizeof(cfg->password));
    strncpy(cfg->station,  "Gelterkinden",sizeof(cfg->station));
    strncpy(cfg->oledAddr, "0x3C",        sizeof(cfg->oledAddr));
}

esp_err_t nvs_config_load(blink_config_t *cfg) {
    nvs_config_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Kein gespeicherter Config — Defaults verwendet");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    // Integers
    int32_t v;
    #define LOAD_INT(key, field) \
        if (nvs_get_i32(h, key, &v) == ESP_OK) cfg->field = (int)v;

    LOAD_INT("startH",        startH)
    LOAD_INT("startM",        startM)
    LOAD_INT("endH",          endH)
    LOAD_INT("endM",          endM)
    LOAD_INT("buttonActiveS", buttonActiveS)
    LOAD_INT("refreshS",      refreshS)
    LOAD_INT("ntpTimeoutS",   ntpTimeoutS)
    LOAD_INT("sleepFallbackS",sleepFallbackS)
    LOAD_INT("sleepAfterS",   sleepAfterS)
    LOAD_INT("sleepMaxMin",   sleepMaxMin)
    LOAD_INT("ledGpio",       ledGpio)
    LOAD_INT("sdaGpio",       sdaGpio)
    LOAD_INT("sclGpio",       sclGpio)
    LOAD_INT("buttonGpio",    buttonGpio)

    uint8_t bv;
    if (nvs_get_u8(h, "sleepEnabled", &bv) == ESP_OK) cfg->sleepEnabled = (bool)bv;

    // Strings
    size_t len;
    #define LOAD_STR(key, field) \
        len = sizeof(cfg->field); \
        nvs_get_str(h, key, cfg->field, &len);

    LOAD_STR("ssid",     ssid)
    LOAD_STR("password", password)
    LOAD_STR("station",  station)
    LOAD_STR("oledAddr", oledAddr)

    nvs_close(h);
    ESP_LOGI(TAG, "Config geladen: Fenster %02d:%02d–%02d:%02d, Station=%s",
             cfg->startH, cfg->startM, cfg->endH, cfg->endM, cfg->station);
    return ESP_OK;
}

esp_err_t nvs_config_save(const blink_config_t *cfg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    #define SAVE_INT(key, field) nvs_set_i32(h, key, (int32_t)cfg->field);

    SAVE_INT("startH",        startH)
    SAVE_INT("startM",        startM)
    SAVE_INT("endH",          endH)
    SAVE_INT("endM",          endM)
    SAVE_INT("buttonActiveS", buttonActiveS)
    SAVE_INT("refreshS",      refreshS)
    SAVE_INT("ntpTimeoutS",   ntpTimeoutS)
    SAVE_INT("sleepFallbackS",sleepFallbackS)
    SAVE_INT("sleepAfterS",   sleepAfterS)
    SAVE_INT("sleepMaxMin",   sleepMaxMin)
    SAVE_INT("ledGpio",       ledGpio)
    SAVE_INT("sdaGpio",       sdaGpio)
    SAVE_INT("sclGpio",       sclGpio)
    SAVE_INT("buttonGpio",    buttonGpio)

    nvs_set_u8(h,  "sleepEnabled", (uint8_t)cfg->sleepEnabled);
    nvs_set_str(h, "ssid",     cfg->ssid);
    nvs_set_str(h, "password", cfg->password);
    nvs_set_str(h, "station",  cfg->station);
    nvs_set_str(h, "oledAddr", cfg->oledAddr);

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Config gespeichert");
    return err;
}
