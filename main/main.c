#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "lwip/apps/sntp.h"
#include "sbb.h"
#include "secrets.h"

static const char *TAG = "main";

// ==========================================================
//  EINSTELLUNGEN – Hier alles anpassen
// ==========================================================

// Bahnhof (Name wie auf sbb.ch)
#define STATION                  "Basel SBB"

// Ziel-Filter: nur Züge zu diesen Zielen anzeigen
//   Einfach auflisten, kein NULL nötig!
//   Kein Filter (alle Züge): DEST_FILTER_COUNT auf 0 setzen
static const char *DEST_FILTERS[] = { "Olten" };
#define DEST_FILTER_COUNT  1

// Aktives Zeitfenster (wann das Display automatisch angeht)
#define ACTIVE_START_H           6       // Startzeit Stunde
#define ACTIVE_START_M           45      // Startzeit Minute
#define ACTIVE_END_H             7       // Endzeit Stunde
#define ACTIVE_END_M             0       // Endzeit Minute

// Nach Knopfdruck: wie viele Minuten bleibt Display an
#define BUTTON_ACTIVE_MIN        10

// Wie oft Abfahrten neu laden (Sekunden)
#define REFRESH_SEC              60

// ==========================================================
//  HARDWARE – Nur ändern bei anderer Verkabelung
// ==========================================================

#define BUTTON_GPIO              0
#define LED_GPIO                 48
#define I2C_SCL_GPIO             5
#define I2C_SDA_GPIO             4
#define OLED_ADDR                0x3C
#define OLED_WIDTH               128
#define OLED_HEIGHT              64
#define NTP_TIMEOUT_MS           5000

// ===== NEOPIXEL =====
static led_strip_handle_t led_strip;
static void led_init(void) {
    led_strip_config_t s = { .strip_gpio_num = LED_GPIO, .max_leds = 1 };
    led_strip_rmt_config_t r = { .resolution_hz = 10*1000*1000, .flags.with_dma = false };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&s, &r, &led_strip));
    led_strip_clear(led_strip);
}
static void led_set(uint8_t r, uint8_t g, uint8_t b) {
    led_strip_set_pixel(led_strip, 0, r/16, g/16, b/16);
    led_strip_refresh(led_strip);
}

// ===== OLED =====
static i2c_master_dev_handle_t oled_dev;
static uint8_t framebuffer[OLED_WIDTH * OLED_HEIGHT / 8];
static bool oled_ok = false;

static void oled_cmd(uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd};
    i2c_master_transmit(oled_dev, buf, 2, 100);
}
static void oled_flush(void) {
    if (!oled_ok) return;
    oled_cmd(0x21); oled_cmd(0); oled_cmd(127);
    oled_cmd(0x22); oled_cmd(0); oled_cmd(7);
    uint8_t buf[OLED_WIDTH + 1];
    for (int p = 0; p < 8; p++) {
        buf[0] = 0x40;
        memcpy(&buf[1], &framebuffer[p * OLED_WIDTH], OLED_WIDTH);
        i2c_master_transmit(oled_dev, buf, sizeof(buf), 100);
    }
}
static void oled_init_display(void) {
    i2c_master_bus_config_t bc = {
        .clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = I2C_NUM_0,
        .scl_io_num = I2C_SCL_GPIO, .sda_io_num = I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDR, .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dc, &oled_dev) != ESP_OK) return;
    oled_cmd(0xAE); oled_cmd(0xD5); oled_cmd(0x80);
    oled_cmd(0xA8); oled_cmd(0x3F); oled_cmd(0xD3); oled_cmd(0x00);
    oled_cmd(0x40); oled_cmd(0x8D); oled_cmd(0x14);
    oled_cmd(0x20); oled_cmd(0x00); oled_cmd(0xA1); oled_cmd(0xC8);
    oled_cmd(0xDA); oled_cmd(0x12); oled_cmd(0x81); oled_cmd(0xCF);
    oled_cmd(0xD9); oled_cmd(0xF1); oled_cmd(0xDB); oled_cmd(0x40);
    oled_cmd(0xA4); oled_cmd(0xA6); oled_cmd(0xAF);
    oled_ok = true;
    memset(framebuffer, 0, sizeof(framebuffer));
    oled_flush();
}
static void draw_pixel(int x, int y, bool on) {
    if (x<0||x>=OLED_WIDTH||y<0||y>=OLED_HEIGHT) return;
    if (on) framebuffer[x+(y/8)*OLED_WIDTH] |=  (1<<(y%8));
    else    framebuffer[x+(y/8)*OLED_WIDTH] &= ~(1<<(y%8));
}

