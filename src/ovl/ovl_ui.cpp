#include "ovl_ui.h"
#include "ovl_state.h"
#include "imgui.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <utility>

std::vector<HitRect> g_menuHits;
void hitMark() { // call right after an interactive widget
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    // A widget scrolled out of its child is still "there" for ImGui: it draws
    // nothing and ignores clicks, but its raw rect would still enter the hit
    // list and swallow a click meant for the game. Clip to the draw list's clip
    // rect and drop whatever is left outside.
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 ca = d->GetClipRectMin(), cb = d->GetClipRectMax();
    if (a.x > cb.x || b.x < ca.x || a.y > cb.y || b.y < ca.y) return;
    if (a.x < ca.x) a.x = ca.x;
    if (a.y < ca.y) a.y = ca.y;
    if (b.x > cb.x) b.x = cb.x;
    if (b.y > cb.y) b.y = cb.y;
    g_menuHits.push_back({ a.x, a.y, b.x, b.y });
}

// ---- toggle-key config (overlay.cfg: toggle=INSERT) ----
int parseKey(const std::string& raw) {
    std::string s;
    for (char c : raw)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            s += (char)toupper((unsigned char)c);
    if (s.size() > 2 && s[0] == '0' && s[1] == 'X')
        return (int)strtoul(s.c_str(), nullptr, 16);
    if (s.size() >= 2 && s[0] == 'F') {
        int n = atoi(s.c_str() + 1);
        if (n >= 1 && n <= 24) return 0x6F + n;
    }
    static const std::pair<const char*, int> names[] = {
        {"INSERT", 0x2D}, {"DELETE", 0x2E}, {"DEL", 0x2E},
        {"HOME", 0x24}, {"END", 0x23}, {"PGUP", 0x21}, {"PGDN", 0x22},
        {"PAGEUP", 0x21}, {"PAGEDOWN", 0x22}, {"SPACE", 0x20},
        {"TAB", 0x09}, {"CAPS", 0x14}, {"CAPSLOCK", 0x14},
        {"\\", 0xDC}, {"BACKSLASH", 0xDC}, // VK_OEM_5
        {"LMB", 0x01}, {"MOUSE1", 0x01}, {"RMB", 0x02}, {"MOUSE2", 0x02},
        {"MMB", 0x04}, {"MOUSE3", 0x04},
        {"XBUTTON1", 0x05}, {"XBUTTON2", 0x06},
        {"XBTN1", 0x05}, {"XBTN2", 0x06}, {"MOUSE4", 0x05}, {"MOUSE5", 0x06},
    };
    for (auto& kv : names)
        if (s == kv.first) return kv.second;
    if (s.size() == 1) return (int)(unsigned char)s[0]; // A-Z, 0-9, symbols
    return 0;
}

// int-backed checkbox: the config flags are plain ints, ImGui wants bool*.
// Returns true on the frame it was clicked, so the caller can persist it.


bool chkInt(const char* label, int* v) {
    bool b = (*v != 0);
    if (ImGui::Checkbox(label, &b)) {
        *v = b ? 1 : 0;
        return true;
    }
    return false;
}
// same thing for the couple of settings that are plain bools
bool chkInt(const char* label, bool* v) { return ImGui::Checkbox(label, v); }

// slider + persist-on-release (writing the cfg file every frame of a drag
// would be pointless churn).
bool sliderFloatCfg(const char* label, float* v, float lo, float hi,
                           const char* fmt, const char* cfgKey) {
    ImGui::SetNextItemWidth(130);
    bool changed = ImGui::SliderFloat(label, v, lo, hi, fmt);
    if (ImGui::IsItemDeactivatedAfterEdit() && cfgKey) {
        char b[32];
        snprintf(b, sizeof b, "%.0f", *v);
        upsertCfgKey(cfgKey, b);
    }
    return changed;
}
bool sliderIntCfg(const char* label, int* v, int lo, int hi, const char* fmt,
                         const char* cfgKey) {
    ImGui::SetNextItemWidth(130);
    bool changed = ImGui::SliderInt(label, v, lo, hi, fmt);
    if (ImGui::IsItemDeactivatedAfterEdit() && cfgKey) {
        char b[32];
        snprintf(b, sizeof b, "%d", *v);
        upsertCfgKey(cfgKey, b);
    }
    return changed;
}

