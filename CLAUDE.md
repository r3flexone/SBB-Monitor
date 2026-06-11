# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32-S3 SBB (Swiss Federal Railways) Departure Monitor. The device:
- Wakes from deep sleep on a schedule (active time window, default 06:45–07:00) or via button press (GPIO 0).
- Falls back to AP mode (SSID `SBB-Monitor`, IP `192.168.4.1`) when no WiFi is reachable, for first-time setup or after a router change.
- Fetches the next departures for a configured station from `transport.opendata.ch`.
- Displays them on an SSD1306 128×64 I2C OLED and signals overall status via a WS2812 NeoPixel (worst-of-4 rule).
- Returns to deep sleep to minimise battery usage.
- Serves a web configuration panel at `http://sbb-monitor.local` while active.

## Build / flash / monitor

Standard ESP-IDF v5.x project. Target is `esp32s3`.

```
# one-time: create WiFi credentials (gitignored)
cp main/secrets.h.example main/secrets.h   # edit WIFI_SSID / WIFI_PASS

idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor               # Ctrl-] to exit monitor
```

On first flash or after partition table changes: `idf.py fullclean` before build.

There is no linter or test harness.

## Git workflow

Develop on the active feature branch, not `main`. User-facing configuration values (station, filters, time windows, etc.) are managed via NVS and the web panel — do not hardcode them in source. If editing `main.c` defaults in `nvs_config.c`, avoid overwriting values the user may have customised.

## Architecture

All application code lives in `main/`. Source files:

- **`main/main.c`** — hardware drivers and the active-window main loop.
- **`main/sbb.c` / `sbb.h`** — WiFi, HTTP, JSON parsing, filter logic. Public API: `sbb_wifi_init()` and `sbb_get_departures()`.
- **`main/nvs_config.c` / `nvs_config.h`** — all configuration in NVS. `blink_config_t` is the central struct. Defaults in `nvs_config_defaults()`.
- **`main/http_server.c` / `http_server.h`** — web panel (SPIFFS + `/api/config` GET/POST + `/api/status` GET + `/api/departures` GET + `/api/restart` POST). Sets `g_cfg_dirty = true` after successful save so the main loop reloads cfg. Optional HTTP Basic Auth via `panelPass` (empty = no auth, the default). `/api/departures` serves `g_last_deps[]`, written by the main loop.
- **`main/spiffs/index.html`** — web panel UI, flashed to SPIFFS.
- **`main/cJSON.c` / `cJSON.h`** — vendored JSON library, do not modify.

### Configuration

All tunables live in `blink_config_t` (`nvs_config.h`). They are:
- Loaded from NVS at startup via `nvs_config_load()`.
- Editable at runtime via `http://sbb-monitor.local` while the device is active.
- Persisted to NVS on save; survive deep sleep and reboots.
- `secrets.h` (`WIFI_SSID` / `WIFI_PASS`) is a compile-time fallback if NVS has no credentials.

When adding a new tunable: add the field to `blink_config_t`, set a default in `nvs_config_defaults()`, add NVS load/save with a key ≤ 15 chars, and add the field to `handler_config_get()` and `handler_config_post()` in `http_server.c`.

### Wake-up and sleep flow (`app_main` in `main.c`)

1. Check wake cause. `ESP_SLEEP_WAKEUP_EXT1` = button on GPIO 0.
2. Init LED and OLED, set TZ (`CET-1CEST,M3.5.0,M10.5.0/3`).
3. `time()` + `localtime_r()` — the ESP32 RTC keeps time across deep sleep, so NTP is only re-run if `tm_year < 100` (no valid time yet).
4. Compute `in_window = time_valid && !weekend_skip && inside_active_time`.
5. **If not in window and not woken by button and `sleepEnabled = true` → sleep.** If `weekendSleepEnabled` and inside the weekend window: sleep directly until `weekendEnd`. Otherwise: sleep until next window start, capped at `sleepMaxMin`. If no valid time: sleep `sleepFallbackS` seconds.
6. **If `sleepEnabled = false` and not in window and not woken by button:** set `run_forever = true` — the active loop runs indefinitely. The OLED countdown bar stays full. When sleep is re-enabled via the web panel, a fresh `buttonActiveMin`-timer starts from the save moment.
7. Otherwise run the active loop until `active_end` (end of time window, or button active duration).
8. Button pressed during active loop → `force_sleep = true`, exits immediately.
9. After the loop, `go_to_sleep(sleepAfterS)` (default 300 s = 5 min).

### Active-loop responsibilities

`while (!force_sleep && (run_forever || xTaskGetTickCount() < active_end))` — one iteration does:
1. Reload `cfg` from NVS if `g_cfg_dirty` is set (after web panel save). Re-evaluates `run_forever`. If sleep was just re-enabled (`was_forever && !run_forever`), resets `active_end` to now + `buttonActiveMin`.
2. Retry-fetch departures (`cfg.apiRetryCount` attempts, `cfg.apiRetryDelayS` apart).
3. If success → update in-RAM cache (`last_deps`, `cached_time`). If failure → show cached data with `!` prefix if `< cfg.staleMaxMin` minutes old.
4. LED: **worst status across all 4 valid, non-cancelled departures** (Ausfall > big delay > small delay > OK).
5. Render via `display_departures()` which in turn calls `draw_header()` (station name + clock).
6. Draw countdown bar — full (100 %) when `run_forever`, counting down otherwise.
7. Compute minutes to next non-cancelled future train → tiered `refresh_sec`.
8. Inner wait loop handles: LED blink in error state, OLED invert for burn-in protection, re-render every 30 s, button-press → `force_sleep`, `g_cfg_dirty` → break to outer loop immediately.

### Stack and memory gotchas

- The main task's default stack is tight. Both `SbbDeparture[4]` arrays (cache + current) are `static` inside `app_main` — keep them static, and be cautious when adding large locals.
- `http_buf` in `sbb.c` is a 32 KB heap buffer, allocated lazily on first call and reused.
- The URL buffer is `char url[512]` — large enough for the full URL with the optional `passList` field. Don't shrink it.
- NVS keys must be ≤ 15 characters (ESP-IDF limit).

### `sbb_get_departures()` contract

Signature: `bool sbb_get_departures(const char *station, SbbDeparture results[4], const char *dest_filters[], int filter_count)`.

- **Count-based, not NULL-terminated.** `filter_count = 0` disables filtering entirely.
- Station name is URL-encoded inside (space → `%20`), so callers pass the canonical name as it appears on sbb.ch (e.g. `"Basel SBB"`).
- Filter matches both the end destination (`to`) and intermediate stops (`passList/station/name`), case-insensitive substring. The `passList` field is requested from the API **only when `filter_count > 0`** to save bandwidth.
- Results are chosen to start at the first departure ≥ current HH:MM; if fewer than 4 future trains are available, the window backs up and some `results[i]` may have `valid = false`.

### Font and UTF-8

`main.c` ships a 5×7 uppercase-only font plus six custom umlaut glyphs (`ÄÖÜäöü`). Lowercase input is mapped to uppercase before lookup. Other Latin-1 accented chars (é, è, â, ô, ç, …) fall back to their base letter via range checks on the second UTF-8 byte (`0xC3 0xXX`), so `Delémont` renders as `DELEMONT`.