static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};
static const uint8_t font_umlaut[][5] = {
    {0x7D,0x12,0x11,0x12,0x7D},{0x3D,0x42,0x41,0x42,0x3D},{0x3E,0x41,0x40,0x41,0x3E},
    {0x22,0x54,0x54,0x54,0x78},{0x38,0x45,0x44,0x45,0x38},{0x3C,0x41,0x40,0x41,0x7C},
};
static void draw_glyph(int x, int y, const uint8_t *g) {
    for (int c = 0; c < 5; c++)
        for (int r = 0; r < 7; r++)
            draw_pixel(x+c, y+r, (g[c]>>r) & 1);
}
static int draw_char_utf8(int x, int y, const unsigned char *s, int *consumed) {
    *consumed = 1;
    unsigned char c = s[0];
    if (c == 0xC3 && s[1] != 0) {
        *consumed = 2;
        switch (s[1]) {
            case 0x84: draw_glyph(x,y,font_umlaut[0]); return x+6;
            case 0x96: draw_glyph(x,y,font_umlaut[1]); return x+6;
            case 0x9C: draw_glyph(x,y,font_umlaut[2]); return x+6;
            case 0xA4: draw_glyph(x,y,font_umlaut[3]); return x+6;
            case 0xB6: draw_glyph(x,y,font_umlaut[4]); return x+6;
            case 0xBC: draw_glyph(x,y,font_umlaut[5]); return x+6;
        }
        *consumed = 1;
    }
    char ch = (char)c;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch >= 32 && ch <= 90) { draw_glyph(x,y,font5x7[ch-32]); return x+6; }
    return x + 6;
}
static void draw_text(int x, int y, const char *text) {
    const unsigned char *s = (const unsigned char *)text;
    while (*s) {
        int consumed = 1;
        x = draw_char_utf8(x, y, s, &consumed);
        s += consumed;
    }
}
static void draw_header(const char *title) {
    memset(framebuffer, 0, sizeof(framebuffer));
    draw_text(0, 0, title);
    for (int x = 0; x < 128; x++) draw_pixel(x, 9, true);
}

static void display_departures(SbbDeparture deps[4]) {
    draw_header(STATION);
    int yp[] = {14, 27, 40, 53};
    for (int i = 0; i < 4; i++) {
        if (!deps[i].valid) continue;
        int y = yp[i];
        char line[32];
        if (deps[i].cancelled) {
            snprintf(line, sizeof(line), "%s AUSFALL", deps[i].time);
            draw_text(0, y, line);
            for (int px = 0; px < 128; px++)
                for (int py = y; py < y+8; py++) {
                    bool cur = (framebuffer[px+(py/8)*OLED_WIDTH] >> (py%8)) & 1;
                    draw_pixel(px, py, !cur);
                }
        } else if (deps[i].delay > 0) {
            int dly = deps[i].delay;
            if (dly > 99) dly = 99;
            char dlystr[4];
            snprintf(dlystr, sizeof(dlystr), "%d", dly);
            if (deps[i].platform[0]) {
                snprintf(line, sizeof(line), "%s+%s %.10s G%.2s",
                         deps[i].time, dlystr, deps[i].destination, deps[i].platform);
            } else {
                snprintf(line, sizeof(line), "%s+%s %.14s",
                         deps[i].time, dlystr, deps[i].destination);
            }
            draw_text(0, y, line);
        } else {
            if (deps[i].platform[0]) {
                snprintf(line, sizeof(line), "%s %.12s G%.2s",
                         deps[i].time, deps[i].destination, deps[i].platform);
            } else {
                snprintf(line, sizeof(line), "%s %.15s",
                         deps[i].time, deps[i].destination);
            }
            draw_text(0, y, line);
        }
    }
    oled_flush();
}

