#include "http_server.h"
#include "nvs_config.h"
#include "sbb.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "http_server";
static httpd_handle_t server = NULL;

// ===== Optionaler Panel-Login (HTTP Basic Auth) =====
// Leer = kein Login (Default fürs Heimnetz). Wird beim Serverstart aus NVS
// geladen und nach jedem Config-Save aktualisiert. Achtung: läuft über
// unverschlüsseltes HTTP — schützt vor Mitbewohnern, nicht vor Angreifern.
static char panel_pass[32] = "";

static void panel_pass_load(void) {
    nvs_handle_t h;
    panel_pass[0] = 0;
    // Namespace wie in nvs_config.c (NVS_NS)
    if (nvs_open("blink_cfg", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(panel_pass);
        if (nvs_get_str(h, "panelPass", panel_pass, &len) != ESP_OK)
            panel_pass[0] = 0;
        nvs_close(h);
    }
}

static bool auth_ok(httpd_req_t *req) {
    if (!panel_pass[0]) return true;
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    if (strncmp(hdr, "Basic ", 6) != 0) return false;
    unsigned char dec[96];
    size_t dlen = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &dlen,
                              (const unsigned char *)hdr + 6, strlen(hdr) - 6) != 0)
        return false;
    dec[dlen] = 0;
    const char *colon = strchr((const char *)dec, ':');   // "user:pass", User egal
    if (!colon) return false;
    return strcmp(colon + 1, panel_pass) == 0;
}

// ESP_OK = durchgelassen; sonst wurde bereits eine 401-Antwort gesendet.
static esp_err_t require_auth(httpd_req_t *req) {
    if (auth_ok(req)) return ESP_OK;
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"SBB-Monitor\"");
    httpd_resp_sendstr(req, "Login erforderlich");
    return ESP_FAIL;
}

// ===== Hilfsfunktion: Datei aus SPIFFS streamen =====
static esp_err_t send_file(httpd_req_t *req, const char *path, const char *mime) {
    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, mime);
    // Sonst zeigt der Browser nach einem Firmware-Update u.U. die alte Panel-Version
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
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
    if (require_auth(req) != ESP_OK) return ESP_OK;
    return send_file(req, "/spiffs/index.html", "text/html");
}

