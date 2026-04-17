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
#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_system.h"

static const char *TAG = "main";

// ==========================================================
//  EINSTELLUNGEN – Hier alles anpassen
// ==========================================================

// Bahnhof (Name wie auf sbb.ch)
#define STATION                  "Gelterkinden"

// Ziel-Filter: nur Züge zu diesen Zielen (Endziel ODER Zwischenhalt) anzeigen.
//   Zeilen auskommentieren = weniger Filter. Alles leer = kein Filter.
//   Die Zählung passiert automatisch — nichts anderes anpassen.
static const char *DEST_FILTERS[] = {
    // "Basel SBB",
    // "Olten",
    NULL,  // Platzhalter, damit das Array auch "leer" gültig bleibt
};
#define DEST_FILTER_COUNT  ((int)(sizeof(DEST_FILTERS)/sizeof(DEST_FILTERS[0])) - 1)

// Aktive Zeitfenster (wann das Display automatisch angeht)
// Beliebig viele Fenster — einfach Zeilen hinzufügen/auskommentieren.
// Format: { Stunde_Start, Minute_Start, Stunde_Ende, Minute_Ende }
static const struct { uint8_t start_h, start_m, end_h, end_m; } TIME_WINDOWS[] = {
    {  6, 45,  6, 55 },   // Morgens
    // { 17, 30, 17, 45 },  // Abends (Beispiel)
};
#define TIME_WINDOW_COUNT  ((int)(sizeof(TIME_WINDOWS)/sizeof(TIME_WINDOWS[0])))

// Nach Knopfdruck: wie viele Minuten bleibt Display an
#define BUTTON_ACTIVE_MIN        2
#define BUTTON_LONG_PRESS_MS     3000    // Ab 3s = langer Druck
#define BUTTON_LONG_ACTIVE_MIN   10      // Bei langem Druck: 10 Min aktiv

// Nur Werktage (Mo–Fr) aktiv? 1 = ja, 0 = auch Sa/So
#define WEEKDAYS_ONLY            1

// OLED Burn-in-Schutz: alle X Min Display invertieren (0 = aus)
#define OLED_INVERT_MIN          5

// Wenn API fehlschlägt: mehrere Versuche bevor "Fehler" angezeigt wird
#define API_RETRY_COUNT          3       // Anzahl Versuche pro Refresh
#define API_RETRY_DELAY_MS       5000    // Wartezeit zwischen Versuchen

// Wie lange dürfen alte (gecachte) Daten nach API-Ausfall noch gezeigt werden
#define STALE_MAX_MIN            10

// Adaptive Refresh-Rate (abhängig von Minuten bis zum nächsten Zug)
#define REFRESH_NEAR_SEC         30      // bis NEAR_MIN Min:  alle 30 s
#define REFRESH_MID_SEC          120     // bis MID_MIN Min:   alle 2 Min
#define REFRESH_FAR_SEC          300     // bis FAR_MIN Min:   alle 5 Min
#define REFRESH_VERYFAR_SEC      600     // darüber:            alle 10 Min
#define REFRESH_NEAR_MIN         5
#define REFRESH_MID_MIN          10
#define REFRESH_FAR_MIN          30

// ==========================================================
//  LED-FARBEN (R, G, B) – 0..255, werden auf 1/16 gedimmt
// ==========================================================

// Status-LED (schlimmster Status aller 4 Züge)
#define LED_OK                   0,   255, 0      // pünktlich / <= 1 Min Verspätung (grün)
#define LED_DELAY_SMALL          0,   255, 255    // leichte Verspätung (cyan)
#define LED_DELAY_BIG            128, 0,   255    // grosse Verspätung (lila)
#define LED_CANCELLED            255, 0,   0      // Ausfall (rot)

// Andere Zustände
#define LED_LOADING              255, 128, 0      // Laden / Kaltstart (orange)
#define LED_ERROR                255, 0,   0      // Fehler beim Laden (rot)

// Ab wann gilt welche Verspätung (in Minuten, inklusive)
#define DELAY_SMALL_MIN          2       // ab 2 Min  -> cyan
#define DELAY_BIG_MIN            6       // ab 6 Min  -> lila

