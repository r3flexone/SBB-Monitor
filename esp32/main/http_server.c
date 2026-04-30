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

// ===== Hilfsfunktion: Datei aus SPIFFS lesen und senden =====
static esp_err_t send_file(httpd_req_t *req, const char *path, const char *mime) {
    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, mime);
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, n);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ===== GET / → index.html =====
static esp_err_t handler_root(httpd_req_t *req) {
    return send_file(req, "/spiffs/index.html", "text/html");
}

// ===== GET /api/config → JSON =====
static esp_err_t handler_config_get(httpd_req_t *req) {
    blink_config_t cfg;
    nvs_config_load(&cfg);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "startH",         cfg.startH);
    cJSON_AddNumberToObject(j, "startM",         cfg.startM);
    cJSON_AddNumberToObject(j, "endH",           cfg.endH);
    cJSON_AddNumberToObject(j, "endM",           cfg.endM);
    cJSON_AddNumberToObject(j, "buttonActiveS",  cfg.buttonActiveS);
    cJSON_AddNumberToObject(j, "refreshS",       cfg.refreshS);
    cJSON_AddStringToObject(j, "ssid",           cfg.ssid);
    // Passwort NICHT zurückschicken
    cJSON_AddNumberToObject(j, "ntpTimeoutS",    cfg.ntpTimeoutS);
    cJSON_AddStringToObject(j, "station",        cfg.station);
    cJSON_AddBoolToObject(j,   "sleepEnabled",   cfg.sleepEnabled);
    cJSON_AddNumberToObject(j, "sleepFallbackS", cfg.sleepFallbackS);
    cJSON_AddNumberToObject(j, "sleepAfterS",    cfg.sleepAfterS);
    cJSON_AddNumberToObject(j, "sleepMaxMin",    cfg.sleepMaxMin);
    cJSON_AddNumberToObject(j, "ledGpio",        cfg.ledGpio);
    cJSON_AddNumberToObject(j, "sdaGpio",        cfg.sdaGpio);
    cJSON_AddNumberToObject(j, "sclGpio",        cfg.sclGpio);
    cJSON_AddStringToObject(j, "oledAddr",       cfg.oledAddr);
    cJSON_AddNumberToObject(j, "buttonGpio",     cfg.buttonGpio);

    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, body);
    free(body);
    return ESP_OK;
}

// ===== POST /api/config → JSON empfangen & speichern =====
static esp_err_t handler_config_post(httpd_req_t *req) {
    char buf[1024];
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
    nvs_config_load(&cfg); // Erst laden, dann überschreiben

    #define GET_INT(key, field) { \
        cJSON *v = cJSON_GetObjectItem(j, key); \
        if (cJSON_IsNumber(v)) cfg.field = (int)v->valuedouble; \
    }
    #define GET_STR(key, field) { \
        cJSON *v = cJSON_GetObjectItem(j, key); \
        if (cJSON_IsString(v) && v->valuestring) \
            strncpy(cfg.field, v->valuestring, sizeof(cfg.field) - 1); \
    }
    #define GET_BOOL(key, field) { \
        cJSON *v = cJSON_GetObjectItem(j, key); \
        if (cJSON_IsBool(v)) cfg.field = cJSON_IsTrue(v); \
    }

    GET_INT("startH",         startH)
    GET_INT("startM",         startM)
    GET_INT("endH",           endH)
    GET_INT("endM",           endM)
    GET_INT("buttonActiveS",  buttonActiveS)
    GET_INT("refreshS",       refreshS)
    GET_STR("ssid",           ssid)
    GET_STR("password",       password)
    GET_INT("ntpTimeoutS",    ntpTimeoutS)
    GET_STR("station",        station)
    GET_BOOL("sleepEnabled",  sleepEnabled)
    GET_INT("sleepFallbackS", sleepFallbackS)
    GET_INT("sleepAfterS",    sleepAfterS)
    GET_INT("sleepMaxMin",    sleepMaxMin)
    GET_INT("ledGpio",        ledGpio)
    GET_INT("sdaGpio",        sdaGpio)
    GET_INT("sclGpio",        sclGpio)
    GET_STR("oledAddr",       oledAddr)
    GET_INT("buttonGpio",     buttonGpio)

    cJSON_Delete(j);

    esp_err_t err = nvs_config_save(&cfg);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (err == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
    }
    return err;
}

// ===== GET /api/status → Live-Status =====
static esp_err_t handler_status_get(httpd_req_t *req) {
    // Hier kannst du später echte LED-Farbe, Abfahrten etc. einhängen
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"wifi\":true,\"ntp\":true}");
    return ESP_OK;
}

// ===== Server starten =====
esp_err_t http_server_start(void) {
    // SPIFFS mounten
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
    for (int i = 0; i < 4; i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP server gestartet — http://blink.local");
    return ESP_OK;
}

void http_server_stop(void) {
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    esp_vfs_spiffs_unregister(NULL);
}
