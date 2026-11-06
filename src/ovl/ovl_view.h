#pragma once
// Screen projection shared by ESP (S3.2) and aim (S3.3a). Moved from the
// overlay.cpp w2s lambda; parameters are explicit so callers keep their state.
#include "ovl_types.h"

inline bool screenW2S(const Vec3& w, const float* mt, int& sx, int& sy,
                      int vpx, int vpy, int vpw, int vph, int sw, int sh) {
    // Project into the GAME client rect (not fullscreen metrics:
    // windowed game => different origin/scale => systematic offset).
    int vw = vpw ? vpw : sw, vh = vph ? vph : sh;
    int ox = vpw ? vpx : 0, oy = vph ? vpy : 0;
    float x = mt[0] * w.x + mt[1] * w.y + mt[2] * w.z + mt[3];
    float y = mt[4] * w.x + mt[5] * w.y + mt[6] * w.z + mt[7];
    float ww = mt[12] * w.x + mt[13] * w.y + mt[14] * w.z + mt[15];
    if (ww < 0.1f) return false;
    sx = ox + (int)((vw / 2) * (1.0f + x / ww));
    sy = oy + (int)((vh / 2) * (1.0f - y / ww));
    return true;
}
