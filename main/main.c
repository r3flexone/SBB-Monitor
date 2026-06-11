#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "nvs_config.h"
#include "http_server.h"
#include "nvs_flash.h"
#include "mdns.h"

static const char *TAG = "main";

// Konfiguration — geladen aus NVS in app_main, überall verfügbar
static blink_config_t cfg;

// Gesetzt vom HTTP-Server nach erfolgreichem POST /api/config
volatile bool g_cfg_dirty = false;

#define OLED_WIDTH  128
#define OLED_HEIGHT  64

// ===== NEOPIXEL =====
static led_strip_handle_t led_strip;
static bool led_ok = false;
static void led_init(void) {
    led_strip_config_t s = { .strip_gpio_num = cfg.ledGpio, .max_leds = 1 };
    led_strip_rmt_config_t r = { .resolution_hz = 10*1000*1000, .flags.with_dma = false };
    // Kein ESP_ERROR_CHECK: ein ungültiger ledGpio aus der NVS-Config würde
    // sonst einen Panic-Boot-Loop erzeugen, der nur per Flash-Erase endet.
    esp_err_t err = led_strip_new_rmt_device(&s, &r, &led_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED init fehlgeschlagen (GPIO %d): %s",
                 cfg.ledGpio, esp_err_to_name(err));
        return;
    }
    led_ok = true;
    led_strip_clear(led_strip);
}
static void led_set(uint8_t r, uint8_t g, uint8_t b) {
    if (!led_ok) return;
    led_strip_set_pixel(led_strip, 0, r/16, g/16, b/16);
    led_strip_refresh(led_strip);
}
// Schlimmster Status aller 4 gültigen Züge: Ausfall > grosse > kleine Verspätung > OK
static void led_show_worst_status(const SbbDeparture deps[4]) {
    int worst = 0;
    for (int i = 0; i < 4; i++) {
        if (!deps[i].valid) continue;
        int s = 0;
        if (deps[i].cancelled)                       s = 3;
        else if (deps[i].delay >= cfg.delayBigMin)   s = 2;
        else if (deps[i].delay >= cfg.delaySmallMin) s = 1;
        if (s > worst) worst = s;
    }
    switch (worst) {
        case 3:  led_set(cfg.ledCancelledRgb[0],  cfg.ledCancelledRgb[1],  cfg.ledCancelledRgb[2]);  break;
        case 2:  led_set(cfg.ledDelayBigRgb[0],   cfg.ledDelayBigRgb[1],   cfg.ledDelayBigRgb[2]);   break;
        case 1:  led_set(cfg.ledDelaySmallRgb[0], cfg.ledDelaySmallRgb[1], cfg.ledDelaySmallRgb[2]); break;
        default: led_set(cfg.ledOkRgb[0],         cfg.ledOkRgb[1],         cfg.ledOkRgb[2]);         break;
    }
}

// ===== OLED =====
static i2c_master_dev_handle_t oled_dev;
static uint8_t framebuffer[OLED_WIDTH * OLED_HEIGHT / 8];
static bool oled_ok = false;

