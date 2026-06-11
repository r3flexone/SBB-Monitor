#include "sbb.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "sbb";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          5
#define HTTP_BUF_SIZE      32768
#define DEP_COUNT          4

static EventGroupHandle_t wifi_event_group;
static int retry_count = 0;
static bool wifi_ready = false;
static bool wifi_initialised = false;
static bool wifi_ap_mode = false;
static char *http_buf = NULL;
static int  http_buf_len = 0;

static void sbb_start_ap(void)
{
    esp_wifi_stop();
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = "SBB-Monitor",
            .ssid_len = 11,
            .channel = 6,
            .max_connection = 3,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();
    wifi_ap_mode = true;
    ESP_LOGI(TAG, "AP-Modus aktiv: SSID=SBB-Monitor IP=192.168.4.1");
}

// ===== WiFi =====

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Sonst meldet sbb_wifi_reconnect() weiter "verbunden" und tut nichts
        wifi_ready = false;
        if (retry_count < MAX_RETRY) {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGW(TAG, "WiFi retry %d/%d", retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        retry_count = 0;
        wifi_ready = true;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "WiFi verbunden!");
    }
}

void sbb_wifi_init(const char *ssid, const char *password)
{
    if (wifi_initialised) {
        ESP_LOGW(TAG, "WiFi schon initialisiert, skip");
        return;
    }
    wifi_initialised = true;

    wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t ia, ig;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, &ia);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, &ig);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "WiFi wird verbunden mit: %s", ssid);
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) ESP_LOGI(TAG, "WiFi OK");
    else { ESP_LOGE(TAG, "WiFi Fehler!"); wifi_ready = false; sbb_start_ap(); }
}

bool sbb_wifi_is_ap_mode(void) { return wifi_ap_mode; }

bool sbb_wifi_reconnect(void)
{
    if (wifi_ready) return true;
    if (!wifi_initialised) return false;
    ESP_LOGI(TAG, "WiFi Reconnect...");
    retry_count = 0;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

// ===== HTTP =====

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (http_buf_len + copy >= HTTP_BUF_SIZE - 1)
            copy = HTTP_BUF_SIZE - 1 - http_buf_len;
        memcpy(http_buf + http_buf_len, evt->data, copy);
        http_buf_len += copy;
        http_buf[http_buf_len] = 0;
    }
    return ESP_OK;
}

// ===== Zeit-Helpers =====

static int time_to_minutes(const char *hhmm)
{
    const char *t = strchr(hhmm, 'T');
    int h = 0, m = 0;
    int got = t ? sscanf(t + 1, "%d:%d", &h, &m)
                : sscanf(hhmm, "%d:%d", &h, &m);
    if (got != 2) return -1;
    return h * 60 + m;
}

static void format_time(const char *iso, char out[6])
{
    const char *t = strchr(iso, 'T');
    if (t) {
        int h = 0, m = 0;
        if (sscanf(t + 1, "%d:%d", &h, &m) == 2) {
            snprintf(out, 6, "%02d:%02d", h, m);
            return;
        }
    }
    // Fallback: erste 5 Zeichen übernehmen
    strncpy(out, iso, 5);
    out[5] = 0;
}

// ===== Filter-Helper =====

static bool str_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return false;
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen == 0) return true;
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            char h = haystack[i + j];
            char n = needle[j];
            if (h >= 'a' && h <= 'z') h -= 32;
            if (n >= 'a' && n <= 'z') n -= 32;
            if (h != n) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// ===== Hauptfunktion =====

