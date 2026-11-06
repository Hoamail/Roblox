#pragma once
// Camera-derived view*projection.
//
// velext v1.0.2 does not read VisualEngine.ViewMatrix anywhere: its world ->
// screen (FUN_7ff7ddc3a120) takes one cached camera block (36 bytes rotation +
// 12 bytes position, i.e. the 48 bytes at Camera+0xC0) plus Camera.FieldOfView
// and Camera.ViewportSize, and projects with the camera basis:
//
//     d     = p - eye
//     depth = dot(d, fwd)                 // fwd = -third column
//     ndcX  = dot(d, right) / (tan(fov/2) * aspect * depth)
//     ndcY  = dot(d, up)    / (tan(fov/2) * depth)
//     px    = clientLeft + (ndcX + 1) * 0.5 * width
//     py    = clientTop  + (1 - ndcY) * 0.5 * height
//
// The theo dump for version-5a21b87bbc2641cf still lists VisualEngine.ViewMatrix
// = 432, but that block is a constant non-VP structure in this build (measured:
// 16 floats identical across 12 samples while the camera rotated, 0 of 2125
// candidate windows in ve[0,0x2400] ever changed). A dead matrix does not fail
// loudly: ESP boxes land nowhere, the FOV gate rejects every entity and the
// pixel deadzone never fires. Deriving the projection from the camera removes
// the dependency on that offset.
//
// The matrix is emitted in the convention src/ovl/ovl_view.h expects
// (sx = left + vw/2 * (1 + x/ww), sy = top + vh/2 * (1 - y/ww)), so every
// existing consumer (ESP, aim, radar) keeps working unchanged.

#include "ovl_types.h"
#include <cmath>

struct CamView {
    Vec3 eye{};
    Vec3 right{ 1, 0, 0 };
    Vec3 up{ 0, 1, 0 };
    Vec3 fwd{ 0, 0, -1 };
    float fovDeg = 70.0f;
    bool ok = false;
};

inline float camViewDot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline bool camViewFinite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline float camViewLen(const Vec3& v) { return sqrtf(camViewDot(v, v)); }

// rot = Camera.Rotation (9 floats, columns Right / Up / -Look).
inline bool camViewBuild(CamView& cv, const float rot[9], const Vec3& eye,
                         float fovDeg) {
    if (!rot) return false;
    for (int i = 0; i < 9; ++i)
        if (!std::isfinite(rot[i])) return false;
    if (!camViewFinite(eye)) return false;
    Vec3 r{ rot[0], rot[3], rot[6] };
    Vec3 u{ rot[1], rot[4], rot[7] };
    Vec3 f{ -rot[2], -rot[5], -rot[8] };
    float rl = camViewLen(r), ul = camViewLen(u), fl = camViewLen(f);
    // A live CFrame basis is orthonormal. Anything else is stale or a
    // half-written block: refuse instead of projecting garbage.
    if (!(rl > 0.9f && rl < 1.1f) || !(ul > 0.9f && ul < 1.1f) ||
        !(fl > 0.9f && fl < 1.1f))
        return false;
    r.x /= rl; r.y /= rl; r.z /= rl;
    u.x /= ul; u.y /= ul; u.z /= ul;
    f.x /= fl; f.y /= fl; f.z /= fl;
    if (fabsf(camViewDot(r, u)) > 0.02f || fabsf(camViewDot(r, f)) > 0.02f ||
        fabsf(camViewDot(u, f)) > 0.02f)
        return false;
    // FieldOfView is the vertical FOV in degrees. 0 / stale reads are common
    // while a place is loading; clamp to something renderable.
    if (!(fovDeg > 1.0f && fovDeg < 179.0f)) {
        if (fovDeg == 0.0f || !std::isfinite(fovDeg)) return false;
        fovDeg = 70.0f;
    }
    cv.eye = eye;
    cv.right = r;
    cv.up = u;
    cv.fwd = f;
    cv.fovDeg = fovDeg;
    cv.ok = true;
    return true;
}