// Error-LED blinken (in ms pro Phase; 0 = dauerhaft an)
#define LED_ERROR_BLINK_MS       500

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
        unsigned char b = s[1];
        switch (b) {
            case 0x84: draw_glyph(x,y,font_umlaut[0]); return x+6;  // Ä
            case 0x96: draw_glyph(x,y,font_umlaut[1]); return x+6;  // Ö
            case 0x9C: draw_glyph(x,y,font_umlaut[2]); return x+6;  // Ü
            case 0xA4: draw_glyph(x,y,font_umlaut[3]); return x+6;  // ä
            case 0xB6: draw_glyph(x,y,font_umlaut[4]); return x+6;  // ö
            case 0xBC: draw_glyph(x,y,font_umlaut[5]); return x+6;  // ü
        }
        // Fallback: akzentuierte Zeichen ohne Akzent rendern (é→E, à→A, ô→O, …)
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
        *consumed = 1;  // unbekanntes UTF-8 — als Einzel-Byte weiterbehandeln
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
    // Titel links (auf max 15 Zeichen gekürzt damit die Uhr rechts Platz hat)
    char hdr[20];
    if (stale) snprintf(hdr, sizeof(hdr), "!%.14s", title);
    else       snprintf(hdr, sizeof(hdr), "%.15s", title);
    draw_text(0, 0, hdr);
    // Uhrzeit oben rechts (5 Zeichen x 6 Pixel = 30 Pixel breit)
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
    draw_header(STATION, stale);
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

// ===== SLEEP =====
static void go_to_sleep(uint64_t us) {
    memset(framebuffer, 0, sizeof(framebuffer));
    oled_flush();
    if (oled_ok) oled_cmd(0xAE);
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
    for (int i = 0; i < TIME_WINDOW_COUNT; i++) {
        int s1 = TIME_WINDOWS[i].start_h * 60 + TIME_WINDOWS[i].start_m;
        int e1 = TIME_WINDOWS[i].end_h * 60 + TIME_WINDOWS[i].end_m;
        if (e1 <= s1) {
            ESP_LOGW(TAG, "Zeitfenster %d: Ende vor Start!", i + 1);
        }
        for (int j = i + 1; j < TIME_WINDOW_COUNT; j++) {
            int s2 = TIME_WINDOWS[j].start_h * 60 + TIME_WINDOWS[j].start_m;
            int e2 = TIME_WINDOWS[j].end_h * 60 + TIME_WINDOWS[j].end_m;
            if (s1 < e2 && s2 < e1) {
                ESP_LOGW(TAG, "Zeitfenster %d und %d ueberlappen!", i + 1, j + 1);
            }
        }
    }
}