// ===== GET /api/config → vollständiges JSON =====
static esp_err_t handler_config_get(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_OK;
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

    // Netzwerk (Passwörter NICHT senden, nur ob Panel-Login aktiv ist)
    cJSON_AddStringToObject(j, "ssid",       cfg.ssid);
    cJSON_AddNumberToObject(j, "ntpTimeoutS",cfg.ntpTimeoutS);
    cJSON_AddStringToObject(j, "station",    cfg.station);
    cJSON_AddBoolToObject(j,   "panelAuthEnabled", cfg.panelPass[0] != 0);

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

    // Wochenend-Schlaf-Fenster
    cJSON_AddBoolToObject(j,   "weekendSleepEnabled", cfg.weekendSleepEnabled);
    cJSON_AddNumberToObject(j, "weekendStartDay", cfg.weekendStartDay);
    cJSON_AddNumberToObject(j, "weekendStartH",   cfg.weekendStartH);
    cJSON_AddNumberToObject(j, "weekendStartM",   cfg.weekendStartM);
    cJSON_AddNumberToObject(j, "weekendEndDay",   cfg.weekendEndDay);
    cJSON_AddNumberToObject(j, "weekendEndH",     cfg.weekendEndH);
    cJSON_AddNumberToObject(j, "weekendEndM",     cfg.weekendEndM);

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
    if (require_auth(req) != ESP_OK) return ESP_OK;
    // static: 4 KB passen schlecht in den 8-KB-httpd-Stack; der Server
    // verarbeitet Requests sequentiell, daher kein Race.
    static char buf[4096];
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
    #define GS(key, field) { cJSON *v=cJSON_GetObjectItem(j,key); if(cJSON_IsString(v)&&v->valuestring) { strncpy(cfg.field,v->valuestring,sizeof(cfg.field)-1); cfg.field[sizeof(cfg.field)-1]='\0'; } }
    #define GRGB(key, arr) { cJSON *v=cJSON_GetObjectItem(j,key); if(cJSON_IsString(v)&&v->valuestring) hex_to_rgb(v->valuestring,cfg.arr); }
    // GPIO nur in gültigem Bereich übernehmen — ein Tippfehler im Panel darf
    // das Gerät nicht in einen Panic-Boot-Loop schicken (ESP32-S3: GPIO 0–48,
    // Button braucht RTC-GPIO 0–21 für den Deep-Sleep-Wakeup).
    #define GGPIO(key, field, maxg) { cJSON *v=cJSON_GetObjectItem(j,key); if(cJSON_IsNumber(v)) { int g=(int)v->valuedouble; if(g>=0&&g<=(maxg)) cfg.field=g; else ESP_LOGW(TAG,"%s: GPIO %d ungueltig, behalte %d",key,g,cfg.field); } }

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
    GI("buttonActiveMin",     buttonActiveMin)
    GI("buttonLongPressMs",   buttonLongPressMs)
    GI("buttonLongActiveMin", buttonLongActiveMin)
    GGPIO("buttonGpio",       buttonGpio, 21)

    // Netzwerk
    GS("ssid",        ssid)
    // Geleerte SSID = explizit zurück auf secrets.h. Dann auch das gespeicherte
    // Passwort verwerfen — sonst entsteht die Kombination "SSID aus secrets.h
    // + altes NVS-Passwort" und der Connect schlägt fehl.
    {
        cJSON *v = cJSON_GetObjectItem(j, "ssid");
        if (cJSON_IsString(v) && v->valuestring && !v->valuestring[0])
            cfg.password[0] = '\0';
    }
    // Passwort: nur überschreiben wenn nicht leer.
    // GET sendet das Passwort aus Sicherheitsgründen nicht zurück, deshalb
    // ist das Feld im UI nach dem Laden leer. Würden wir den leeren String
    // übernehmen, würde jeder Save das bestehende Passwort löschen.
    {
        cJSON *v = cJSON_GetObjectItem(j, "password");
        if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
            strncpy(cfg.password, v->valuestring, sizeof(cfg.password) - 1);
            cfg.password[sizeof(cfg.password) - 1] = '\0';
        }
    }
    GI("ntpTimeoutS", ntpTimeoutS)
    GS("station",     station)
    // Panel-Login: leeres Feld = unverändert (wie WLAN-Passwort);
    // explizites Deaktivieren über panelPassClear:true.
    {
        cJSON *v = cJSON_GetObjectItem(j, "panelPass");
        if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
            strncpy(cfg.panelPass, v->valuestring, sizeof(cfg.panelPass) - 1);
            cfg.panelPass[sizeof(cfg.panelPass) - 1] = '\0';
        }
        if (cJSON_IsTrue(cJSON_GetObjectItem(j, "panelPassClear")))
            cfg.panelPass[0] = '\0';
    }

    // Ziel-Filter
    cJSON *filters = cJSON_GetObjectItem(j, "destFilters");
    if (cJSON_IsArray(filters)) {
        int cnt = cJSON_GetArraySize(filters);
        if (cnt > 4) cnt = 4;
        cfg.destFilterCount = cnt;
        for (int i = 0; i < cnt; i++) {
            cJSON *f = cJSON_GetArrayItem(filters, i);
            if (cJSON_IsString(f) && f->valuestring) {
                strncpy(cfg.destFilters[i], f->valuestring, sizeof(cfg.destFilters[i]) - 1);
                cfg.destFilters[i][sizeof(cfg.destFilters[i]) - 1] = '\0';
            }
        }
    }

    // Schlaf
    GB("sleepEnabled",   sleepEnabled)
    GI("sleepFallbackS", sleepFallbackS)
    GI("sleepAfterS",    sleepAfterS)
    GI("sleepMaxMin",    sleepMaxMin)

    // Hardware
    GGPIO("ledGpio",   ledGpio, 48)
    GGPIO("sdaGpio",   sdaGpio, 48)
    GGPIO("sclGpio",   sclGpio, 48)
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

    // Wochenend-Schlaf-Fenster
    GB("weekendSleepEnabled", weekendSleepEnabled)
    GI("weekendStartDay", weekendStartDay)
    GI("weekendStartH",   weekendStartH)
    GI("weekendStartM",   weekendStartM)
    GI("weekendEndDay",   weekendEndDay)
    GI("weekendEndH",     weekendEndH)
    GI("weekendEndM",     weekendEndM)

    cJSON_Delete(j);

    esp_err_t err = nvs_config_save(&cfg);
    if (err == ESP_OK) {
        extern volatile bool g_cfg_dirty;
        g_cfg_dirty = true;
        strncpy(panel_pass, cfg.panelPass, sizeof(panel_pass) - 1);
        panel_pass[sizeof(panel_pass) - 1] = '\0';
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
    }
    return err;
}