static void oled_cmd(uint8_t cmd) {
    if (!oled_dev) return;   // OLED-Init fehlgeschlagen oder noch nicht erfolgt
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
    int oled_addr = (int)strtol(cfg.oledAddr, NULL, 16);
    if (oled_addr == 0) oled_addr = 0x3C;
    i2c_master_bus_config_t bc = {
        .clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = I2C_NUM_0,
        .scl_io_num = cfg.sclGpio, .sda_io_num = cfg.sdaGpio,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (uint16_t)oled_addr, .scl_speed_hz = 100000,
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

// ===== FONT =====
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
        unsigned char b = s[1];
        switch (b) {
            case 0x84: draw_glyph(x,y,font_umlaut[0]); return x+6;
            case 0x96: draw_glyph(x,y,font_umlaut[1]); return x+6;
            case 0x9C: draw_glyph(x,y,font_umlaut[2]); return x+6;
            case 0xA4: draw_glyph(x,y,font_umlaut[3]); return x+6;
            case 0xB6: draw_glyph(x,y,font_umlaut[4]); return x+6;
            case 0xBC: draw_glyph(x,y,font_umlaut[5]); return x+6;
        }
        char base = 0;
        if      ((b >= 0x80 && b <= 0x85) || (b >= 0xA0 && b <= 0xA5)) base = 'A';
        else if (b == 0x87 || b == 0xA7)                               base = 'C';
        else if ((b >= 0x88 && b <= 0x8B) || (b >= 0xA8 && b <= 0xAB)) base = 'E';
        else if ((b >= 0x8C && b <= 0x8F) || (b >= 0xAC && b <= 0xAF)) base = 'I';
        else if (b == 0x91 || b == 0xB1)                               base = 'N';
        else if ((b >= 0x92 && b <= 0x98) || (b >= 0xB2 && b <= 0xB8)) base = 'O';
        else if ((b >= 0x99 && b <= 0x9B) || (b >= 0xB9 && b <= 0xBB)) base = 'U';
        else if (b == 0x9D || b == 0xBD)                               base = 'Y';
        if (base) {
            draw_glyph(x, y, font5x7[base - 0x20]);
            return x + 6;
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
static void draw_header(const char *title, bool stale) {
    memset(framebuffer, 0, sizeof(framebuffer));
    char hdr[20];
    if (stale) snprintf(hdr, sizeof(hdr), "!%.14s", title);
    else       snprintf(hdr, sizeof(hdr), "%.15s", title);
    draw_text(0, 0, hdr);
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    if (ti.tm_year >= 100) {
        char clk[6];
        snprintf(clk, sizeof(clk), "%02d:%02d", ti.tm_hour, ti.tm_min);
        draw_text(128 - 5*6, 0, clk);
    }
    for (int x = 0; x < 128; x++) draw_pixel(x, 9, true);
}

static void display_departures(SbbDeparture deps[4], bool stale) {
    draw_header(cfg.station, stale);
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

// ===== COUNTDOWN BAR =====
static void draw_countdown_bar(TickType_t active_start, TickType_t active_end) {
    TickType_t now = xTaskGetTickCount();
    int total = (int)(active_end - active_start);
    int remaining = (int)(active_end - now);
    if (remaining < 0) remaining = 0;
    if (total <= 0) return;
    int bar_w = (remaining * OLED_WIDTH) / total;
    for (int x = 0; x < OLED_WIDTH; x++) {
        if (x < bar_w)
            framebuffer[x + 7 * OLED_WIDTH] |= 0xC0;
        else
            framebuffer[x + 7 * OLED_WIDTH] &= ~0xC0;
    }
}

static void flush_page7(void) {
    if (!oled_ok) return;
    oled_cmd(0x21); oled_cmd(0); oled_cmd(127);
    oled_cmd(0x22); oled_cmd(7); oled_cmd(7);
    uint8_t buf[OLED_WIDTH + 1];
    buf[0] = 0x40;
    memcpy(&buf[1], &framebuffer[7 * OLED_WIDTH], OLED_WIDTH);
    i2c_master_transmit(oled_dev, buf, sizeof(buf), 100);
}

// Countdown-Balken aktualisieren und nur Page 7 flushen.
// Bei run_forever bleibt der Balken voll (total=1, remaining=1).
static void redraw_bar(bool run_forever, TickType_t active_start, TickType_t active_end) {
    TickType_t t = xTaskGetTickCount();
    draw_countdown_bar(run_forever ? t : active_start, run_forever ? t + 1 : active_end);
    flush_page7();
}

// ===== SLEEP =====
static void go_to_sleep(uint64_t us) {
    memset(framebuffer, 0, sizeof(framebuffer));
    oled_flush();
    if (oled_ok) oled_cmd(0xAE);
    if (led_ok) { led_strip_clear(led_strip); led_strip_refresh(led_strip); }
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_sleep_enable_timer_wakeup(us);
    // Interner Pullup gilt im Deep Sleep nur über die RTC-Domain — ohne
    // externen Pullup würde der Button-Pin sonst floaten (Geister-Wakeups).
    if (rtc_gpio_is_valid_gpio(cfg.buttonGpio)) {
        rtc_gpio_pullup_en(cfg.buttonGpio);
        rtc_gpio_pulldown_dis(cfg.buttonGpio);
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
        esp_sleep_enable_ext1_wakeup(1ULL << cfg.buttonGpio, ESP_EXT1_WAKEUP_ANY_LOW);
    } else {
        ESP_LOGW(TAG, "GPIO %d ist kein RTC-GPIO — kein Button-Wakeup", cfg.buttonGpio);
    }
    esp_deep_sleep_start();
}

// ===== WIFI =====
// NVS-Credentials, mit secrets.h als Compile-Time-Fallback
static void wifi_connect_from_cfg(void) {
    const char *ssid = cfg.ssid[0]     ? cfg.ssid     : WIFI_SSID;
    const char *pass = cfg.password[0] ? cfg.password : WIFI_PASS;
    sbb_wifi_init(ssid, pass);
}

// ===== NTP =====
static bool ntp_sync(void) {
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    if (!sntp_enabled()) sntp_init();
    else { sntp_stop(); sntp_init(); }

    time_t now; struct tm ti;
    int steps = (cfg.ntpTimeoutS * 1000) / 250;
    for (int i = 0; i < steps; i++) {
        vTaskDelay(pdMS_TO_TICKS(250));
        time(&now); localtime_r(&now, &ti);
        if (ti.tm_year >= 100) {
            ESP_LOGI(TAG, "NTP OK (%02d:%02d:%02d)", ti.tm_hour, ti.tm_min, ti.tm_sec);
            return true;
        }
    }
    ESP_LOGE(TAG, "NTP FAIL nach %d s", cfg.ntpTimeoutS);
    return false;
}

// ===== BOARD INFO =====
static void log_board_info(void) {
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    ESP_LOGI(TAG, "Chip: ESP32-S3 rev v%d.%d, %d Core(s)",
             ci.revision / 100, ci.revision % 100, ci.cores);
    ESP_LOGI(TAG, "Heap frei: %lu KB",
             (unsigned long)(esp_get_free_heap_size() / 1024));
}

// ===== SCHLAF-INFO =====
static void show_sleep_info(int sleep_min) {
    if (!oled_ok) return;
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    int wake_min = (ti.tm_hour * 60 + ti.tm_min + sleep_min) % (24 * 60);
    draw_header("SCHLAFE", false);
    char info[24];
    snprintf(info, sizeof(info), "BIS %02d:%02d", wake_min / 60, wake_min % 60);
    draw_text(0, 20, info);
    if (sleep_min >= 60) {
        snprintf(info, sizeof(info), "(%dH %dMIN)", sleep_min / 60, sleep_min % 60);
    } else {
        snprintf(info, sizeof(info), "(%d MIN)", sleep_min);
    }
    draw_text(0, 32, info);
    oled_flush();
    vTaskDelay(pdMS_TO_TICKS(2000));
}

// ===== ZEITFENSTER-VALIDIERUNG =====
static void check_window_overlaps(void) {
    for (int i = 0; i < cfg.timeWindowCount; i++) {
        int s1 = cfg.timeWindows[i].startH * 60 + cfg.timeWindows[i].startM;
        int e1 = cfg.timeWindows[i].endH * 60 + cfg.timeWindows[i].endM;
        // e1 < s1 ist ein gewolltes Fenster über Mitternacht; nur e1 == s1 ist leer.
        if (e1 == s1) {
            ESP_LOGW(TAG, "Zeitfenster %d: Laenge 0!", i + 1);
        }
        bool wrap1 = (e1 <= s1);
        for (int j = i + 1; j < cfg.timeWindowCount; j++) {
            int s2 = cfg.timeWindows[j].startH * 60 + cfg.timeWindows[j].startM;
            int e2 = cfg.timeWindows[j].endH * 60 + cfg.timeWindows[j].endM;
            bool wrap2 = (e2 <= s2);
            // Overlap-Pruefung gilt nur fuer zwei nicht-umschlagende Fenster.
            if (!wrap1 && !wrap2 && s1 < e2 && s2 < e1) {
                ESP_LOGW(TAG, "Zeitfenster %d und %d ueberlappen!", i + 1, j + 1);
            }
        }
    }
}

// ===== MAIN =====
static bool in_weekend_window(const struct tm *ti, const blink_config_t *c) {
    int cur   = ti->tm_wday * 24 * 60 + ti->tm_hour * 60 + ti->tm_min;
    int start = c->weekendStartDay * 24 * 60 + c->weekendStartH * 60 + c->weekendStartM;
    int end   = c->weekendEndDay   * 24 * 60 + c->weekendEndH   * 60 + c->weekendEndM;
    if (start <= end) return (cur >= start && cur < end);
    return (cur >= start || cur < end);
}

void app_main(void) {
    // NVS + Config ganz oben (vor Hardware-Init, da GPIO-Pins aus Config kommen)
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_config_load(&cfg);

    // Singular-API: auf allen ESP-IDF v5.x verfügbar. ESP_SLEEP_WAKEUP_UNDEFINED == 0
    // (Kaltstart), daher bleiben die wakeup==0 / !=0 Prüfungen weiter unten gültig.
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    bool woken_by_button = (wakeup == ESP_SLEEP_WAKEUP_EXT1);

    led_init();
    oled_init_display();

    if (wakeup == 0) {
        log_board_info();
        check_window_overlaps();
    }

    // Button-GPIO immer konfigurieren (für Halt-Erkennung und Sleep-Taste)
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << cfg.buttonGpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    int button_active_min = cfg.buttonActiveMin;
    if (woken_by_button) {
        int hold_ms = 0;
        while (gpio_get_level(cfg.buttonGpio) == 0 &&
               hold_ms < cfg.buttonLongPressMs + 1000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            hold_ms += 50;
        }
        if (hold_ms >= cfg.buttonLongPressMs) {
            button_active_min = cfg.buttonLongActiveMin;
        }
        ESP_LOGI(TAG, "Button %d ms -> %d Min aktiv", hold_ms, button_active_min);
    }

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    bool time_valid = (ti.tm_year >= 100);
    bool ap_mode = false;

    if (!time_valid) {
        if (oled_ok) {
            oled_cmd(0xAF);
            draw_header("KALTSTART", false);
            draw_text(0, 20, "WIFI+NTP...");
            oled_flush();
        }
        led_set(cfg.ledLoadingRgb[0], cfg.ledLoadingRgb[1], cfg.ledLoadingRgb[2]);
        wifi_connect_from_cfg();
        ap_mode = sbb_wifi_is_ap_mode();
        if (!ap_mode) {
            time_valid = ntp_sync();
            time(&now); localtime_r(&now, &ti);
        }
    }

    int cur_min = ti.tm_hour * 60 + ti.tm_min;
    bool is_weekend = (ti.tm_wday == 0 || ti.tm_wday == 6);
    bool weekend_skip = cfg.weekdaysOnly && is_weekend;

    int active_rem_min = 0;   // Minuten bis zum Fenster-Ende (wrap-fähig)
    bool in_window = false;
    if (time_valid && !weekend_skip) {
        for (int i = 0; i < cfg.timeWindowCount; i++) {
            int ws = cfg.timeWindows[i].startH * 60 + cfg.timeWindows[i].startM;
            int we = cfg.timeWindows[i].endH * 60 + cfg.timeWindows[i].endM;
            bool inside;
            if (we > ws) {                 // normales Fenster innerhalb eines Tages
                inside = (cur_min >= ws && cur_min < we);
            } else {                       // Fenster über Mitternacht (we <= ws)
                inside = (cur_min >= ws || cur_min < we);
            }
            if (inside) {
                in_window = true;
                int rem = we - cur_min;
                if (rem <= 0) rem += 24 * 60;   // Ende liegt am Folgetag
                active_rem_min = rem;
                break;
            }
        }
    }

    if (!in_window && !woken_by_button && cfg.sleepEnabled && !ap_mode) {
        int d;
        if (time_valid) {
            d = 24 * 60;
            for (int i = 0; i < cfg.timeWindowCount; i++) {
                int ws = cfg.timeWindows[i].startH * 60 + cfg.timeWindows[i].startM;
                int diff = ws - cur_min;
                if (diff <= 0) diff += 24 * 60;
                if (diff < d) d = diff;
            }
            if (cfg.weekendSleepEnabled && in_weekend_window(&ti, &cfg)) {
                int end_abs = cfg.weekendEndDay * 24 * 60 + cfg.weekendEndH * 60 + cfg.weekendEndM;
                int cur_abs = ti.tm_wday * 24 * 60 + cur_min;
                int d_weekend = end_abs - cur_abs;
                if (d_weekend < 0) d_weekend += 7 * 24 * 60;
                d = d_weekend;
                ESP_LOGI(TAG, "Wochenend-Fenster: schlafe %d Min", d);
            } else {
                if (d > cfg.sleepMaxMin) d = cfg.sleepMaxMin;
                if (weekend_skip) ESP_LOGI(TAG, "Wochenende, schlafe %d Min", d);
                else              ESP_LOGI(TAG, "Schlafe %d Min", d);
            }
        } else {
            d = cfg.sleepFallbackS / 60;
            if (d < 1) d = 1;
        }
        show_sleep_info(d);
        go_to_sleep((uint64_t)d * 60ULL * 1000000ULL);
        return;
    }

    if (woken_by_button) ESP_LOGI(TAG, "Button aktiv (%d Min)", button_active_min);
    else                 ESP_LOGI(TAG, "Zeitfenster aktiv");

    if (oled_ok) {
        oled_cmd(0xAF);
        draw_header(cfg.station, false);
        draw_text(0, 20, "LADE ZUEGE...");
        oled_flush();
    }
    led_set(cfg.ledLoadingRgb[0], cfg.ledLoadingRgb[1], cfg.ledLoadingRgb[2]);

    if (wakeup != 0) {
        wifi_connect_from_cfg();
        ap_mode = sbb_wifi_is_ap_mode();
        if (!ap_mode) {
            ntp_sync();
            time(&now); localtime_r(&now, &ti);
            cur_min = ti.tm_hour*60 + ti.tm_min;
        }
    }

    // mDNS + HTTP-Server (WiFi/netif bereits durch sbb_wifi_init aktiv)
    if (!ap_mode) {
        mdns_init();
        mdns_hostname_set("sbb-monitor");
        mdns_instance_name_set("SBB Monitor");
    }
    http_server_start();

    if (ap_mode) {
        if (oled_ok) {
            draw_header("KEIN WLAN", false);
            draw_text(0, 20, "SSID: SBB-MONITOR");
            draw_text(0, 32, "192.168.4.1");
            draw_text(0, 44, "WLAN EINRICHTEN");
            oled_flush();
        }
        led_set(cfg.ledLoadingRgb[0], cfg.ledLoadingRgb[1], cfg.ledLoadingRgb[2]);
        while (!g_cfg_dirty) {
            // Button im AP-Modus: sleepFallbackS schlafen statt fixer 30 s —
            // ein kurzer Zyklus würde sonst nur AP→Sleep→AP im Minutentakt kosten.
            if (gpio_get_level(cfg.buttonGpio) == 0)
                go_to_sleep((uint64_t)cfg.sleepFallbackS * 1000000ULL);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        // Kurze Pause damit die HTTP-Response noch ausgeliefert wird
        vTaskDelay(pdMS_TO_TICKS(500));
        // Neue WLAN-Credentials gespeichert → neu starten
        esp_restart();
    }

    TickType_t active_start = xTaskGetTickCount();
    TickType_t active_end;
    if (woken_by_button) {
        active_end = active_start + pdMS_TO_TICKS((uint32_t)button_active_min * 60 * 1000);
    } else {
        int rem = active_rem_min;
        if (rem < 1) rem = 1;
        active_end = active_start + pdMS_TO_TICKS((uint32_t)rem * 60 * 1000);
    }

    // Dest-Filter: Pointer-Array aus cfg.destFilters[][] bauen
    const char *filter_ptrs[4] = {0};
    for (int i = 0; i < cfg.destFilterCount && i < 4; i++)
        filter_ptrs[i] = cfg.destFilters[i];

    // static: aus dem Stack raus (verhindert Stack-Overflow im Main-Task)
    static SbbDeparture deps[4];
    static SbbDeparture last_deps[4];
    memset(deps, 0, sizeof(deps));
    memset(last_deps, 0, sizeof(last_deps));
    bool has_cached = false;
    time_t cached_time = 0;

    bool inverted = false;
    bool force_sleep = false;
    // sleepEnabled=false: Schleife läuft unbegrenzt (bis Button-Sleep oder Sleep wird aktiviert)
    bool run_forever = !cfg.sleepEnabled && !in_window && !woken_by_button;
    TickType_t next_invert = xTaskGetTickCount() +
        pdMS_TO_TICKS((uint32_t)(cfg.oledInvertMin > 0 ? cfg.oledInvertMin : 1440) * 60 * 1000);

    while (!force_sleep && (run_forever || xTaskGetTickCount() < active_end)) {
        if (g_cfg_dirty) {
            bool was_forever = run_forever;
            esp_err_t load_err = nvs_config_load(&cfg);
            g_cfg_dirty = false;
            if (load_err != ESP_OK) {
                ESP_LOGE(TAG, "Config Reload fehlgeschlagen: %s", esp_err_to_name(load_err));
            } else {
                ESP_LOGI(TAG, "Config neu geladen (Web-Panel)");
            }
            run_forever = !cfg.sleepEnabled && !in_window && !woken_by_button;
            if (was_forever && !run_forever) {
                // Sleep wurde aktiviert → frischen buttonActiveMin-Timer starten
                active_start = xTaskGetTickCount();
                active_end   = active_start + pdMS_TO_TICKS((uint32_t)cfg.buttonActiveMin * 60 * 1000);
                ESP_LOGI(TAG, "Sleep aktiviert → Timer %d Min", cfg.buttonActiveMin);
            }
            for (int i = 0; i < 4; i++)
                filter_ptrs[i] = (i < cfg.destFilterCount) ? cfg.destFilters[i] : NULL;
        }
        sbb_wifi_reconnect();

        bool success = false;
        for (int attempt = 0; attempt < cfg.apiRetryCount && !success; attempt++) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "API Retry %d/%d", attempt + 1, cfg.apiRetryCount);
                vTaskDelay(pdMS_TO_TICKS((uint32_t)cfg.apiRetryDelayS * 1000));
            }
            success = sbb_get_departures(cfg.station, deps, filter_ptrs, cfg.destFilterCount);
        }

        bool show_stale = false;
        if (success) {
            memcpy(last_deps, deps, sizeof(deps));
            has_cached = true;
            time(&cached_time);
        } else {
            time_t n; time(&n);
            if (has_cached && (n - cached_time) < cfg.staleMaxMin * 60) {
                show_stale = true;
            }
        }

        if (success) {
            led_show_worst_status(deps);
        } else {
            led_set(cfg.ledCancelledRgb[0], cfg.ledCancelledRgb[1], cfg.ledCancelledRgb[2]);
        }

        // Display
        if (success || show_stale) {
            display_departures(success ? deps : last_deps, show_stale);
        } else {
            draw_header(cfg.station, false);
            draw_text(0, 20, "API FEHLER");
            draw_text(0, 32, "PRUEFE NETZ...");
            oled_flush();
        }
        redraw_bar(run_forever, active_start, active_end);

        // Adaptiver Refresh
        int refresh_sec;
        if (success) {
            int min_to_next = -1;
            time_t n; struct tm nt; time(&n); localtime_r(&n, &nt);
            int cur_m = nt.tm_hour * 60 + nt.tm_min;
            for (int i = 0; i < 4; i++) {
                if (!deps[i].valid || deps[i].cancelled) continue;
                int h, m;
                if (sscanf(deps[i].time, "%d:%d", &h, &m) == 2) {
                    // Wrap-fähig: Zug nach Mitternacht zählt als zukünftig (≤ 12 h)
                    int diff = ((h * 60 + m + deps[i].delay) - cur_m + 24 * 60) % (24 * 60);
                    if (diff <= 12 * 60 && (min_to_next < 0 || diff < min_to_next))
                        min_to_next = diff;
                }
            }
            if (min_to_next < 0) {
                ESP_LOGW(TAG, "Kein Zug in Zukunft! cur_m=%d", cur_m);
                refresh_sec = cfg.refreshFarSec;
            } else if (min_to_next <= cfg.refreshNearMin) refresh_sec = cfg.refreshNearSec;
            else if (min_to_next <= cfg.refreshMidMin)    refresh_sec = cfg.refreshMidSec;
            else if (min_to_next <= cfg.refreshFarMin)    refresh_sec = cfg.refreshFarSec;
            else                                          refresh_sec = cfg.refreshVeryfarSec;
            if (min_to_next >= 0)
                ESP_LOGI(TAG, "Nächster Zug in %d Min -> Refresh %d s", min_to_next, refresh_sec);
        } else {
            refresh_sec = cfg.refreshMidSec;
        }

        // Wartephase mit LED-Blink, OLED-Invert und Uhr-Update
        TickType_t wait_end = xTaskGetTickCount() +
            pdMS_TO_TICKS((uint32_t)refresh_sec * 1000);
        bool blink_on = true;
        TickType_t next_toggle = xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)cfg.ledErrorBlinkMs);
        TickType_t next_clock = xTaskGetTickCount() + pdMS_TO_TICKS(30 * 1000);
        TickType_t next_bar = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
        while (xTaskGetTickCount() < wait_end && (run_forever || xTaskGetTickCount() < active_end)) {
            TickType_t t = xTaskGetTickCount();
            if (!success && cfg.ledErrorBlinkMs > 0 && t >= next_toggle) {
                blink_on = !blink_on;
                if (blink_on) led_set(cfg.ledCancelledRgb[0], cfg.ledCancelledRgb[1], cfg.ledCancelledRgb[2]);
                else          led_set(0, 0, 0);
                next_toggle = t + pdMS_TO_TICKS((uint32_t)cfg.ledErrorBlinkMs);
            }
            if (cfg.oledInvertMin > 0 && t >= next_invert) {
                inverted = !inverted;
                oled_cmd(inverted ? 0xA7 : 0xA6);
                next_invert = t + pdMS_TO_TICKS((uint32_t)cfg.oledInvertMin * 60 * 1000);
            }
            if (has_cached && t >= next_clock) {
                display_departures(last_deps, show_stale);
                redraw_bar(run_forever, active_start, active_end);
                next_clock = t + pdMS_TO_TICKS(30 * 1000);
            }
            if (t >= next_bar) {
                redraw_bar(run_forever, active_start, active_end);
                next_bar = t + pdMS_TO_TICKS(1000);
            }
            // Button während aktivem Betrieb → sofort schlafen
            if (gpio_get_level(cfg.buttonGpio) == 0) {
                ESP_LOGI(TAG, "Button gedrückt → Schlaf");
                force_sleep = true;
                break;
            }
            // Config-Änderung → äußere Schleife sofort reagieren lassen
            if (g_cfg_dirty) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (force_sleep) break;
    }

    if (inverted) oled_cmd(0xA6);
    http_server_stop();
    show_sleep_info(cfg.sleepAfterS / 60);
    go_to_sleep((uint64_t)cfg.sleepAfterS * 1000000ULL);
}