std::string keyName(int vk) {
    char n[16];
    if (vk >= 0x70 && vk <= 0x87) {
        snprintf(n, sizeof n, "F%d", vk - 0x6F);
        return n;
    }
    switch (vk) {
    case 0x2D: return "INSERT";
    case 0x2E: return "DELETE";
    case 0x24: return "HOME";
    case 0x23: return "END";
    case 0x21: return "PGUP";
    case 0x22: return "PGDN";
    case 0x20: return "SPACE";
    case 0xBA: return ";";
    case 0xDB: return "[";
    case 0xDD: return "]";
    case 0xDE: return "'";
    case 0xDC: return "\\";
    case 0x01: return "LMB";
    case 0x02: return "RMB";
    case 0x04: return "MMB";
    case 0x05: return "XBUTTON1";
    case 0x06: return "XBUTTON2";
    }
    if (vk >= 0x21 && vk < 0x7F) {
        char s[2] = { (char)vk, 0 };
        return s;
    }
    snprintf(n, sizeof n, "0x%X", vk);
    return n;
}

// Live "armed + key" state of one feature: dim when off, green while applying.
void keyStateLabel(int vk, bool applying) {
    char lbl[40];
    if (vk)
        snprintf(lbl, sizeof lbl, "[%s] %s", keyName(vk).c_str(),
                 applying ? "ON" : "off");
    else
        snprintf(lbl, sizeof lbl, "[no key]%s", applying ? " ON" : "");
    if (applying)
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", lbl);
    else
        ImGui::TextDisabled("%s", lbl);
}

// Keybind capture state: the panel never takes keyboard focus, so rebinding is
// polled in the main loop (see the capture block there), not by ImGui.
int* g_capVk = nullptr;         // slot being rebound
const char* g_capCfg = nullptr; // its overlay.cfg key
DWORD g_capT0 = 0;              // capture start (diag only)
bool g_capSeeded = false;       // previous down-state snapshot is initialized

void keyBindStart(int* vk, const char* cfgKey) {
    g_capVk = vk;
    g_capCfg = cfgKey;
    g_capT0 = GetTickCount();
    g_capSeeded = false; // first poll seeds the edge table from live state
}

void keyBindStop() {
    g_capVk = nullptr;
    g_capCfg = nullptr;
    g_capSeeded = false;
}

// One step of the keybind capture edge detector. `down` is the caller's
// GetAsyncKeyState snapshot (bit 0x8000 folded to bool, 256 entries).
//
// Only RISING edges count, and the edge table is seeded from the live state on
// the first step, so whatever is already held when the capture opens (the click
// itself, or a key some input hook left stuck in the async table) can neither
// bind itself nor wedge the capture. An earlier version instead waited until
// *every* vk was up before arming: one permanently-down vk in the async key
// state froze every bind button at "[press key]" forever, with no way out but
// pressing that exact (unknown) key again.
int keyBindPoll(const bool* down, DWORD nowMs, int* outVk) {
    static bool prev[0x100] = {};
    if (!g_capVk || !down || !outVk) return 0;
    if (!g_capSeeded) {
        memcpy(prev, down, sizeof prev);
        g_capSeeded = true;
        return 0;
    }
    // A press that started after the capture opened produces the first edge.
    for (int vk = 1; vk <= 0xFE; ++vk) {
        if (!down[vk] || prev[vk]) continue;
        memcpy(prev, down, sizeof prev);
        *outVk = (vk == VK_ESCAPE) ? 0 : vk;
        return 1;
    }
    memcpy(prev, down, sizeof prev);
    // Long-open capture (user walked away) is harmless: the table keeps
    // tracking, so the next real press still binds.
    (void)nowMs;
    return 0;
}

// Click to rebind: the next key/button press (see the capture poll in the main
// loop) becomes the hotkey. Escape during capture unbinds, a second click on
// the same button cancels.
void keyBindWidget(const char* label, int* vk, const char* cfgKey,
                          float w) {
    ImGui::PushID(cfgKey);
    bool cap = (g_capVk == vk);
    char lbl[64];
    if (cap)
        snprintf(lbl, sizeof lbl, "[press key] %s", label);
    else if (vk && *vk)
        snprintf(lbl, sizeof lbl, "[%s] %s", keyName(*vk).c_str(), label);
    else
        snprintf(lbl, sizeof lbl, "[none] %s", label);
    if (cap) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.35f, 0.25f, 1.0f));
    if (ImGui::Button(lbl, ImVec2(w, 0.0f))) {
        if (cap) {
            keyBindStop();
        } else if (vk) {
            keyBindStart(vk, cfgKey);
            if (g_diagState) {
                printf("bind start %s\n", cfgKey);
                fflush(stdout);
            }
        }
    }
    if (cap) ImGui::PopStyleColor();
    ImGui::PopID();
}

