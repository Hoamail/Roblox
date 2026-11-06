#pragma once
#include <windows.h>

// diagRate: defined in overlay.cpp (g_diagFile + mutex), used by server/world.
void diagRate(const char* key, DWORD intervalMs, const char* fmt, ...);

// Timing helpers: defined in overlay.cpp, used by aim/cache.
double qpcSeconds();
uint64_t qpcNs();

// Camera.FieldOfView unit trap. The dump key is stable but the *unit* is not:
// on 11f155f3 / f0ea5da9 the slot stores radians (70 deg reads 1.22173), on
// older builds it stored degrees. A radian value fed to tanf() as degrees
// collapses the whole projection (1.22 deg => ~66x zoom => every ESP box and
// name lands thousands of pixels off screen), and the aim focal check
// (10 < fov < 150) silently falls back to the legacy gain.
// Roblox clamps the property to [1, 120] degrees, so anything below 3.2 is a
// radian value (1.22 rad == 69.9 deg); treat it as such.
inline float camFovDegrees(float raw) {
    if (!(raw > 0.0f)) return 0.0f;
    return raw < 3.2f ? raw * 57.29577951308232f : raw;
}
