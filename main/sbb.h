#pragma once

#include <stdbool.h>

// Eine Abfahrt ab dem konfigurierten Bahnhof
typedef struct {
    char time[6];           // "HH:MM"
    char destination[32];   // Endziel, z.B. "Basel SBB"
    char platform[6];       // Gleis, z.B. "3" oder "" wenn unbekannt
    int  delay;             // Verspätung in Minuten (0 = pünktlich)
    bool cancelled;         // true = Zug fällt aus
    bool valid;             // true = Eintrag enthält Daten
} SbbDeparture;

// WiFi verbinden (einmal aufrufen)
void sbb_wifi_init(const char *ssid, const char *password);

// Nächste 4 Abfahrten ab jetzt holen.
//   station:      Bahnhof-Name wie auf sbb.ch (z.B. "Gelterkinden")
//   dest_filters: NULL-terminierte Liste von Ziel-Teilstrings, case-insensitive
//                 { "Basel", "Olten", NULL } = nur Züge Richtung Basel/Olten
//                 { NULL }                   = alle Züge (kein Filter)
bool sbb_get_departures(const char *station, SbbDeparture results[4],
                        const char *dest_filters[]);
