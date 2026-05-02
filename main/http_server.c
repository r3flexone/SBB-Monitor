#include "http_server.h"
#include "nvs_config.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "http_server";
static httpd_handle_t server = NULL;

// ===== Hilfsfunktion: Datei aus SPIFFS streamen =====
static esp_err_t send_file(httpd_req_t *req, const char *path, const char *mime) {
    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, mime);
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        httpd_resp_send_chunk(req, buf, n);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ===== Hilfsfunktion: RGB uint8[3] → "#RRGGBB" =====
static void rgb_to_hex(const uint8_t rgb[3], char *out) {
    snprintf(out, 8, "#%02X%02X%02X", rgb[0], rgb[1], rgb[2]);
}

// ===== Hilfsfunktion: "#RRGGBB" → RGB uint8[3] =====
static void hex_to_rgb(const char *hex, uint8_t rgb[3]) {
    if (!hex || hex[0] != '#' || strlen(hex) < 7) return;
    unsigned r, g, b;
    if (sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
        rgb[0] = (uint8_t)r;
        rgb[1] = (uint8_t)g;
        rgb[2] = (uint8_t)b;
    }
}

// ===== GET / → index.html =====
static esp_err_t handler_root(httpd_req_t *req) {
    return send_file(req, "/spiffs/index.html", "text/html");
}

// ===== GET /api/config → vollständiges JSON =====
static esp_err_t handler_config_get(httpd_req_t *req) {
    blink_config_t cfg;
    nvs_config_load(&cfg);

    cJSON *j = cJSON_CreateObject();

    // Zeitfenster-Array
    cJSON *tws = cJSON_AddArrayToObject(j, "timeWindows");
    for (int i = 0; i < cfg.timeWindowCount; i++) {
        cJSON *tw = cJSON_CreateObject();
        cJSON_AddNumberToObject(tw, "startH", cfg.timeWindows[i].startH);
        cJSON_AddNumberToObject(tw, "startM", cfg.timeWindows[i].startM);
        cJSON_AddNumberToObject(tw, "endH",   cfg.timeWindows[i].endH);
        cJSON_AddNumberToObject(tw, "endM",   cfg.timeWindows[i].endM);
        cJSON_AddItemToArray(tws, tw);
    }

    // Button
    cJSON_AddNumberToObject(j, "buttonActiveMin",      cfg.buttonActiveMin);
    cJSON_AddNumberToObject(j, "buttonLongPressMs",   cfg.buttonLongPressMs);
    cJSON_AddNumberToObject(j, "buttonLongActiveMin", cfg.buttonLongActiveMin);
    cJSON_AddNumberToObject(j, "buttonGpio",          cfg.buttonGpio);

    // Netzwerk (Passwort NICHT senden)
    cJSON_AddStringToObject(j, "ssid",       cfg.ssid);
    cJSON_AddNumberToObject(j, "ntpTimeoutS",cfg.ntpTimeoutS);
    cJSON_AddStringToObject(j, "station",    cfg.station);

    // Ziel-Filter
    cJSON *filters = cJSON_AddArrayToObject(j, "destFilters");
    for (int i = 0; i < cfg.destFilterCount; i++)
        cJSON_AddItemToArray(filters, cJSON_CreateString(cfg.destFilters[i]));
    cJSON_AddNumberToObject(j, "destFilterCount", cfg.destFilterCount);

    // Schlaf
    cJSON_AddBoolToObject(j,   "sleepEnabled",   cfg.sleepEnabled);
    cJSON_AddNumberToObject(j, "sleepFallbackS", cfg.sleepFallbackS);
    cJSON_AddNumberToObject(j, "sleepAfterS",    cfg.sleepAfterS);
    cJSON_AddNumberToObject(j, "sleepMaxMin",    cfg.sleepMaxMin);

    // Hardware
    cJSON_AddNumberToObject(j, "ledGpio",     cfg.ledGpio);
    cJSON_AddNumberToObject(j, "sdaGpio",     cfg.sdaGpio);
    cJSON_AddNumberToObject(j, "sclGpio",     cfg.sclGpio);
    cJSON_AddStringToObject(j, "oledAddr",    cfg.oledAddr);
    cJSON_AddNumberToObject(j, "oledInvertMin", cfg.oledInvertMin);

    // LED-Farben als "#RRGGBB"
    char hex[8];
    rgb_to_hex(cfg.ledOkRgb,         hex); cJSON_AddStringToObject(j, "ledOkColor",         hex);
    rgb_to_hex(cfg.ledDelaySmallRgb, hex); cJSON_AddStringToObject(j, "ledDelaySmallColor", hex);
    rgb_to_hex(cfg.ledDelayBigRgb,   hex); cJSON_AddStringToObject(j, "ledDelayBigColor",   hex);
    rgb_to_hex(cfg.ledCancelledRgb,  hex); cJSON_AddStringToObject(j, "ledCancelledColor",  hex);
    rgb_to_hex(cfg.ledLoadingRgb,    hex); cJSON_AddStringToObject(j, "ledLoadingColor",     hex);
    cJSON_AddNumberToObject(j, "ledErrorBlinkMs", cfg.ledErrorBlinkMs);

    // Verspätungs-Schwellen
    cJSON_AddNumberToObject(j, "delaySmallMin", cfg.delaySmallMin);
    cJSON_AddNumberToObject(j, "delayBigMin",   cfg.delayBigMin);

    // Adaptiver Refresh
    cJSON_AddNumberToObject(j, "refreshNearSec",   cfg.refreshNearSec);
    cJSON_AddNumberToObject(j, "refreshMidSec",    cfg.refreshMidSec);
    cJSON_AddNumberToObject(j, "refreshFarSec",    cfg.refreshFarSec);
    cJSON_AddNumberToObject(j, "refreshVeryfarSec",cfg.refreshVeryfarSec);
    cJSON_AddNumberToObject(j, "refreshNearMin",   cfg.refreshNearMin);
    cJSON_AddNumberToObject(j, "refreshMidMin",    cfg.refreshMidMin);
    cJSON_AddNumberToObject(j, "refreshFarMin",    cfg.refreshFarMin);

    // API
    cJSON_AddNumberToObject(j, "apiRetryCount",  cfg.apiRetryCount);
    cJSON_AddNumberToObject(j, "apiRetryDelayS", cfg.apiRetryDelayS);
    cJSON_AddNumberToObject(j, "staleMaxMin",    cfg.staleMaxMin);

    // Verhalten
    cJSON_AddBoolToObject(j, "weekdaysOnly", cfg.weekdaysOnly);

    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, body);
    free(body);
    return ESP_OK;
}

