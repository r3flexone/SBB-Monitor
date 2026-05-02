# main.c — Ausstehende Änderungen

Diese Datei beschreibt konkrete Code-Änderungen die in `main/main.c` gemacht werden müssen.
Alle anderen Dateien (`nvs_config.h/.c`, `http_server.c`, `spiffs/index.html`) sind bereits aktuell.

---

## 1. Wochenend-Schlaf-Fenster

### Kontext

`main.c` berechnet die Schlafdauer so (ca. Zeile 260 in `app_main`):

```c
if (d > cfg.sleepMaxMin) d = cfg.sleepMaxMin;
```

`sleepMaxMin` ist 120 min — deshalb wacht das Gerät Sa/So alle 2h auf obwohl `weekdaysOnly = true`.

### Neue Felder (bereits in nvs_config.h)

```c
int  weekendStartDay;  // Wochentag ab wann Wochenend-Modus (0=So..6=Sa), Default: 5 (Fr)
int  weekendStartH;    // Startzeit Stunde, Default: 18
int  weekendStartM;    // Startzeit Minute, Default: 0
int  weekendEndDay;    // Wochentag bis wann Wochenend-Modus (0=So..6=Sa), Default: 1 (Mo)
int  weekendEndH;      // Endzeit Stunde, Default: 5
int  weekendEndM;      // Endzeit Minute, Default: 0
```

### Hilfsfunktion (neu hinzufügen, vor app_main)

```c
// Gibt true zurück wenn der aktuelle Zeitpunkt im Wochenend-Fenster liegt.
// Unterstützt Fenster die über Mitternacht und über Wochengrenzen gehen.
static bool in_weekend_window(const struct tm *ti, const blink_config_t *cfg) {
    // Aktueller Zeitpunkt in "Wochentag-Minuten" (0 = So 00:00, max = Sa 23:59)
    int cur  = ti->tm_wday * 24 * 60 + ti->tm_hour * 60 + ti->tm_min;
    int start = cfg->weekendStartDay * 24 * 60 + cfg->weekendStartH * 60 + cfg->weekendStartM;
    int end   = cfg->weekendEndDay   * 24 * 60 + cfg->weekendEndH   * 60 + cfg->weekendEndM;

    if (start <= end) {
        // Normales Fenster (z.B. Fr 18:00 → Mo 05:00, start < end)
        return (cur >= start && cur < end);
    } else {
        // Fenster geht über Wochengrenze (So → Sa) — unwahrscheinlich aber sicher
        return (cur >= start || cur < end);
    }
}
```

### Änderung in app_main

Ersetze:

```c
if (d > cfg.sleepMaxMin) d = cfg.sleepMaxMin;
```

Mit:

```c
if (cfg.weekdaysOnly && in_weekend_window(&ti, &cfg)) {
    // Im Wochenend-Fenster: direkt bis End-Zeitpunkt schlafen
    int end_abs = cfg.weekendEndDay * 24 * 60 + cfg.weekendEndH * 60 + cfg.weekendEndM;
    int cur_abs = ti.tm_wday * 24 * 60 + cur_min;
    int d_weekend = end_abs - cur_abs;
    if (d_weekend <= 0) d_weekend += 7 * 24 * 60;
    d = d_weekend;
    ESP_LOGI(TAG, "Wochenend-Fenster: schlafe %d Min", d);
} else {
    if (d > cfg.sleepMaxMin) d = cfg.sleepMaxMin;
}
```

### Beispiele

| Zeitpunkt | weekendStart | weekendEnd | Schlafdauer |
|-----------|-------------|-----------|-------------|
| Fr 18:30  | Fr 18:00    | Mo 05:00  | ~58.5h      |
| Sa 08:00  | Fr 18:00    | Mo 05:00  | ~45h        |
| So 22:00  | Fr 18:00    | Mo 05:00  | ~7h         |
| Mo 04:00  | Fr 18:00    | Mo 05:00  | ~1h         |
| Mo 05:01  | Fr 18:00    | Mo 05:00  | normal (≤120 min) |
| weekdaysOnly = false | — | — | immer normal |

---

## 2. WiFi-Credentials Fallback (bereits funktional)

`main.c` macht bereits:

```c
const char *wifi_ssid = cfg.ssid[0] ? cfg.ssid : WIFI_SSID;
const char *wifi_pass = cfg.password[0] ? cfg.password : WIFI_PASS;
```

Keine Änderung nötig. Das Web-Panel zeigt "Leer = secrets.h" als Hint.

---

## 3. Nach den Änderungen

```bash
idf.py fullclean   # nötig wegen Partition-Table-Änderung (einmalig)
idf.py build flash monitor
```
