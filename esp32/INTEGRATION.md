# Blink Web Panel — ESP32 Integration

## Übersicht

Das Web-Panel läuft direkt auf dem ESP32. Wenn das Gerät aktiv ist (im Zeitfenster oder per Button geweckt), ist das Panel unter `http://blink.local` erreichbar.

---

## Neue Dateien

```
esp32/
  web/
    index.html          ← Web-Panel (auf SPIFFS flashen)
  main/
    http_server.h/.c    ← HTTP-Server (SPIFFS + /api/config)
    nvs_config.h/.c     ← NVS Lesen/Schreiben
```

---

## Schritt-für-Schritt Integration

### 1. Dateien ins Repo kopieren

```bash
# Aus dem Design System in dein Repo kopieren:
cp esp32/main/http_server.{h,c}  /pfad/zu/blink/main/
cp esp32/main/nvs_config.{h,c}   /pfad/zu/blink/main/
mkdir -p /pfad/zu/blink/main/spiffs
cp esp32/web/index.html           /pfad/zu/blink/main/spiffs/

# Hinweis: Die Haupt-Firmware-Datei heisst main.c (nicht blink_example_main.c)
```

### 2. CMakeLists.txt anpassen

In `main/CMakeLists.txt` (Datei `main.c`, nicht `blink_example_main.c`):

```cmake
idf_component_register(
    SRCS
        "blink_example_main.c"
        "sbb.c"
        "cJSON.c"
        "http_server.c"      # NEU
        "nvs_config.c"       # NEU
    INCLUDE_DIRS "."
    REQUIRES
        driver led_strip esp_wifi esp_http_server
        nvs_flash spiffs esp_netif mdns        # NEU: mdns für blink.local
)

# SPIFFS Partition einbinden
spiffs_create_partition_image(storage main/spiffs FLASH_IN_PROJECT)  # NEU
```

### 3. Partition Table

Erstelle `partitions.csv` im Projektroot:

```
# Name,   Type, SubType, Offset,   Size,    Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  1500K,
storage,  data, spiffs,  ,         256K,
```

Dann in `sdkconfig.defaults`:
```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

### 4. blink_example_main.c anpassen

```c
#include "http_server.h"
#include "nvs_config.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mdns.h"

void app_main(void) {
    // NVS initialisieren (ganz am Anfang!)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Config laden (ersetzt die #define-Konstanten)
    blink_config_t cfg;
    nvs_config_load(&cfg);

    // Alle bisherigen #define durch cfg.xxx ersetzen, z.B.:
    // ACTIVE_START_H → cfg.startH
    // SBB_REFRESH_MS → cfg.refreshS * 1000
    // ...

    // Nach WiFi-Init: mDNS + HTTP-Server starten
    esp_netif_init();
    mdns_init();
    mdns_hostname_set("blink");
    mdns_instance_name_set("Blink ESP32");

    http_server_start();  // Panel unter http://blink.local

    // ... restliche app_main Logik ...
}
```

### 5. Flashen

```bash
idf.py build flash monitor
```

Das Panel ist dann unter **http://blink.local** erreichbar, solange der ESP wach ist.

---

## Hinweise

- Das Passwort wird beim GET `/api/config` **nicht** zurückgesendet (Sicherheit)
- Änderungen werden in NVS gespeichert und überleben Neustarts
- Der HTTP-Server sollte **vor** `go_to_sleep()` gestoppt werden: `http_server_stop()`
- mDNS (`blink.local`) funktioniert auf iOS/macOS direkt; Windows braucht ggf. Bonjour