// ---- keybind capture self-test (-bindtest): pure logic, no game ------------
int runBindSelfTest() {
    int checks = 0, fails = 0;
    struct Sim {
        bool down[0x100] = {};
        int slot = 0;      // stands in for e.g. &cfgAimKey
        int result = -2;   // -2 = still waiting
        bool done = false;
        void start(const char* cfg = "test.key") {
            keyBindStop();
            slot = 0;
            result = -2;
            done = false;
            keyBindStart(&slot, cfg);
        }
        bool step() {
            int nv = 0;
            if (!keyBindPoll(down, 0, &nv)) return false;
            result = nv;
            done = true;
            keyBindStop();
            return true;
        }
        bool press(int vk) { down[vk] = true; return step(); }
        void release(int vk) { down[vk] = false; step(); }
    };
    auto chk = [&](const char* what, bool ok) {
        ++checks;
        if (!ok) {
            ++fails;
            printf("FAIL %s\n", what);
        }
    };
    printf("=== ext_overlay keybind capture self-test (no game) ===\n");
    {   // [1] the click that opens the capture must not become the hotkey
        Sim s; s.start();
        s.down[0x01] = true;                       // LMB still down at poll 1
        chk("[1] opening click does not bind", !s.step());
        s.release(0x01);
        chk("[1] release does not bind", !s.done);
        chk("[1] next press binds", s.press(0x4B) && s.result == 0x4B);
        printf("[1] click ignored, K captured: %s\n", s.result == 0x4B ? "ok" : "FAIL");
    }
    {   // [2] regression: one vk stuck down in the async table (input hook ate
        // the KEYUP) used to freeze every bind button at "[press key]".
        Sim s; s.start();
        s.down[0x4B] = true;
        chk("[2] seed with a stuck key", !s.step());
        bool wedged = false;
        for (int i = 0; i < 20; ++i) wedged = s.step() || wedged;
        chk("[2] stuck key must not complete a bind", !wedged);
        chk("[2] other key still binds", s.press(0x55) && s.result == 0x55);
        printf("[2] stuck vk 0x4B does not wedge capture: %s\n",
               s.result == 0x55 ? "ok" : "FAIL");
        Sim t; t.start();
        t.down[0x4B] = true; t.step();              // seeded while down
        chk("[2] stuck key never binds from stale state", !t.step());
    }
    {   // [3] Escape during capture = unbind (result 0 => cfg gets NONE)
        Sim s; s.start();
        s.step();                                   // idle seed poll
        chk("[3] Esc unbinds", s.press(0x1B) && s.result == 0);
    }
    {   // [4] a key already held when the capture opens cannot bind itself
        Sim s; s.start();
        s.down[0x41] = true;
        chk("[4] held-at-start does not bind", !s.step() && !s.step());
        s.release(0x41);
        chk("[4] repressing it binds", s.press(0x41) && s.result == 0x41);
    }
    {   // [5] nothing pressed -> capture stays open (no timeout, no auto-bind)
        Sim s; s.start();
        s.step();                                   // idle seed poll
        bool ever = false;
        for (int i = 0; i < 200; ++i) ever = s.step() || ever;
        chk("[5] idle capture stays open", !ever && !s.done);
    }
    {   // [6] one capture at a time; stop always clears the slot
        Sim s; s.start("a.key");
        int other = 0;
        keyBindStart(&other, "b.key");
        chk("[6] start replaces the slot", g_capVk == &other);
        keyBindStop();
        chk("[6] stop clears the slot", g_capVk == nullptr && g_capCfg == nullptr);
        chk("[6] poll on an idle capture is a no-op", !s.step());
    }
    printf("=== %s (%d checks, %d fails) ===\n", fails ? "FAIL" : "PASS",
           checks, fails);
    return fails ? 1 : 0;
}

// A scrolling child must own its pixels: without this the wheel slides off an
// unmarked pixel, falls through to the game and zooms the camera instead.
void markChildArea() {
    ImVec2 cp = ImGui::GetWindowPos();
    ImVec2 cs = ImGui::GetWindowSize();
    g_menuHits.push_back({ cp.x, cp.y, cp.x + cs.x, cp.y + cs.y });
}

char g_toast[96] = { 0 };      // transient panel message (panic, key, wp)
DWORD g_toastTick = 0;
void toast(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_toast, sizeof g_toast, fmt, ap);
    va_end(ap);
    g_toastTick = GetTickCount();
}
