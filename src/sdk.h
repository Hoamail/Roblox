#pragma once
// P1 SDK: pointer-chain walkers over live offsets. No hardcoded addresses;
// every step resolves through Offsets (per-version pinned).
#include "memory.h"
#include "offsets.h"
#include <cstdint>
#include <cstring>
#include <string>

struct Vec3 { float x = 0, y = 0, z = 0; };

// Viewport-shift payload (two shorts at Camera.Viewport).
struct VP2 {
    short x, y;
};

class SDK {
public:
    Memory& m;
    const Offsets& o;
    uint64_t base = 0;

    SDK(Memory& mem, const Offsets& off) : m(mem), o(off) {}

    bool moduleBase(const std::wstring& mod) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, m.pid);
        if (snap == INVALID_HANDLE_VALUE) return false;
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        bool ok = false;
        if (Module32FirstW(snap, &me)) {
            do {
                if (mod == me.szModule) { base = (uint64_t)me.modBaseAddr; ok = true; break; }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return ok;
    }

    bool dataModel(uint64_t& dm) {
        uint64_t fake = 0;
        if (!m.read(base + o.fakeDataModelPtr, fake) || !fake) return false;
        return m.read(fake + (uint64_t)o.fakeRealDataModel, dm) && dm;
    }
    bool workspace(uint64_t dm, uint64_t& ws) {
        return m.read(dm + (uint64_t)o.dataModelWorkspace, ws) && ws;
    }
    bool camera(uint64_t ws, uint64_t& cam) {
        return m.read(ws + (uint64_t)o.workspaceCurrentCamera, cam) && cam;
    }
    bool cameraPos(uint64_t cam, Vec3& v) {
        return m.read(cam + (uint64_t)o.cameraPosition, v);
    }
    bool placeId(uint64_t dm, uint64_t& id) {
        // 64-bit: modern PlaceIds exceed 32 bits.
        return m.read(dm + (uint64_t)o.dataModelPlaceId, id);
    }
    bool gameId(uint64_t dm, uint64_t& id) {
        return m.read(dm + (uint64_t)o.dataModelGameId, id);
    }

    // First printable run (>=3 chars) within 48 bytes. Handles both
    // inline SSO strings and length-prefixed blobs.
    // Bulk reads (8B per syscall): byte-by-byte RPM here once froze the
    // render thread for seconds on garbage walks. Never regress this.
    std::string readRun(uint64_t addr) {
        unsigned char buf[48]{};
        for (int i = 0; i < 48; i += 8) {
            uint64_t q = 0;
            if (!m.read(addr + (uint64_t)i, q)) break;
            memcpy(&buf[i], &q, 8);
        }
        std::string best, cur;
        for (int i = 0; i < 48; ++i) {
            unsigned char c = (unsigned char)buf[i];
            if (c >= 32 && c < 127) {
                cur += (char)c;
                if (cur.size() > best.size()) best = cur;
            } else {
                cur.clear();
            }
            if (c == 0 && i > 0 && buf[i - 1] == 0) break;
        }
        return best.size() >= 3 ? best : "";
    }

    // Instance name: TString lives at container+8.
    std::string readName(uint64_t obj) {
        uint64_t cont = 0;
        if (!m.read(obj + 112, cont) || !cont) return "";
        return readTStringDirect(cont + 8);
    }

    std::string readClass(uint64_t obj) {
        uint64_t desc = 0, dstr = 0;
        if (!m.read(obj + 24, desc) || !desc) return "";
        if (!m.read(desc + 8, dstr) || !dstr) return "";
        return readRun(dstr);
    }

    static bool plausiblePtr(uint64_t v) {
        return v > 0x10000ULL && v < 0x7FFFFFFFFFFFULL;
    }

    // Decode a TString living AT addr: 16 bytes that are either inline
    // chars (first 8 all printable-or-zero) or a heap pointer.
    // Pointer targets are validated before following (avoids dereferencing
    // inline bytes that merely look like pointers, e.g. "Players").
    std::string readTStringDirect(uint64_t addr) {
        unsigned char buf[16]{};
        for (int i = 0; i < 16; i += 8) {
            uint64_t q = 0;
            if (!m.read(addr + (uint64_t)i, q)) return "";
            memcpy(&buf[i], &q, 8);
        }
        bool first8ok = true;
        for (int i = 0; i < 8; ++i) {
            unsigned char c = buf[i];
            if (c != 0 && (c < 32 || c >= 127)) {
                first8ok = false;
                break;
            }
        }
        if (first8ok) {
            char s[17]{};
            memcpy(s, buf, 16);
            return std::string(s);
        }
        uint64_t p = 0;
        memcpy(&p, buf, 8);
        if (!plausiblePtr(p) || (p & 7)) return "";
        // target must start with text, else it isn't a string
        uint64_t hq = 0;
        if (!m.read(p, hq)) return "";
        unsigned char head[8]{};
        memcpy(head, &hq, 8);
        if (head[0] < 32 || head[0] >= 127) return "";
        return readRun(p);
    }

    // Iterate direct children (stride-16 shared refs). cb returns false to stop.
    template <typename F>
    void walkChildren(uint64_t obj, F cb) {
        uint64_t vec = 0, b = 0, e = 0;
        if (!m.read(obj + (uint64_t)o.instanceChildrenStart, vec)) return;
        if (!m.read(vec, b) || !m.read(vec + 8, e)) return;
        if (e <= b || e - b > 0x20000) return;
        long long n = (long long)((e - b) / 16);
        if (n > 2000) return;
        for (long long i = 0; i < n; ++i) {
            uint64_t child = 0;
            if (!m.read(b + (uint64_t)i * 16, child) || !child) continue;
            if (!cb(child)) break;
        }
    }
    bool findChildByClass(uint64_t obj, const std::string& cls, uint64_t& out) {
        bool found = false;
        walkChildren(obj, [&](uint64_t c) {
            if (readClass(c) == cls) {
                out = c;
                found = true;
                return false;
            }
            return true;
        });
        return found;
    }
};

// Largest visible top-level window of a pid -> its CLIENT rect in screen
// coords (what the game actually renders into; differs from fullscreen
// metrics when windowed). Free functions (EnumWindows needs statics).
struct WinPick {
    DWORD pid;
    HWND hwnd;
    int area;
};

static BOOL CALLBACK pickCb(HWND h, LPARAM l) {
    WinPick* p = (WinPick*)l;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != p->pid || !IsWindowVisible(h)) return TRUE;
    RECT r{};
    GetWindowRect(h, &r);
    int a = (r.right - r.left) * (r.bottom - r.top);
    if (a > p->area) {
        p->area = a;
        p->hwnd = h;
    }
    return TRUE;
}

inline bool gameClientRect(DWORD pid, RECT& out) {
    WinPick p{ pid, nullptr, 0 };
    EnumWindows(pickCb, (LPARAM)&p);
    if (!p.hwnd) return false;
    POINT pt{ 0, 0 };
    ClientToScreen(p.hwnd, &pt);
    RECT cr{};
    GetClientRect(p.hwnd, &cr);
    out.left = pt.x;
    out.top = pt.y;
    out.right = pt.x + (cr.right - cr.left);
    out.bottom = pt.y + (cr.bottom - cr.top);
    return (out.right > out.left) && (out.bottom > out.top);
}