// ===== POST /api/config =====
static esp_err_t handler_config_post(httpd_req_t *req) {
    char buf[4096];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    blink_config_t cfg;
    nvs_config_load(&cfg);

    #define GI(key, field) { cJSON *v=cJSON_GetObjectItem(j,key); if(cJSON_IsNumber(v)) cfg.field=(int)v->valuedouble; }
    #define GB(key, field) { cJSON *v=cJSON_GetObjectItem(j,key); if(cJSON_IsBool(v))   cfg.field=cJSON_IsTrue(v); }
    #define GS(key, field) { cJSON *v=cJSON_GetObjectItem(j,key); if(cJSON_IsString(v)&&v->valuestring) strncpy(cfg.field,v->valuestring,sizeof(cfg.field)-1); }
    #define GRGB(key, arr) { cJSON *v=cJSON_GetObjectItem(j,key); if(cJSON_IsString(v)&&v->valuestring) hex_to_rgb(v->valuestring,cfg.arr); }

    // Zeitfenster-Array
    cJSON *tws = cJSON_GetObjectItem(j, "timeWindows");
    if (cJSON_IsArray(tws)) {
        int cnt = cJSON_GetArraySize(tws);
        if (cnt > 8) cnt = 8;
        cfg.timeWindowCount = cnt;
        for (int i = 0; i < cnt; i++) {
            cJSON *tw = cJSON_GetArrayItem(tws, i);
            cJSON *sh = cJSON_GetObjectItem(tw, "startH");
            cJSON *sm = cJSON_GetObjectItem(tw, "startM");
            cJSON *eh = cJSON_GetObjectItem(tw, "endH");
            cJSON *em = cJSON_GetObjectItem(tw, "endM");
            if (cJSON_IsNumber(sh)) cfg.timeWindows[i].startH = (int)sh->valuedouble;
            if (cJSON_IsNumber(sm)) cfg.timeWindows[i].startM = (int)sm->valuedouble;
            if (cJSON_IsNumber(eh)) cfg.timeWindows[i].endH   = (int)eh->valuedouble;
            if (cJSON_IsNumber(em)) cfg.timeWindows[i].endM   = (int)em->valuedouble;
        }
    }

    // Button
    GI("buttonActiveMin",       buttonActiveMin)
    GI("buttonLongPressMs",   buttonLongPressMs)
    GI("buttonLongActiveMin", buttonLongActiveMin)
    GI("buttonGpio",          buttonGpio)

    // Netzwerk
    GS("ssid",        ssid)
    GS("password",    password)
    GI("ntpTimeoutS", ntpTimeoutS)
    GS("station",     station)

    // Ziel-Filter
    cJSON *filters = cJSON_GetObjectItem(j, "destFilters");
    if (cJSON_IsArray(filters)) {
        int cnt = cJSON_GetArraySize(filters);
        if (cnt > 4) cnt = 4;
        cfg.destFilterCount = cnt;
        for (int i = 0; i < cnt; i++) {
            cJSON *f = cJSON_GetArrayItem(filters, i);
            if (cJSON_IsString(f) && f->valuestring)
                strncpy(cfg.destFilters[i], f->valuestring, sizeof(cfg.destFilters[i]) - 1);
        }
    }

    // Schlaf
    GB("sleepEnabled",   sleepEnabled)
    GI("sleepFallbackS", sleepFallbackS)
    GI("sleepAfterS",    sleepAfterS)
    GI("sleepMaxMin",    sleepMaxMin)

    // Hardware
    GI("ledGpio",      ledGpio)
    GI("sdaGpio",      sdaGpio)
    GI("sclGpio",      sclGpio)
    GS("oledAddr",     oledAddr)
    GI("oledInvertMin",oledInvertMin)

    // LED-Farben
    GRGB("ledOkColor",         ledOkRgb)
    GRGB("ledDelaySmallColor", ledDelaySmallRgb)
    GRGB("ledDelayBigColor",   ledDelayBigRgb)
    GRGB("ledCancelledColor",  ledCancelledRgb)
    GRGB("ledLoadingColor",    ledLoadingRgb)
    GI("ledErrorBlinkMs",      ledErrorBlinkMs)

    // Verspätungs-Schwellen
    GI("delaySmallMin", delaySmallMin)
    GI("delayBigMin",   delayBigMin)

    // Adaptiver Refresh
    GI("refreshNearSec",    refreshNearSec)
    GI("refreshMidSec",     refreshMidSec)
    GI("refreshFarSec",     refreshFarSec)
    GI("refreshVeryfarSec", refreshVeryfarSec)
    GI("refreshNearMin",    refreshNearMin)
    GI("refreshMidMin",     refreshMidMin)
    GI("refreshFarMin",     refreshFarMin)

    // API
    GI("apiRetryCount",  apiRetryCount)
    GI("apiRetryDelayS", apiRetryDelayS)
    GI("staleMaxMin",    staleMaxMin)

    // Verhalten
    GB("weekdaysOnly", weekdaysOnly)

    cJSON_Delete(j);

    esp_err_t err = nvs_config_save(&cfg);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
    }
    return err;
}

// ===== GET /api/status =====
static esp_err_t handler_status_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"wifi\":true,\"ntp\":true}");
    return ESP_OK;
}

// ===== Server starten =====
esp_err_t http_server_start(void) {
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPIFFS mount fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;

    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start fehlgeschlagen");
        return ESP_FAIL;
    }

    httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = handler_root },
        { .uri = "/api/config", .method = HTTP_GET,  .handler = handler_config_get },
        { .uri = "/api/config", .method = HTTP_POST, .handler = handler_config_post },
        { .uri = "/api/status", .method = HTTP_GET,  .handler = handler_status_get },
    };
    for (int i = 0; i < 4; i++)
        httpd_register_uri_handler(server, &uris[i]);

    ESP_LOGI(TAG, "HTTP server gestartet — http://blink.local");
    return ESP_OK;
}

void http_server_stop(void) {
    if (server) { httpd_stop(server); server = NULL; }
    esp_vfs_spiffs_unregister(NULL);
}
