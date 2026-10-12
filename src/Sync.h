#pragma once
#include <windows.h>

#include <cstdint>

namespace app {

// Feste Wiedergabeverzoegerung fuer Bild UND Ton relativ zum Zeitstempel des Senders. Sie faengt die
// regelmaessigen WLAN-Pausen des iPhones ab (gemessen: ~70-140 ms) und haelt Bild und Ton synchron.
constexpr int64_t kPlayoutDelayNs = 200'000'000;

// Zeitstempel weiter als das von "jetzt" entfernt gelten als unbrauchbar (Uhr noch nicht synchronisiert).
constexpr int64_t kMaxPlausibleOffsetNs = 2'000'000'000;

// Lokale Uhr in Nanosekunden seit 1970 - dieselbe Zeitbasis wie raop_ntp_get_local_time() der UxPlay-Bibliothek,
// in die sie die Sender-Zeitstempel (ntp_time_local) umrechnet.
inline int64_t NowNs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) * 100ULL);
}

// Liefert den Abspielzeitpunkt fuer einen Sender-Zeitstempel, oder 0 wenn der Zeitstempel unbrauchbar ist.
inline int64_t PlayoutTargetNs(uint64_t ptsNs) {
    if (ptsNs == 0) return 0;
    const int64_t pts = static_cast<int64_t>(ptsNs);
    const int64_t diff = NowNs() - pts;
    if (diff > kMaxPlausibleOffsetNs || diff < -kMaxPlausibleOffsetNs) return 0;
    return pts + kPlayoutDelayNs;
}

}  // namespace app