// ===== SLEEP =====
static void go_to_sleep(uint64_t us) {
    memset(framebuffer, 0, sizeof(framebuffer));
    oled_flush(); oled_cmd(0xAE);
    led_strip_clear(led_strip); led_strip_refresh(led_strip);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_sleep_enable_timer_wakeup(us);
    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

// ===== NTP =====
static bool ntp_sync(void) {
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    if (!sntp_enabled()) sntp_init();
    else { sntp_stop(); sntp_init(); }

    time_t now; struct tm ti;
    int steps = NTP_TIMEOUT_MS / 250;
    for (int i = 0; i < steps; i++) {
        vTaskDelay(pdMS_TO_TICKS(250));
        time(&now); localtime_r(&now, &ti);
        if (ti.tm_year >= 100) {
            ESP_LOGI(TAG, "NTP OK (%02d:%02d:%02d)", ti.tm_hour, ti.tm_min, ti.tm_sec);
            return true;
        }
    }
    ESP_LOGE(TAG, "NTP FAIL nach %d ms", NTP_TIMEOUT_MS);
    return false;
}

// ===== MAIN =====
void app_main(void) {
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    bool woken_by_button = (wakeup == ESP_SLEEP_WAKEUP_EXT1);

    led_init();
    oled_init_display();

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    bool time_valid = (ti.tm_year >= 100);

    if (!time_valid) {
        if (oled_ok) {
            oled_cmd(0xAF);
            draw_header("KALTSTART");
            draw_text(0, 20, "WIFI+NTP...");
            oled_flush();
        }
        led_set(255, 128, 0);
        sbb_wifi_init(WIFI_SSID, WIFI_PASS);
        time_valid = ntp_sync();
        time(&now); localtime_r(&now, &ti);
    }

    int cur_min = ti.tm_hour*60 + ti.tm_min;
    int start_min = ACTIVE_START_H*60 + ACTIVE_START_M;
    int end_min = ACTIVE_END_H*60 + ACTIVE_END_M;
    bool in_window = time_valid && (cur_min >= start_min && cur_min < end_min);

    if (!in_window && !woken_by_button) {
        uint64_t us;
        if (time_valid) {
            int d = start_min - cur_min;
            if (d <= 0) d += 24*60;
            if (d > 120) d = 120;  // max 2h schlafen, dann neu prüfen
            us = (uint64_t)d * 60ULL * 1000000ULL;
            ESP_LOGI(TAG, "Schlafe %d Min", d);
        } else {
            us = 5ULL * 60 * 1000000ULL;  // 5 Min Fallback
        }
        go_to_sleep(us);
        return;
    }

    if (woken_by_button) ESP_LOGI(TAG, "Button aktiv");
    else                 ESP_LOGI(TAG, "Zeitfenster aktiv");

    if (oled_ok) {
        oled_cmd(0xAF);
        draw_header(STATION);
        draw_text(0, 20, "LADE ZUEGE...");
        oled_flush();
    }
    led_set(255, 128, 0);

    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        sbb_wifi_init(WIFI_SSID, WIFI_PASS);
        ntp_sync();
        time(&now); localtime_r(&now, &ti);
        cur_min = ti.tm_hour*60 + ti.tm_min;
    }

    TickType_t active_end;
    if (woken_by_button) {
        active_end = xTaskGetTickCount() + pdMS_TO_TICKS(((uint32_t)BUTTON_ACTIVE_MIN * 60 * 1000));
    } else {
        int rem = end_min - cur_min;
        if (rem < 1) rem = 1;
        active_end = xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)rem * 60 * 1000);
    }

    SbbDeparture deps[4];
    while (xTaskGetTickCount() < active_end) {
        if (sbb_get_departures(STATION, deps, DEST_FILTERS, DEST_FILTER_COUNT)) {
            if (deps[0].cancelled)      led_set(255, 0, 0);
            else if (deps[0].delay > 5) led_set(128, 0, 255);
            else if (deps[0].delay > 1) led_set(0, 255, 255);
            else                        led_set(0, 255, 0);
            display_departures(deps);
        } else {
            led_set(255, 0, 0);
        }
        TickType_t wait_end = xTaskGetTickCount() + pdMS_TO_TICKS((REFRESH_SEC * 1000));
        while (xTaskGetTickCount() < wait_end && xTaskGetTickCount() < active_end) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    go_to_sleep(5ULL * 60 * 1000000ULL);  // 5 Min nach Zeitfenster-Ende
}