// ===== GET /api/departures — letzte geholte Abfahrten (Cache aus main.c) =====
static esp_err_t handler_departures_get(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_OK;
    time_t now; time(&now);

    cJSON *j = cJSON_CreateObject();
    // ageS = Sekunden seit der letzten erfolgreichen Abfrage, -1 = noch keine
    cJSON_AddNumberToObject(j, "ageS",
        g_last_deps_time ? (double)(now - g_last_deps_time) : -1);
    cJSON *arr = cJSON_AddArrayToObject(j, "departures");
    for (int i = 0; i < 4; i++) {
        if (!g_last_deps[i].valid) continue;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "time",        g_last_deps[i].time);
        cJSON_AddStringToObject(d, "destination", g_last_deps[i].destination);
        cJSON_AddStringToObject(d, "platform",    g_last_deps[i].platform);
        cJSON_AddNumberToObject(d, "delay",       g_last_deps[i].delay);
        cJSON_AddBoolToObject(d,   "cancelled",   g_last_deps[i].cancelled);
        cJSON_AddItemToArray(arr, d);
    }
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, body);
    free(body);
    return ESP_OK;
}

// ===== POST /api/restart =====
static esp_err_t handler_restart(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    ESP_LOGI(TAG, "Neustart per Web-Panel");
    // Response noch ausliefern lassen (gleiches Muster wie AP-Mode-Restart)
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ===== GET /api/status =====
static esp_err_t handler_status_get(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_OK;
    // Echte Werte: wifi = im STA-Modus verbunden (kein AP-Fallback),
    // ntp = gueltige Systemzeit vorhanden (tm_year >= 100 == ab Jahr 2000).
    bool wifi = !sbb_wifi_is_ap_mode();
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    bool ntp = (ti.tm_year >= 100);

    char body[40];
    snprintf(body, sizeof(body), "{\"wifi\":%s,\"ntp\":%s}",
             wifi ? "true" : "false", ntp ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, body);
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

    panel_pass_load();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;

    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start fehlgeschlagen");
        return ESP_FAIL;
    }

    httpd_uri_t uris[] = {
        { .uri = "/",               .method = HTTP_GET,  .handler = handler_root },
        { .uri = "/api/config",     .method = HTTP_GET,  .handler = handler_config_get },
        { .uri = "/api/config",     .method = HTTP_POST, .handler = handler_config_post },
        { .uri = "/api/status",     .method = HTTP_GET,  .handler = handler_status_get },
        { .uri = "/api/departures", .method = HTTP_GET,  .handler = handler_departures_get },
        { .uri = "/api/restart",    .method = HTTP_POST, .handler = handler_restart },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++)
        httpd_register_uri_handler(server, &uris[i]);

    ESP_LOGI(TAG, "HTTP server gestartet — http://sbb-monitor.local");
    return ESP_OK;
}

void http_server_stop(void) {
    if (server) { httpd_stop(server); server = NULL; }
    esp_vfs_spiffs_unregister(NULL);
}
