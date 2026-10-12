#pragma once
#include <windows.h>

#include <cstdint>

namespace app {

// Fixed playout delay for video AND audio relative to the sender's timestamp. It absorbs the iPhone's
// regular Wi-Fi pauses (measured: ~70-140 ms) and keeps picture and sound in sync.
constexpr int64_t kPlayoutDelayNs = 200'000'000;

// Timestamps further than this from "now" are considered unusable (clock not yet synchronized).
constexpr int64_t kMaxPlausibleOffsetNs = 2'000'000'000;

// Local clock in nanoseconds since 1970 - the same time base as raop_ntp_get_local_time() in the UxPlay library,
// into which it converts the sender timestamps (ntp_time_local).
inline int64_t NowNs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) * 100ULL);
}

// Returns the playout time for a sender timestamp, or 0 if the timestamp is unusable.
inline int64_t PlayoutTargetNs(uint64_t ptsNs) {
    if (ptsNs == 0) return 0;
    const int64_t pts = static_cast<int64_t>(ptsNs);
    const int64_t diff = NowNs() - pts;
    if (diff > kMaxPlausibleOffsetNs || diff < -kMaxPlausibleOffsetNs) return 0;
    return pts + kPlayoutDelayNs;
}

}  // namespace app
