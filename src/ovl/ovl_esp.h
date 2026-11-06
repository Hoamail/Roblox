#pragma once
// ESP boxes / names / health / skeleton / off-screen arrows + radar.
// Moved verbatim from overlay.cpp in S3.2; no owned state.
#include "ovl_types.h"

struct ImDrawList;

struct EspCtx {
    EspCtx(Memory& m_, const Offsets& o_, std::vector<Ent>& ents_,
           int& vpx_, int& vpy_, int& vpw_, int& vph_,
           int& sw_, int& sh_, uint64_t& camAddr_)
      : m(m_), o(o_), ents(ents_), vpx(vpx_), vpy(vpy_), vpw(vpw_),
        vph(vph_), sw(sw_), sh(sh_), camAddr(camAddr_) {}

    Memory& m;
    const Offsets& o;
    std::vector<Ent>& ents;
    int& vpx;
    int& vpy;
    int& vpw;
    int& vph;
    int& sw;
    int& sh;
    uint64_t& camAddr;

    void draw(ImDrawList* dl, const float* vm);

private:
    bool w2s(const Vec3& w, const float* mt, int& sx, int& sy);
};
