#pragma once
#include "sdk.h"        // Memory, Offsets, SDK
#include "ovl_types.h"
#include <vector>
#include <windows.h>

// Bó biến main-local mà thân tab cần. Giá trị được chụp ngay trước dispatch,
// đúng thời điểm code inline cũ đọc chúng -> không đổi ngữ nghĩa.
struct MenuCtx {
    Memory& m;
    const Offsets& o;
    std::vector<Ent>& ents;
    uint64_t place = 0;
    bool haveWrite = false;
    bool aimOn = false;       // main(): aim toggle/hold state
    bool trigOn = false;
    bool acOn = false;
    bool flyApply = false;    // main(): cfg && hotkey (effective)
    bool speedApply = false;
    bool jumpApply = false;
    int dbgEnts = 0;
    float loopMs = 0.0f;
    DWORD nowTick = 0;   // = nowLoop trong main loop
};

void drawTabBody(MenuCtx& c);
void drawTabCombat(MenuCtx& c);
void drawTabVisual(MenuCtx& c);
void drawTabTeleport(MenuCtx& c);
void drawTabWorld(MenuCtx& c);
void drawTabServer(MenuCtx& c);
void drawTabTools(MenuCtx& c);
void drawTabMisc(MenuCtx& c);
void drawTabStunt(MenuCtx& c);