// ===== MAIN =====
void app_main(void) {
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    bool woken_by_button = (wakeup == ESP_SLEEP_WAKEUP_EXT1);

    led_init();
    oled_init_display();

    if (wakeup == ESP_SLEEP_WAKEUP_UNDEFINED) {
        log_board_info();
        check_window_overlaps();
    }

    int button_active_min = BUTTON_ACTIVE_MIN;
    if (woken_by_button) {
        gpio_config_t btn = {
            .pin_bit_mask = 1ULL << BUTTON_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&btn);
        int hold_ms = 0;
        while (gpio_get_level(BUTTON_GPIO) == 0 &&
               hold_ms < BUTTON_LONG_PRESS_MS + 1000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            hold_ms += 50;
        }
        if (hold_ms >= BUTTON_LONG_PRESS_MS) {
            button_active_min = BUTTON_LONG_ACTIVE_MIN;
        }
        ESP_LOGI(TAG, "Button %d ms -> %d Min aktiv", hold_ms, button_active_min);
    }

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    bool time_valid = (ti.tm_year >= 100);

    if (!time_valid) {
        if (oled_ok) {
            oled_cmd(0xAF);
            draw_header("KALTSTART", false);
            draw_text(0, 20, "WIFI+NTP...");
            oled_flush();
        }
        led_set(LED_LOADING);
        sbb_wifi_init(WIFI_SSID, WIFI_PASS);
        time_valid = ntp_sync();
        time(&now); localtime_r(&now, &ti);
    }

    int cur_min = ti.tm_hour * 60 + ti.tm_min;
    bool is_weekend = (ti.tm_wday == 0 || ti.tm_wday == 6);
    bool weekend_skip = WEEKDAYS_ONLY && is_weekend;

    int active_end_min = 0;
    bool in_window = false;
    if (time_valid && !weekend_skip) {
        for (int i = 0; i < TIME_WINDOW_COUNT; i++) {
            int ws = TIME_WINDOWS[i].start_h * 60 + TIME_WINDOWS[i].start_m;
            int we = TIME_WINDOWS[i].end_h * 60 + TIME_WINDOWS[i].end_m;
            if (cur_min >= ws && cur_min < we) {
                in_window = true;
                active_end_min = we;
                break;
            }
        }
    }

    if (!in_window && !woken_by_button) {
        int d;
        if (time_valid) {
            d = 24 * 60;
            for (int i = 0; i < TIME_WINDOW_COUNT; i++) {
                int ws = TIME_WINDOWS[i].start_h * 60 + TIME_WINDOWS[i].start_m;
                int diff = ws - cur_min;
                if (diff <= 0) diff += 24 * 60;
                if (diff < d) d = diff;
            }
            if (d > 120) d = 120;
            if (weekend_skip) ESP_LOGI(TAG, "Wochenende, schlafe %d Min", d);
            else              ESP_LOGI(TAG, "Schlafe %d Min", d);
        } else {
            d = 5;
        }
        show_sleep_info(d);
        go_to_sleep((uint64_t)d * 60ULL * 1000000ULL);
        return;
    }

    if (woken_by_button) ESP_LOGI(TAG, "Button aktiv (%d Min)", button_active_min);
    else                 ESP_LOGI(TAG, "Zeitfenster aktiv");

    if (oled_ok) {
        oled_cmd(0xAF);
        draw_header(STATION, false);
        draw_text(0, 20, "LADE ZUEGE...");
        oled_flush();
    }
    led_set(LED_LOADING);

    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        sbb_wifi_init(WIFI_SSID, WIFI_PASS);
        ntp_sync();
        time(&now); localtime_r(&now, &ti);
        cur_min = ti.tm_hour*60 + ti.tm_min;
    }

    TickType_t active_start = xTaskGetTickCount();
    TickType_t active_end;
    if (woken_by_button) {
        active_end = active_start + pdMS_TO_TICKS((uint32_t)button_active_min * 60 * 1000);
    } else {
        int rem = active_end_min - cur_min;
        if (rem < 1) rem = 1;
        active_end = active_start + pdMS_TO_TICKS((uint32_t)rem * 60 * 1000);
    }

    // static: aus dem Stack raus (verhindert Stack-Overflow im Main-Task)
    static SbbDeparture deps[4];
    static SbbDeparture last_deps[4];
    memset(deps, 0, sizeof(deps));
    memset(last_deps, 0, sizeof(last_deps));
    bool has_cached = false;
    time_t cached_time = 0;

    bool inverted = false;
    TickType_t next_invert = xTaskGetTickCount() +
        pdMS_TO_TICKS((uint32_t)OLED_INVERT_MIN * 60 * 1000);

    while (xTaskGetTickCount() < active_end) {
        // --- WiFi prüfen / reconnect ---
        sbb_wifi_reconnect();

        // --- API-Abfrage mit Retries ---
        bool success = false;
        for (int attempt = 0; attempt < API_RETRY_COUNT && !success; attempt++) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "API Retry %d/%d", attempt + 1, API_RETRY_COUNT);
                vTaskDelay(pdMS_TO_TICKS(API_RETRY_DELAY_MS));
            }
            success = sbb_get_departures(STATION, deps, DEST_FILTERS, DEST_FILTER_COUNT);
        }

        // --- Cache aktualisieren bzw. als Stale markieren ---
        bool show_stale = false;
        if (success) {
            memcpy(last_deps, deps, sizeof(deps));
            has_cached = true;
            time(&cached_time);
        } else {
            time_t n; time(&n);
            if (has_cached && (n - cached_time) < STALE_MAX_MIN * 60) {
                show_stale = true;
            }
        }

        // --- LED setzen (schlimmster Status aller 4 Züge) ---
        if (success) {
            int worst = 0;  // 0=OK, 1=small, 2=big, 3=cancelled
            for (int i = 0; i < 4; i++) {
                if (!deps[i].valid) continue;
                int s = 0;
                if (deps[i].cancelled)                     s = 3;
                else if (deps[i].delay >= DELAY_BIG_MIN)   s = 2;
                else if (deps[i].delay >= DELAY_SMALL_MIN) s = 1;
                if (s > worst) worst = s;
            }
            switch (worst) {
                case 3:  led_set(LED_CANCELLED);   break;
                case 2:  led_set(LED_DELAY_BIG);   break;
                case 1:  led_set(LED_DELAY_SMALL); break;
                default: led_set(LED_OK);          break;
            }
        } else {
            led_set(LED_ERROR);
        }

        // --- Display ---
        if (success || show_stale) {
            display_departures(success ? deps : last_deps, show_stale);
        } else {
            // Kein Erfolg UND kein gültiger Cache → Fehler explizit zeigen
            draw_header(STATION, false);
            draw_text(0, 20, "API FEHLER");
            draw_text(0, 32, "PRUEFE NETZ...");
            oled_flush();
        }
        draw_countdown_bar(active_start, active_end);
        flush_page7();

        // --- Adaptiver Refresh-Intervall ---
        int refresh_sec;
        if (success) {
            int min_to_next = -1;  // -1 = noch nichts gefunden
            time_t n; struct tm nt; time(&n); localtime_r(&n, &nt);
            int cur_m = nt.tm_hour * 60 + nt.tm_min;
            for (int i = 0; i < 4; i++) {
                if (!deps[i].valid || deps[i].cancelled) continue;
                int h, m;
                if (sscanf(deps[i].time, "%d:%d", &h, &m) == 2) {
                    int diff = (h * 60 + m + deps[i].delay) - cur_m;
                    if (diff >= 0 && (min_to_next < 0 || diff < min_to_next)) {
                        min_to_next = diff;
                    }
                }
            }
            if (min_to_next < 0) {
                // Keine zukünftigen Züge in der Liste – Debug-Log + langsamer Refresh
                ESP_LOGW(TAG, "Kein Zug in Zukunft! cur_m=%d", cur_m);
                for (int i = 0; i < 4; i++) {
                    ESP_LOGW(TAG, "  deps[%d] valid=%d canc=%d time='%s' delay=%d",
                             i, deps[i].valid, deps[i].cancelled,
                             deps[i].time, deps[i].delay);
                }
                refresh_sec = REFRESH_FAR_SEC;
            } else if (min_to_next <= REFRESH_NEAR_MIN) refresh_sec = REFRESH_NEAR_SEC;
            else if (min_to_next <= REFRESH_MID_MIN)    refresh_sec = REFRESH_MID_SEC;
            else if (min_to_next <= REFRESH_FAR_MIN)    refresh_sec = REFRESH_FAR_SEC;
            else                                        refresh_sec = REFRESH_VERYFAR_SEC;
            if (min_to_next >= 0) {
                ESP_LOGI(TAG, "Nächster Zug in %d Min -> Refresh %d s",
                         min_to_next, refresh_sec);
            }
        } else {
            refresh_sec = REFRESH_MID_SEC;  // bei Fehler nicht zu oft hämmern
        }

        // --- Wartephase mit LED-Blink, OLED-Invert und Uhr-Update ---
        TickType_t wait_end = xTaskGetTickCount() +
            pdMS_TO_TICKS((uint32_t)refresh_sec * 1000);
        bool blink_on = true;
        TickType_t next_toggle = xTaskGetTickCount() + pdMS_TO_TICKS(LED_ERROR_BLINK_MS);
        TickType_t next_clock = xTaskGetTickCount() + pdMS_TO_TICKS(30 * 1000);
        TickType_t next_bar = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
        while (xTaskGetTickCount() < wait_end && xTaskGetTickCount() < active_end) {
            TickType_t t = xTaskGetTickCount();
            // LED blinken bei Fehler
            if (!success && LED_ERROR_BLINK_MS > 0 && t >= next_toggle) {
                blink_on = !blink_on;
                if (blink_on) led_set(LED_ERROR);
                else          led_set(0, 0, 0);
                next_toggle = t + pdMS_TO_TICKS(LED_ERROR_BLINK_MS);
            }
            // OLED Burn-in-Schutz: periodisch invertieren
            if (OLED_INVERT_MIN > 0 && t >= next_invert) {
                inverted = !inverted;
                oled_cmd(inverted ? 0xA7 : 0xA6);
                next_invert = t + pdMS_TO_TICKS((uint32_t)OLED_INVERT_MIN * 60 * 1000);
            }
            // Uhr auf dem Display alle 30 s aktualisieren
            if (has_cached && t >= next_clock) {
                display_departures(last_deps, show_stale);
                draw_countdown_bar(active_start, active_end);
                flush_page7();
                next_clock = t + pdMS_TO_TICKS(30 * 1000);
            }
            // Countdown-Bar jede Sekunde aktualisieren
            if (t >= next_bar) {
                draw_countdown_bar(active_start, active_end);
                flush_page7();
                next_bar = t + pdMS_TO_TICKS(1000);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    if (inverted) oled_cmd(0xA6);
    show_sleep_info(5);
    go_to_sleep(5ULL * 60 * 1000000ULL);
}