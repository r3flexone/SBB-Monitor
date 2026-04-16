# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32-S3 SBB (Swiss Federal Railways) Departure Monitor. The device:
- Wakes from deep sleep on a schedule (active time window, e.g. 06:45–06:55) or via button press (GPIO 0).
- Fetches the next departures for a configured station from `transport.opendata.ch`.
- Displays them on an SSD1306 128×64 I2C OLED and signals overall status via a WS2812 NeoPixel (worst-of-4 rule).
- Returns to deep sleep to minimise battery usage.

The `README.md` in the repo root is stale — it is the unmodified ESP-IDF `blink` example README. Do not use it as a source of truth; the project has been fully repurposed.

## Build / flash / monitor

Standard ESP-IDF v6 project. Target is `esp32s3`.

```
# one-time: create WiFi credentials (gitignored)
cp main/secrets.h.example main/secrets.h   # edit WIFI_SSID / WIFI_PASS

idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor               # Ctrl-] to exit monitor
```

There is no linter or test harness. `pytest_blink.py` is leftover from the blink example and is not used.

## Git workflow

Active development branch is `claude/esp32-sbb-monitor-8qfn3` (push/pull here, not `main`). User-facing configuration values in `main/main.c` (STATION, DEST_FILTERS, time window, etc.) are *user-owned* — they may be changed on the remote between sessions, so rebase before committing and do not revert personal settings.

## Architecture

All application code lives in `main/`. There are only three source files you should touch:

- **`main/main.c`** — user config, hardware drivers, and the active-window main loop.
- **`main/sbb.c` / `sbb.h`** — WiFi, HTTP, JSON parsing, filter logic. The public API is just `sbb_wifi_init()` and `sbb_get_departures()`.
- **`main/cJSON.c` / `cJSON.h`** — vendored JSON library, do not modify.

`main/Kconfig.projbuild` and the `CONFIG_BLINK_*` defaults in `sdkconfig.defaults*` are leftovers from the blink example. They are unused by the current code; don't rely on them.

### Config-first design (`main/main.c`)

The top of `main.c` (between the two `====` banner comments) is the **single user-facing configuration block**. When adding a new tunable (colour, threshold, interval), put its `#define` there, not next to the logic that consumes it. Current blocks are:
- `EINSTELLUNGEN` — station, destination filters, active time window, button duration, weekday-only flag, burn-in, API retry, stale-data age, adaptive refresh thresholds.
- `LED-FARBEN` — all LED colours as `R, G, B` triples consumed by `led_set(...)` via macro expansion (not functions), plus the delay-tier minute thresholds.
- `HARDWARE` — pin assignments. Only change for different wiring.

### Wake-up and sleep flow (`app_main` in `main.c`)

1. Check wake cause. `ESP_SLEEP_WAKEUP_EXT1` = button on GPIO 0.
2. Init LED and OLED, set TZ (`CET-1CEST,M3.5.0,M10.5.0/3`).
3. `time()` + `localtime_r()` — the ESP32 RTC keeps time across deep sleep, so NTP is only re-run if `tm_year < 100` (no valid time yet).
4. Compute `in_window = time_valid && !weekend_skip && inside_active_time`.
5. **If not in window and not woken by button → sleep again** for `min(start_min - cur_min, 120)` minutes. The 2 h cap ensures we re-check periodically even on weekends.
6. Otherwise run the active loop until `active_end` (end of time window, or `BUTTON_ACTIVE_MIN` minutes if button-woken).
7. After the loop, `go_to_sleep(5 min)` as fallback.

### Active-loop responsibilities

One `while (xTaskGetTickCount() < active_end)` iteration does:
1. Retry-fetch departures (`API_RETRY_COUNT` attempts, `API_RETRY_DELAY_MS` apart).
2. If success → update in-RAM cache (`last_deps`, `cached_time`). If failure → show cached data with a `!` prefix if `< STALE_MAX_MIN` old.
3. LED: **worst status across all 4 valid, non-cancelled departures** (Ausfall > big delay > small delay > OK).
4. Render via `display_departures()` which in turn calls `draw_header()` (station name + clock).
5. Compute minutes to next non-cancelled future train → tiered `refresh_sec` (`REFRESH_NEAR/MID/FAR/VERYFAR`).
6. Inner wait loop (`vTaskDelay(100)`) handles: LED blink while in error state, OLED invert every `OLED_INVERT_MIN` minutes (burn-in protection), and re-rendering every 30 s so the header clock stays fresh.

### Stack and memory gotchas

- The main task's default stack (≈3584 bytes) is tight. Two `SbbDeparture[4]` arrays (cache + current) plus `sbb_get_departures()`'s ~1800-byte stack frame will overflow it. Both arrays are `static` inside `app_main` — keep them static, and be cautious when adding more local arrays.
- `http_buf` in `sbb.c` is a 32 KB heap buffer, allocated lazily on first call and reused.
- The URL buffer is `char url[512]` — large enough for the full URL with the optional `passList` field. Don't shrink it.

### `sbb_get_departures()` contract

Signature: `bool sbb_get_departures(const char *station, SbbDeparture results[4], const char *dest_filters[], int filter_count)`.

- **Count-based, not NULL-terminated.** `filter_count = 0` disables filtering entirely. `sizeof(arr)/sizeof(arr[0])` at the call site is the intended pattern.
- Station name is URL-encoded inside (space → `%20`), so callers pass the canonical name as it appears on sbb.ch (e.g. `"Basel SBB"`).
- Filter matches both the end destination (`to`) and intermediate stops (`passList/station/name`), case-insensitive substring. The `passList` field is requested from the API **only when `filter_count > 0`** to save bandwidth.
- Results are chosen to start at the first departure ≥ current HH:MM; if fewer than 4 future trains are available, the window backs up and some `results[i]` may have `valid = false`.

### Font and UTF-8

`main.c` ships a 5×7 uppercase-only font plus six custom umlaut glyphs (`ÄÖÜäöü`). Lowercase input is mapped to uppercase before lookup. Other Latin-1 accented chars (é, è, â, ô, ç, …) fall back to their base letter via range checks on the second UTF-8 byte (`0xC3 0xXX`), so `Delémont` renders as `DELEMONT`.