inline bool camViewMatrix(const CamView& cv, float aspect, float out[16]) {
    if (!cv.ok || !out) return false;
    if (!(aspect > 0.05f && aspect < 20.0f)) return false;
    float tanHalf = tanf(cv.fovDeg * 0.5f * 0.017453292519943295f);
    if (!(tanHalf > 1e-3f)) return false;
    const float sx = 1.0f / aspect;
    for (int i = 0; i < 16; ++i) out[i] = 0.0f;
    out[0] = cv.right.x * sx;
    out[1] = cv.right.y * sx;
    out[2] = cv.right.z * sx;
    out[3] = -camViewDot(cv.right, cv.eye) * sx;
    out[4] = cv.up.x;
    out[5] = cv.up.y;
    out[6] = cv.up.z;
    out[7] = -camViewDot(cv.up, cv.eye);
    out[12] = cv.fwd.x * tanHalf;
    out[13] = cv.fwd.y * tanHalf;
    out[14] = cv.fwd.z * tanHalf;
    out[15] = -camViewDot(cv.fwd, cv.eye) * tanHalf;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(out[i])) return false;
    return true;
}

// Sanity gate for a matrix that came from memory (the VisualEngine matrix).
// A real view*projection has a unit-length forward row (the w row: w = 0 at the
// eye, w > 0 in front), coherent x/y scales (|row0|*aspect == |row1| ==
// 1/tan(fov/2)) and maps the eye + a point straight ahead onto the centre of
// the client rect. A stale constant blob fails at least one of those: the
// measured failure on 5a21b87b had row2 = (0,0,0,0.0998), i.e. no forward row
// at all, and everything projected megapixels away.
inline bool camViewMatrixSane(const float mt[16], const Vec3& eye, int vw, int vh) {
    if (!mt) return false;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(mt[i])) return false;
    Vec3 f{ mt[12], mt[13], mt[14] };
    float fl = camViewLen(f);
    if (!(fl > 1e-6f && fl < 1e6f)) return false;
    const float cw = vw > 0 ? (float)vw : 1920.0f;
    const float ch = vh > 0 ? (float)vh : 1080.0f;
    const float aspect = cw / ch > 0.05f ? cw / ch : 1.0f;
    // Coherent perspective scales: the x row and the y row carry the same
    // tan(fov/2) factor, so |row0| * aspect == |row1| whatever the matrix
    // puts that factor (the engine keeps it in the x/y rows, our synthesized
    // matrix keeps it in the w row). A blob with unrelated rows fails here.
    float gx = camViewLen(Vec3{ mt[0], mt[1], mt[2] });
    float gy = camViewLen(Vec3{ mt[4], mt[5], mt[6] });
    if (!(gy > 1e-3f && gy < 1e4f)) return false;
    if (!(gx > 0.5f * gy / aspect && gx < 2.0f * gy / aspect)) return false;
    // The w row must pass through the current eye: w(eye) == 0 for a real
    // view*projection. A block that is not anchored at the camera (stale, or
    // a matrix for another space) is off by |eye| and refused here.
    float we = mt[12] * eye.x + mt[13] * eye.y + mt[14] * eye.z + mt[15];
    if (!(fabsf(we) <= 2.0f * fl)) return false;
    Vec3 p{ eye.x + f.x * (20.0f / fl), eye.y + f.y * (20.0f / fl),
            eye.z + f.z * (20.0f / fl) };
    float x = mt[0] * p.x + mt[1] * p.y + mt[2] * p.z + mt[3];
    float y = mt[4] * p.x + mt[5] * p.y + mt[6] * p.z + mt[7];
    float w = mt[12] * p.x + mt[13] * p.y + mt[14] * p.z + mt[15];
    if (!(w > 1e-4f)) return false;
    float sx = cw * 0.5f * (1.0f + x / w);
    float sy = ch * 0.5f * (1.0f - y / w);
    if (!std::isfinite(sx) || !std::isfinite(sy)) return false;
    // A point straight ahead must land on the centre (5% tolerance covers the
    // 20-unit step and float error, not a wrong basis).
    return fabsf(sx - cw * 0.5f) <= 0.05f * cw &&
           fabsf(sy - ch * 0.5f) <= 0.05f * ch;
}

// Cross-check a memory matrix against the camera basis read the same frame.
// The dump key for VisualEngine.ViewMatrix can be dead (5a21b87b) or live
// (f0ea5da9); when it is live it tracks Camera.Rotation exactly, so a forward
// row that disagrees with the camera proves the block is stale.
inline bool camViewMatrixAgrees(const float mt[16], const CamView& cv,
                                float minDot = 0.98f) {
    if (!cv.ok || !mt) return false;
    Vec3 f{ mt[12], mt[13], mt[14] };
    float fl = camViewLen(f);
    if (!(fl > 1e-6f)) return false;
    f.x /= fl;
    f.y /= fl;
    f.z /= fl;
    return fabsf(camViewDot(f, cv.fwd)) >= minDot;
}