bool sbb_get_departures(const char *station, SbbDeparture results[DEP_COUNT],
                        const char *dest_filters[], int filter_count)
{
    if (!wifi_ready) { ESP_LOGE(TAG, "WiFi nicht bereit!"); return false; }

    if (http_buf == NULL) {
        http_buf = malloc(HTTP_BUF_SIZE);
        if (!http_buf) { ESP_LOGE(TAG, "Kein RAM!"); return false; }
    }

    memset(results, 0, sizeof(SbbDeparture) * DEP_COUNT);
    http_buf_len = 0;
    http_buf[0] = 0;

    // Station vollständig URL-encodieren: nur unreserved (A-Z a-z 0-9 - _ . ~)
    // bleiben unverändert, alles andere wird zu %XX (inkl. UTF-8 für Umlaute).
    // 64-Zeichen-Station → max. 3 Bytes pro Zeichen, daher Puffer großzügig.
    static const char hexd[] = "0123456789ABCDEF";
    char station_enc[200];
    int si = 0;
    for (int i = 0; station[i] && si < (int)sizeof(station_enc) - 4; i++) {
        unsigned char ch = (unsigned char)station[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            station_enc[si++] = (char)ch;
        } else {
            station_enc[si++] = '%';
            station_enc[si++] = hexd[ch >> 4];
            station_enc[si++] = hexd[ch & 0x0F];
        }
    }
    station_enc[si] = 0;

    // passList nur laden wenn Filter aktiv ist (spart Daten)
    const char *pass_field = (filter_count > 0)
        ? "&fields[]=stationboard/passList/station/name"
        : "";

    char url[512];
    snprintf(url, sizeof(url),
        "http://transport.opendata.ch/v1/stationboard?station=%s&limit=15"
        "&transportations=train"
        "&fields[]=stationboard/stop/departure"
        "&fields[]=stationboard/stop/delay"
        "&fields[]=stationboard/stop/cancelled"
        "&fields[]=stationboard/stop/platform"
        "&fields[]=stationboard/to%s",
        station_enc, pass_field);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { ESP_LOGE(TAG, "HTTP init fehlgeschlagen"); return false; }
    esp_err_t err = esp_http_client_perform(client);

    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "HTTP Response (%d bytes)", http_buf_len);

    if (err != ESP_OK) { ESP_LOGE(TAG, "HTTP: %s", esp_err_to_name(err)); return false; }
    if (http_buf_len == 0) { ESP_LOGE(TAG, "Leere Antwort!"); return false; }

    cJSON *root = cJSON_Parse(http_buf);
    if (!root) { ESP_LOGE(TAG, "JSON Parse Fehler"); return false; }

    cJSON *stationboard = cJSON_GetObjectItem(root, "stationboard");
    if (!stationboard) { cJSON_Delete(root); return false; }

    time_t now; struct tm timeinfo;
    time(&now); localtime_r(&now, &timeinfo);
    int target_min = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int count = cJSON_GetArraySize(stationboard);

    typedef struct {
        char time[6];
        char destination[32];
        char platform[6];
        int  delay;
        bool cancelled;
        int  minutes;
    } Entry;

    // static: spart ~1 KB Stack (sbb_get_departures wird nur sequentiell aufgerufen)
    static Entry entries[20];
    memset(entries, 0, sizeof(entries));
    int n = 0;

    int total_trains = 0;
    for (int i = 0; i < count && n < 20; i++) {
        cJSON *item = cJSON_GetArrayItem(stationboard, i);
        if (!item) continue;
        cJSON *stop = cJSON_GetObjectItem(item, "stop");
        if (!stop) continue;
        cJSON *departure = cJSON_GetObjectItem(stop, "departure");
        if (!departure || !departure->valuestring) continue;
        total_trains++;

        cJSON *dest = cJSON_GetObjectItem(item, "to");
        cJSON *delay_json = cJSON_GetObjectItem(stop, "delay");
        cJSON *cancelled = cJSON_GetObjectItem(stop, "cancelled");
        cJSON *platform = cJSON_GetObjectItem(stop, "platform");

        // Filter prüfen: Endziel ODER Zwischenstation muss matchen
        bool matches = (filter_count == 0);
        if (!matches && dest && dest->valuestring) {
            for (int f = 0; f < filter_count; f++) {
                if (dest_filters[f] &&
                    str_contains_ci(dest->valuestring, dest_filters[f])) {
                    matches = true; break;
                }
            }
        }
        if (!matches) {
            cJSON *pass_list = cJSON_GetObjectItem(item, "passList");
            if (pass_list) {
                int pl = cJSON_GetArraySize(pass_list);
                for (int p = 0; p < pl && !matches; p++) {
                    cJSON *pi = cJSON_GetArrayItem(pass_list, p);
                    if (!pi) continue;
                    cJSON *ps = cJSON_GetObjectItem(pi, "station");
                    if (!ps) continue;
                    cJSON *pn = cJSON_GetObjectItem(ps, "name");
                    if (!pn || !pn->valuestring) continue;
                    for (int f = 0; f < filter_count; f++) {
                        if (dest_filters[f] &&
                            str_contains_ci(pn->valuestring, dest_filters[f])) {
                            matches = true; break;
                        }
                    }
                }
            }
        }
        if (!matches) continue;

        entries[n].cancelled = cancelled && cJSON_IsTrue(cancelled);
        entries[n].minutes = time_to_minutes(departure->valuestring);
        format_time(departure->valuestring, entries[n].time);
        entries[n].delay = delay_json ? delay_json->valueint : 0;

        if (dest && dest->valuestring) {
            strncpy(entries[n].destination, dest->valuestring, 31);
            entries[n].destination[31] = 0;
        } else {
            strcpy(entries[n].destination, "?");
        }

        if (platform && cJSON_IsString(platform) && platform->valuestring) {
            strncpy(entries[n].platform, platform->valuestring, 5);
            entries[n].platform[5] = 0;
        } else {
            entries[n].platform[0] = 0;
        }

        n++;
    }

    cJSON_Delete(root);
    if (filter_count > 0) ESP_LOGI(TAG, "Filter: %d/%d Züge passen", n, total_trains);
    if (n == 0) return false;

    // Wrap-fähiger Vergleich: ein Zug gilt als zukünftig, wenn er innerhalb
    // der nächsten 12 h liegt (modulo Tag). Sonst würde um 23:50 ein
    // 00:05-Zug als "vergangen" verworfen.
    int target_idx = -1;
    for (int i = 0; i < n; i++) {
        if (entries[i].minutes < 0) continue;
        int fwd = (entries[i].minutes - target_min + 24 * 60) % (24 * 60);
        if (fwd <= 12 * 60) { target_idx = i; break; }
    }
    if (target_idx < 0) {
        ESP_LOGW(TAG, "Alle %d Züge in der Vergangenheit (cur=%02d:%02d)",
                 n, target_min/60, target_min%60);
        return false;
    }

    int start = target_idx;
    if (start + DEP_COUNT > n) start = n - DEP_COUNT;
    if (start < 0) start = 0;

    for (int i = 0; i < DEP_COUNT; i++) {
        int idx = start + i;
        if (idx < 0 || idx >= n) { results[i].valid = false; continue; }
        results[i].valid = true;
        strncpy(results[i].time, entries[idx].time, 5);
        strncpy(results[i].destination, entries[idx].destination, 31);
        strncpy(results[i].platform, entries[idx].platform, 5);
        results[i].platform[5] = 0;
        results[i].delay = entries[idx].delay;
        results[i].cancelled = entries[idx].cancelled;
    }
    return true;
}