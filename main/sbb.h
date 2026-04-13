#pragma once

#include <stdbool.h>

#define SBB_STATION        "Gelterkinden"

typedef struct {
    char time[6];           // "HH:MM"
    char destination[32];
    char platform[6];       // "3", "12", oder "" wenn nicht vorhanden
    int  delay;             // Verspätung in Minuten, 0 = pünktlich
    bool cancelled;
    bool valid;
} SbbDeparture;

void sbb_wifi_init(const char *ssid, const char *password);

// Liefert die nächsten 4 Abfahrten ab jetzt.
// dest_filters: NULL-terminierte Liste von Ziel-Teilstrings (case-insensitive).
//   Beispiel: { "Basel", "Olten", NULL } → nur Züge Richtung Basel/Olten
//   { NULL } → alle Abfahrten (kein Filter)
bool sbb_get_departures(SbbDeparture results[4], const char *dest_filters[]);