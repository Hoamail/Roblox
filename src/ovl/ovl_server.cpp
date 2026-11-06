// Server tab module, moved verbatim from overlay.cpp (S2.3).
#include "ovl_server.h"
#include "ovl_state.h"
#include "ovl_core.h"
#include "ovl_ui.h"
#include "ovl_cfg.h"
#include "offsets.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <shellapi.h>
#include <string>
#include <vector>

// ============================================================================
// Server tab (Đợt 4): session info, real ping, server list / hop / rejoin,
// auto-rejoin watchdog, anti-AFK.
//
// [P] READ-ONLY with respect to game memory: every value below is obtained
// through Memory/SDK reads. This module never writes a byte into the game.
// Data that leaves the process:
//   - HTTPS GETs to games.roblox.com / apis.roblox.com (public endpoints, no
//     auth) for the server list;
//   - HTTPS GET to ipwho.is ONLY when srv.geo is enabled (default off), and it
//     carries nothing but the server IP we already read;
//   - ShellExecuteW("roblox://...") for join / hop / rejoin;
//   - SendInput with a relative +1px / -1px mouse move for anti-AFK.
// No cookie, token or account data is ever read or sent.
// ============================================================================

static const int  kSrvHttpTimeoutMs = 4000;
static const DWORD kSrvListMinIntervalMs = 5000;
static const DWORD kSrvListRateLimitBackoffMs = 30000;
static const wchar_t* kSrvUa = L"ext-overlay/1.0 (server tab)";
static const wchar_t* kSrvUaGeo = L"ext-overlay/1.0 (server tab geo)";
// [I] If roblox:// ever stops launching the client, flip this to true: the
// join/hop buttons then use the web "games/start" URL (same params).
static const bool kSrvWebUriFallback = false;


// ---- tiny JSON helpers -----------------------------------------------------
// The offsets fetcher already parses its JSON by hand; pulling in a library to
// read five scalars would be a new dependency. These helpers only understand
// objects/arrays of scalars, which is exactly the shape of the two endpoints.
static bool srvJsonStr(const char*& p, const char* e, std::string& out) {
    if (p >= e || *p != '"') return false;
    ++p;
    out.clear();
    while (p < e) {
        char c = *p++;
        if (c == '"') return true;
        if (c == '\\' && p < e) c = *p++; // enough for ids/names/city names
        out += c;
    }
    return false;
}

// Find "key":value at object depth 1. Strings are consumed whole, so braces
// inside a name cannot confuse the depth counter; nested objects/arrays are
// skipped. quote=true reads a JSON string, false returns the raw token.
static bool srvJsonField(const char* b, const char* e, const char* key,
                         std::string& out, bool quote) {
    int depth = 0;
    const char* p = b;
    while (p < e) {
        char c = *p;
        if (c == '"') {
            std::string s;
            const char* q = p;
            if (!srvJsonStr(q, e, s)) return false;
            const char* r = q;
            while (r < e && (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n'))
                ++r;
            if (depth == 1 && s == key && r < e && *r == ':') {
                ++r;
                while (r < e &&
                       (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n'))
                    ++r;
                if (quote) return srvJsonStr(r, e, out);
                const char* st = r;
                while (r < e && *r != ',' && *r != '}' && *r != ']') ++r;
                while (r > st && (r[-1] == ' ' || r[-1] == '\t' ||
                                  r[-1] == '\r' || r[-1] == '\n'))
                    --r;
                if (r == st) return false;
                out.assign(st, r);
                return true;
            }
            p = q;
            continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            --depth;
            if (depth <= 0) break;
        }
        ++p;
    }
    return false;
}

// Count the strings of the array at "key" (used when a response omits
// "playing": public-server rows then only carry "playerTokens").
static int srvJsonCountArray(const char* b, const char* e, const char* key) {
    int depth = 0, n = 0;
    const char* p = b;
    while (p < e) {
        char c = *p;
        if (c == '"') {
            std::string s;
            const char* q = p;
            if (!srvJsonStr(q, e, s)) return n;
            const char* r = q;
            while (r < e && (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n'))
                ++r;
            if (depth == 1 && s == key && r < e && *r == ':') {
                ++r;
                while (r < e &&
                       (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n'))
                    ++r;
                if (r >= e || *r != '[') return 0;
                ++r;
                int d = 1;
                while (r < e && d > 0) {
                    char a = *r;
                    if (a == '"') {
                        if (d == 1) ++n;
                        std::string dummy;
                        const char* qq = r;
                        if (!srvJsonStr(qq, e, dummy)) return n;
                        r = qq;
                        continue;
                    }
                    if (a == '[' || a == '{') ++d;
                    else if (a == ']' || a == '}') --d;
                    ++r;
                }
                return n;
            }
            p = q;
            continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            --depth;
            if (depth <= 0) break;
        }
        ++p;
    }
    return n;
}

// Walk {"data":[{...},{...}]}: returns true when the array was found (an empty
// array is a valid answer: the place simply has no public servers).
static bool srvParseList(const std::string& body, std::vector<SrvRow>& rows) {
    rows.clear();
    const char* b = body.c_str();
    const char* e = b + body.size();
    const char* arr = nullptr;
    const char* p = b;
    while (p < e) {
        if (*p == '"') {
            std::string s;
            const char* q = p;
            if (!srvJsonStr(q, e, s)) break;
            const char* r = q;
            while (r < e && (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n'))
                ++r;
            if (s == "data" && r < e && *r == ':') {
                ++r;
                while (r < e &&
                       (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n'))
                    ++r;
                if (r < e && *r == '[') {
                    arr = r + 1;
                    break;
                }
            }
            p = q;
            continue;
        }
        ++p;
    }
    if (!arr) return false;
    p = arr;
    while (p < e) {
        if (*p == ']') break;
        if (*p != '{') {
            ++p;
            continue;
        }
        const char* ob = p;
        int depth = 0;
        const char* q = p;
        while (q < e) {
            if (*q == '"') {
                std::string s;
                const char* r = q;
                if (!srvJsonStr(r, e, s)) {
                    q = e;
                    break;
                }
                q = r;
                continue;
            }
            if (*q == '{' || *q == '[') ++depth;
            else if (*q == '}' || *q == ']') {
                --depth;
                if (depth == 0) {
                    ++q;
                    break;
                }
            }
            ++q;
        }
        SrvRow row;
        std::string v;
        if (srvJsonField(ob, q, "id", v, true) && !v.empty()) row.id = v;
        if (srvJsonField(ob, q, "maxPlayers", v, false))
            row.maxPlayers = atoi(v.c_str());
        if (srvJsonField(ob, q, "ping", v, false)) row.ping = atoi(v.c_str());
        if (srvJsonField(ob, q, "playing", v, false))
            row.playing = atoi(v.c_str());
        else
            row.playing = srvJsonCountArray(ob, q, "playerTokens");
        if (!row.id.empty()) rows.push_back(row);
        p = q;
    }
    return true;
}

// ---- state (main thread owns all of it; the worker only touches g_srvWire) --
SrvState srv;

// Worker -> UI/main handoff (same shape as g_snapMu for the aim snapshot).
SrvWire g_srvWire;
std::atomic<bool> g_srvRun{true};
static std::atomic<bool> g_srvWantList{false};
static std::atomic<unsigned long long> g_srvListPlace{0};
static std::atomic<DWORD> g_srvListNotBefore{0};
static std::atomic<bool> g_srvWantGeo{false};
static std::mutex g_srvGeoMu;
static std::string g_srvGeoIpReq;
static int g_srvListSeqSeen = 0;
static int g_srvGeoSeqSeen = 0;
int g_srvSel = -1; // selected row in the filtered list (UI only)

// ---- small helpers ---------------------------------------------------------
static std::string srvWideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr,
                        nullptr);
    return s;
}
static std::wstring srvUtf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// The panel cannot take keyboard focus (WS_EX_NOACTIVATE), so there is no
// InputText anywhere: clipboard is the only input path.
bool srvClipGet(std::string& out) {
    out.clear();
    if (!OpenClipboard(nullptr)) return false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
            out = srvWideToUtf8(w);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return !out.empty();
}
bool srvClipSet(const std::string& s) {
    if (s.empty()) return false;
    std::wstring w = srvUtf8ToWide(s);
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    bool ok = false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (mem) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            if (SetClipboardData(CF_UNICODETEXT, mem)) ok = true;
            else GlobalFree(mem); // ownership only transfers on success
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
    return ok;
}

// jobId = UUID with dashes (32..40 so both dashed and bare forms work; A-F
// accepted because Roblox has shipped uppercase ids before).
bool srvValidJob(const std::string& s) {
    if (s.size() < 32 || s.size() > 40) return false;
    for (char c : s) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F') || c == '-')
            continue;
        return false;
    }
    return true;
}
std::string srvShort(const std::string& j) {
    return j.size() > 8 ? j.substr(0, 8) : j;
}

// Display form of DataModel.ServerIP: keep the raw string, but for the geo
// lookup strip an optional "|port" / ":port" (IPv6 colons are left alone).
static std::string srvIpOnly(const std::string& raw) {
    std::string s = raw;
    size_t bar = s.find('|');
    if (bar != std::string::npos) s = s.substr(0, bar);
    size_t c = s.rfind(':');
    if (c != std::string::npos && s.find(':') == c) {
        std::string port = s.substr(c + 1);
        bool digits = !port.empty() && port.size() <= 5;
        for (char ch : port)
            if (ch < '0' || ch > '9') digits = false;
        if (digits) s = s.substr(0, c);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    if (s.find(':') != std::string::npos) return ""; // IPv6: skip geo
    return s;
}

// Clipboard input: bare jobId, roblox:// URI or the web start link. Params may
// appear in any order; a missing placeId means "current place".
bool srvParseJoin(const std::string& raw, uint64_t curPlace,
                         SrvJoin& out) {
    out = SrvJoin{};
    size_t a = raw.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    size_t z = raw.find_last_not_of(" \t\r\n");
    std::string s = raw.substr(a, z - a + 1);
    size_t jp = s.find("gameInstanceId=");
    size_t pp = s.find("placeId=");
    if (jp != std::string::npos) {
        size_t vs = jp + 15;
        size_t ve = s.find_first_of("&#", vs);
        std::string job = s.substr(vs, ve == std::string::npos
                                           ? std::string::npos
                                           : ve - vs);
        if (!srvValidJob(job)) return false;
        out.job = job;
        if (pp != std::string::npos) {
            size_t ps = pp + 8;
            size_t pe = s.find_first_of("&#", ps);
            std::string pv = s.substr(ps, pe == std::string::npos
                                              ? std::string::npos
                                              : pe - ps);
            out.place = strtoull(pv.c_str(), nullptr, 10);
        }
        if (!out.place) out.place = curPlace;
        return true;
    }
    if (srvValidJob(s)) {
        out.job = s;
        out.place = curPlace;
        return true;
    }
    return false;
}

static std::string srvBuildUri(uint64_t place, const std::string& job) {
    char b[320];
    if (kSrvWebUriFallback)
        snprintf(b, sizeof b,
                 "https://www.roblox.com/games/start?placeId=%llu&gameInstanceId=%s",
                 (unsigned long long)place, job.c_str());
    else
        snprintf(b, sizeof b,
                 "roblox://experiences/start?placeId=%llu&gameInstanceId=%s",
                 (unsigned long long)place, job.c_str());
    return b;
}
std::string srvBuildLink(uint64_t place, const std::string& job) {
    char b[320];
    snprintf(b, sizeof b,
             "https://www.roblox.com/games/start?placeId=%llu&gameInstanceId=%s",
             (unsigned long long)place, job.c_str());
    return b;
}

// Open a join URI. srv.lastjob/srv.lastplace are written to overlay.cfg BEFORE
// the URI is opened: hopping starts a new client and this overlay is expected
// to exit with the old one (accepted v1 behaviour, see HANDOFF).
void srvOpenJoin(uint64_t place, const std::string& job, const char* why) {
    if (!place) {
        toast("join: no placeId yet");
        return;
    }
    if (!srvValidJob(job)) {
        toast("join: bad jobId");
        return;
    }
    cfgSrvLastJob = job;
    cfgSrvLastPlace = place;
    upsertCfgKey("srv.lastjob", job.c_str());
    char pb[32];
    snprintf(pb, sizeof pb, "%llu", (unsigned long long)place);
    upsertCfgKey("srv.lastplace", pb);
    srv.joinCd = GetTickCount();
    srv.arFired = false;
    srv.unloadTick = 0;
    std::string uri = srvBuildUri(place, job);
    toast("joining %s..", srvShort(job).c_str());
    diagRate("srv-join", 0, "join place=%llu job=%s why=%s uri=%s",
             (unsigned long long)place, job.c_str(), why, uri.c_str());
    std::wstring w = srvUtf8ToWide(uri);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---- real ping (4.2) -------------------------------------------------------
// Stats -> "Data Ping" (StatsItem) -> Value. Value has been observed as double
// (1280 holds 8 bytes); float is tried as a fallback. Pointer cached, re-scan
// at most once per second, unknown => UI shows "--" and one diag line per 5 s.
static uint64_t g_srvPingItem = 0;
static DWORD g_srvPingFind = 0;
static int srvReadPing(Memory& m, SDK& sdk, const Offsets& o, uint64_t dm,
                       DWORD now) {
    if (!o.statsItemValue) return -1;
    if (!g_srvPingItem && now - g_srvPingFind >= 1000) {
        g_srvPingFind = now;
        uint64_t stats = 0;
        if (sdk.findChildByClass(dm, "Stats", stats)) {
            sdk.walkChildren(stats, [&](uint64_t it) {
                if (sdk.readName(it) == "Data Ping") {
                    g_srvPingItem = it;
                    return false;
                }
                return true;
            });
        }
        if (!g_srvPingItem)
            diagRate("srv-ping-miss", 5000,
                     "srv ping: Stats/Data Ping not found (showing --)");
    }
    if (!g_srvPingItem) return -1;
    int v = -1;
    double d = 0.0;
    if (m.read(g_srvPingItem + (uint64_t)o.statsItemValue, d)) {
        if (d >= 1.0 && d <= 100000.0) v = (int)d;
    }
    if (v < 0) {
        float f = 0.0f;
        if (m.read(g_srvPingItem + (uint64_t)o.statsItemValue, f)) {
            if (f >= 1.0f && f <= 100000.0f) v = (int)f;
        }
    }
    if (v < 0) {
        g_srvPingItem = 0; // instance changed under us; rescan next second
        diagRate("srv-ping-read", 5000,
                 "srv ping: value unreadable (showing --)");
    }
    return v;
}

// ---- filtering / requests --------------------------------------------------
std::vector<SrvRow> srvFilterRows(const std::vector<SrvRow>& in,
                                         const std::string& curJob) {
    std::vector<SrvRow> out;
    for (const SrvRow& r : in) {
        if (!srvValidJob(r.id)) continue;
        if (!curJob.empty() && _stricmp(r.id.c_str(), curJob.c_str()) == 0)
            continue; // never "hop" to the server we are already on
        if (r.playing < cfgSrvHopMin) continue;
        if (cfgSrvHopFresh && r.maxPlayers > 0 && r.playing >= r.maxPlayers)
            continue; // full
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const SrvRow& a, const SrvRow& b) {
        int pa = a.ping > 0 ? a.ping : 999999;
        int pb = b.ping > 0 ? b.ping : 999999;
        return pa < pb;
    });
    return out;
}

void srvRequestList(uint64_t place) {
    if (!place) {
        toast("no placeId yet");
        return;
    }
    DWORD now = GetTickCount();
    DWORD notBefore = g_srvListNotBefore.load(std::memory_order_relaxed);
    if (notBefore && (LONG)(now - notBefore) < 0) {
        DWORD waitMs = notBefore - now;
        toast("server list throttled - retry in %lus",
              (unsigned long)((waitMs + 999) / 1000));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_srvWire.mu);
        if (g_srvWire.listState == 1) return; // already loading
        g_srvWire.listState = 1;
    }
    g_srvListNotBefore.store(now + kSrvListMinIntervalMs,
                             std::memory_order_relaxed);
    g_srvListPlace.store((unsigned long long)place);
    g_srvWantList.store(true);
    diagRate("srv-list-req", 0, "srv list request place=%llu",
             (unsigned long long)place);
}

static void srvRequestGeo(const std::string& ip) {
    if (ip.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_srvWire.mu);
        if (g_srvWire.geoState == 1) return;
        g_srvWire.geoState = 1;
    }
    {
        std::lock_guard<std::mutex> lk(g_srvGeoMu);
        g_srvGeoIpReq = ip;
    }
    srv.geoTry = GetTickCount();
    g_srvWantGeo.store(true);
    diagRate("srv-geo-req", 0, "geo request ip=%s (srv.geo on)", ip.c_str());
}

// ---- HTTP fetch (worker thread) --------------------------------------------
// Primary endpoint per PLAN §4.3; on 401/403/odd JSON fall back to the
// apis.roblox.com universe lookup + the v2 server list.
static bool srvFetchList(uint64_t place, std::vector<SrvRow>& rows, int& code,
                         std::string& endpoint) {
    rows.clear();
    endpoint.clear();
    code = 0;
    if (!place) return false;
    char pth[320];
    snprintf(pth, sizeof pth,
             "/v1/games/%llu/servers/Public?sortOrder=Asc&excludeFullGames=true&limit=100",
             (unsigned long long)place);
    int st = 0;
    int firstFailure = 0;
    std::string body =
        httpGetEx(L"games.roblox.com", srvUtf8ToWide(pth), kSrvHttpTimeoutMs,
                  kSrvUa, &st);
    code = st;
    if (!g_srvRun.load(std::memory_order_relaxed)) return false; // shutting down
    if (st == 200 && srvParseList(body, rows)) {
        endpoint = "v1";
        return true;
    }
    firstFailure = st > 0 ? st : 0;
    diagRate("srv-list-v1", 0, "srv v1 list failed http=%d body=%d bytes -> v2",
             st, (int)body.size());

    snprintf(pth, sizeof pth, "/universes/v1/places/%llu/universe",
             (unsigned long long)place);
    st = 0;
    body = httpGetEx(L"apis.roblox.com", srvUtf8ToWide(pth), kSrvHttpTimeoutMs,
                     kSrvUa, &st);
    if (!g_srvRun.load(std::memory_order_relaxed)) return false;
    if (st != 200) {
        code = (firstFailure == 403 || firstFailure == 429) ? firstFailure : st;
        diagRate("srv-list-uni", 0, "srv universe lookup failed http=%d", st);
        return false;
    }
    std::string uni;
    if (!srvJsonField(body.c_str(), body.c_str() + body.size(), "universeId",
                      uni, false) ||
        uni.empty() || uni == "null") {
        code = st;
        diagRate("srv-list-uni", 0, "srv universe lookup: no universeId");
        return false;
    }
    snprintf(pth, sizeof pth,
             "/v2/games/%s/servers/Public?sortOrder=Asc&excludeFullGames=true&limit=100",
             uni.c_str());
    st = 0;
    body = httpGetEx(L"games.roblox.com", srvUtf8ToWide(pth), kSrvHttpTimeoutMs,
                     kSrvUa, &st);
    code = st;
    if (!g_srvRun.load(std::memory_order_relaxed)) return false;
    if (st == 200 && srvParseList(body, rows)) {
        endpoint = "v2";
        return true;
    }
    code = (firstFailure == 403 || firstFailure == 429) ? firstFailure : st;
    diagRate("srv-list-v2", 0, "srv v2 list failed http=%d", st);
    return false;
}

static bool srvFetchGeo(const std::string& ip, std::string& city,
                        std::string& country, int& code) {
    city.clear();
    country.clear();
    code = 0;
    if (ip.empty()) return false;
    std::string path = "/" + ip;
    int st = 0;
    std::string body =
        httpGetEx(L"ipwho.is", srvUtf8ToWide(path), kSrvHttpTimeoutMs,
                  kSrvUaGeo, &st);
    code = st;
    if (st != 200) return false;
    const char* b = body.c_str();
    const char* e = b + body.size();
    std::string okv;
    if (!srvJsonField(b, e, "success", okv, false) || okv != "true") return false;
    srvJsonField(b, e, "city", city, true);
    srvJsonField(b, e, "country", country, true);
    return !city.empty() || !country.empty();
}

// Worker: only fetches and publishes state. cfg/log writes stay on the main
// thread; the only thing it logs is diag (mutex-protected file).
void srvWorkerThread() {
    while (g_srvRun.load(std::memory_order_relaxed)) {
        bool did = false;
        if (g_srvWantList.exchange(false)) {
            uint64_t place = (uint64_t)g_srvListPlace.load();
            std::vector<SrvRow> rows;
            int code = 0;
            std::string ep;
            bool ok = srvFetchList(place, rows, code, ep);
            std::lock_guard<std::mutex> lk(g_srvWire.mu);
            g_srvWire.listCode = code;
            g_srvWire.listEndpoint = ep;
            if (code == 403 || code == 429)
                g_srvListNotBefore.store(
                    GetTickCount() + kSrvListRateLimitBackoffMs,
                    std::memory_order_relaxed);
            if (ok) {
                g_srvWire.rows.swap(rows);
                g_srvWire.listState = 2;
            } else {
                g_srvWire.listState = 3;
            }
            ++g_srvWire.listSeq;
            did = true;
        }
        if (g_srvWantGeo.exchange(false)) {
            std::string ip;
            {
                std::lock_guard<std::mutex> lk(g_srvGeoMu);
                ip = g_srvGeoIpReq;
            }
            std::string city, country;
            int code = 0;
            bool ok = srvFetchGeo(ip, city, country, code);
            std::lock_guard<std::mutex> lk(g_srvWire.mu);
            g_srvWire.geoIp = ip;
            g_srvWire.geoCity = city;
            g_srvWire.geoCountry = country;
            g_srvWire.geoState = ok ? 2 : 3;
            ++g_srvWire.geoSeq;
            did = true;
        }
        if (!did) Sleep(40);
    }
}

// ---- 4.10 roster diff (optional) -------------------------------------------
// Client-side approximation: names come from the entity list, which is name-
// only, so a player still streaming in can look like a join. To keep the
// toasts honest the first fill and any empty roster are skipped silently.
static void srvDiffRoster(const std::vector<std::string>& names) {
    std::set<std::string> cur;
    for (const std::string& n : names)
        if (!n.empty() && n.size() <= 32) cur.insert(n);
    if (srv.firstRoster || srv.roster.empty() || cur.empty()) {
        srv.firstRoster = false;
        srv.roster = cur;
        return;
    }
    int joined = 0, left = 0;
    for (const std::string& n : cur)
        if (!srv.roster.count(n)) {
            if (joined < 3) toast("%s joined the server", n.c_str());
            ++joined;
        }
    for (const std::string& n : srv.roster)
        if (!cur.count(n)) {
            if (left < 3) toast("%s left the server", n.c_str());
            ++left;
        }
    if (joined || left)
        diagRate("srv-roster", 0, "roster +%d -%d (%zu online)", joined, left,
                 cur.size());
    srv.roster.swap(cur);
}

static void srvDrainWire() {
    int lseq = 0, lstate = 0, lcode = 0, gseq = 0, gstate = 0;
    size_t n = 0;
    std::string ep, city, country, gip;
    {
        std::lock_guard<std::mutex> lk(g_srvWire.mu);
        lseq = g_srvWire.listSeq;
        lstate = g_srvWire.listState;
        lcode = g_srvWire.listCode;
        n = g_srvWire.rows.size();
        ep = g_srvWire.listEndpoint;
        gseq = g_srvWire.geoSeq;
        gstate = g_srvWire.geoState;
        city = g_srvWire.geoCity;
        country = g_srvWire.geoCountry;
        gip = g_srvWire.geoIp;
    }
    if (lseq && lseq != g_srvListSeqSeen) {
        g_srvListSeqSeen = lseq;
        if (lstate == 2) {
            diagRate("srv-list", 0, "srv list ok endpoint=%s http=%d servers=%zu",
                     ep.c_str(), lcode, n);
        } else {
            toast("server list failed (HTTP %d)", lcode);
            diagRate("srv-list", 0, "srv list FAIL endpoint=%s http=%d",
                     ep.c_str(), lcode);
        }
    }
    if (gseq && gseq != g_srvGeoSeqSeen) {
        g_srvGeoSeqSeen = gseq;
        if (gstate == 2) {
            srv.regionIp = gip;
            if (city.empty()) srv.region = country;
            else if (country.empty()) srv.region = city;
            else srv.region = city + ", " + country;
            diagRate("srv-geo", 0, "geo ip=%s -> %s", gip.c_str(),
                     srv.region.c_str());
        } else {
            srv.region = "?";
            diagRate("srv-geo", 0, "geo ip=%s FAIL", gip.c_str());
        }
    }
}

// ---- per-frame tick --------------------------------------------------------
// Called from the main loop next to worldTick(), i.e. BEFORE the "panel hidden"
// early-exit: the watchdog and anti-AFK must live outside the UI.
void srvTick(Memory& m, SDK& sdk, const Offsets& o, DWORD now,
                    bool fgGame, bool typingNow, bool aiming,
                    const std::vector<std::string>& names, int entCount) {
    if (now - srv.lastInfo >= 1000) {
        srv.lastInfo = now;
        uint64_t dm = 0;
        bool haveDm = sdk.dataModel(dm);
        bool loadedNow = false;
        if (haveDm) {
            uint64_t v = 0;
            if (o.dataModelPlaceId && sdk.placeId(dm, v) && v) srv.place = v;
            if (o.dataModelGameId && sdk.gameId(dm, v) && v) srv.gameId = v;
            int pv = 0;
            if (o.dataModelPlaceVersion &&
                m.read(dm + (uint64_t)o.dataModelPlaceVersion, pv))
                srv.placeVersion = pv;
            std::string job;
            if (o.dataModelJobId)
                job = sdk.readTStringDirect(dm + (uint64_t)o.dataModelJobId);
            if (srvValidJob(job)) {
                if (job != srv.jobId) {
                    // jobId changed => new server: reset every per-server bit.
                    srv.jobId = job;
                    srv.jobSeen = now;
                    srv.region.clear();
                    srv.regionIp.clear();
                    srv.ping = -1;
                    srv.firstRoster = true;
                    srv.roster.clear();
                    srv.arFired = false;
                    srv.unloadTick = 0;
                    g_srvPingItem = 0;
                    if (srv.place) {
                        cfgSrvLastJob = job;
                        cfgSrvLastPlace = srv.place;
                        upsertCfgKey("srv.lastjob", job.c_str());
                        char pb[32];
                        snprintf(pb, sizeof pb, "%llu",
                                 (unsigned long long)srv.place);
                        upsertCfgKey("srv.lastplace", pb);
                    }
                    diagRate("srv-job", 0, "srv job=%s place=%llu",
                             job.c_str(), (unsigned long long)srv.place);
                }
            } else if (!job.empty()) {
                diagRate("srv-badjob", 10000,
                         "srv jobId rejected (len=%d)", (int)job.size());
            }
            std::string ip;
            if (o.dataModelServerIP)
                ip = sdk.readTStringDirect(dm + (uint64_t)o.dataModelServerIP);
            if (!ip.empty()) srv.serverIP = ip; // keep old value on read error
            srv.ping = srvReadPing(m, sdk, o, dm, now);
            srv.players = (int)names.size();
            int loaded = 0;
            if (o.dataModelGameLoaded &&
                m.read(dm + (uint64_t)o.dataModelGameLoaded, loaded)) {
                loadedNow = loaded != 0;
                if (loadedNow) {
                    srv.wasLoaded = true;
                    srv.loaded = true;
                    srv.arFired = false;
                    srv.unloadTick = 0;
                } else {
                    srv.loaded = false;
                }
            }
        }
        srv.loaded = loadedNow;
        // 4.10 roster diff runs on the same 1 s sample as the entity cache.
        srvDiffRoster(names);

        // Region lookup: explicit opt-in, one request per IP (retry 60 s).
        if (cfgSrvGeo) {
            std::string ip = srvIpOnly(srv.serverIP);
            if (!ip.empty() && ip != srv.regionIp &&
                (!srv.geoTry || now - srv.geoTry >= 60000))
                srvRequestGeo(ip);
        }

        // Auto-rejoin: was loaded once, now unloaded, no entities for 5 s.
        // The 5 s window must be continuous: any sample that breaks the
        // condition resets the timer.
        bool arCond = cfgSrvAutoRejoin && srv.wasLoaded && !srv.loaded &&
                      entCount == 0 && !cfgSrvLastJob.empty() && !srv.arFired &&
                      (srv.joinCd == 0 || now - srv.joinCd >= 60000);
        if (!arCond) {
            srv.unloadTick = 0;
        } else if (!srv.unloadTick) {
            srv.unloadTick = now;
        } else if (now - srv.unloadTick >= 5000) {
            srv.arFired = true;
            srv.unloadTick = 0;
            toast("auto-rejoin %s..", srvShort(cfgSrvLastJob).c_str());
            diagRate("srv-autorejoin", 0,
                     "auto-rejoin job=%s place=%llu loaded=0 ents=0 held=5s",
                     cfgSrvLastJob.c_str(),
                     (unsigned long long)cfgSrvLastPlace);
            srvOpenJoin(cfgSrvLastPlace, cfgSrvLastJob, "auto-rejoin");
        }

        srvDrainWire();
    }

    // Anti-AFK: relative +1/-1 px, no click. Foreground game only, never while
    // typing in chat or while aim is held.
    if (cfgSrvAntiAfk && fgGame && !typingNow && !aiming) {
        if (!srv.afkNext) srv.afkNext = now + (DWORD)cfgSrvAfkMin * 60000;
        if ((long)(now - srv.afkNext) >= 0) {
            INPUT n1{};
            n1.type = INPUT_MOUSE;
            n1.mi.dx = 1;
            n1.mi.dy = 0;
            n1.mi.dwFlags = MOUSEEVENTF_MOVE;
            INPUT n2 = n1;
            n2.mi.dx = -1;
            SendInput(1, &n1, sizeof n1);
            SendInput(1, &n2, sizeof n2);
            srv.afkNext = now + (DWORD)cfgSrvAfkMin * 60000;
            diagRate("srv-antiafk", 0,
                     "anti-afk nudge +1/-1 px (interval %d min)", cfgSrvAfkMin);
        }
    } else if (!cfgSrvAntiAfk) {
        srv.afkNext = 0;
    }
}

// ---- self-test (-srvtest <placeId>): no game, no UI --------------------------
int runSrvSelfTest(uint64_t place) {
    printf("=== ext_overlay server tab self-test (no game) ===\n");
    printf("place=%llu\n\n", (unsigned long long)place);
    int fails = 0;

    printf("[1] jobId validation\n");
    {
        const char* good = "8b1a4e2c-3d4f-4a5b-9c6d-7e8f90123456";
        const char* good2 = "8B1A4E2C3D4F4A5B9C6D7E8F90123456";
        const char* bad1 = "";
        const char* bad2 = "abc";
        const char* bad3 = "8b1a4e2c-3d4f-4a5b-9c6d-7e8f9012345g";
        bool a = srvValidJob(good), b = srvValidJob(good2),
             c = !srvValidJob(bad1), d = !srvValidJob(bad2),
             f = !srvValidJob(bad3);
        printf("  uuid=%d bare=%d empty=%d short=%d badhex=%d %s\n", a, b, c, d,
               f, (a && b && c && d && f) ? "PASS" : "FAIL");
        if (!(a && b && c && d && f)) ++fails;
    }

    printf("[2] ServerIP -> IP for geo\n");
    {
        struct Case { const char* in; const char* out; };
        static const Case cs[] = {
            { "1.2.3.4|53640", "1.2.3.4" },
            { "1.2.3.4:53640", "1.2.3.4" },
            { "1.2.3.4", "1.2.3.4" },
            { "2001:db8::1", "" }, // IPv6: geo skipped
        };
        int ok = 0;
        for (const Case& c : cs) {
            std::string got = srvIpOnly(c.in);
            bool p = got == c.out;
            ok += p;
            printf("  %-18s -> %-10s %s\n", c.in, got.c_str(), p ? "PASS" : "FAIL");
        }
        if (ok != (int)(sizeof cs / sizeof cs[0])) ++fails;
    }

    printf("[3] clipboard join parser\n");
    {
        const char* job = "8b1a4e2c-3d4f-4a5b-9c6d-7e8f90123456";
        char uri[256], web[256];
        snprintf(uri, sizeof uri,
                 "roblox://experiences/start?gameInstanceId=%s&placeId=123", job);
        snprintf(web, sizeof web,
                 "https://www.roblox.com/games/start?placeId=123&gameInstanceId=%s",
                 job);
        SrvJoin j1, j2, j3, j4;
        bool r1 = srvParseJoin(job, 7, j1) && j1.job == job && j1.place == 7;
        bool r2 = srvParseJoin(uri, 7, j2) && j2.job == job && j2.place == 123;
        bool r3 = srvParseJoin(web, 7, j3) && j3.job == job && j3.place == 123;
        bool r4 = !srvParseJoin("hello world", 7, j4);
        printf("  bare=%d roblox=%d web=%d reject=%d %s\n", r1, r2, r3, r4,
               (r1 && r2 && r3 && r4) ? "PASS" : "FAIL");
        if (!(r1 && r2 && r3 && r4)) ++fails;
        std::string u = srvBuildUri(123, job);
        bool ru = u.find("roblox://experiences/start") == 0 &&
                  u.find(job) != std::string::npos;
        printf("  uri  %s %s\n", u.c_str(), ru ? "PASS" : "FAIL");
        if (!ru) ++fails;
    }

    printf("[4] server-list JSON parser\n");
    {
        const std::string body =
            "{\"previousPageCursor\":null,\"data\":["
            "{\"id\":\"8b1a4e2c-3d4f-4a5b-9c6d-7e8f90123456\",\"maxPlayers\":50,"
            "\"ping\":30,\"fps\":60,\"playerTokens\":[\"a\",\"b\",\"c\"]},"
            "{\"id\":\"9c2b5f3d-4e5a-4b6c-8d7e-0f1a2b3c4d5e\",\"maxPlayers\":50,"
            "\"playing\":12,\"ping\":90,\"fps\":59},"
            "{\"id\":\"not-a-job\",\"maxPlayers\":10,\"playing\":1,\"ping\":5}"
            "],\"nextPageCursor\":\"x\"}";
        std::vector<SrvRow> rows;
        bool ok = srvParseList(body, rows);
        bool shape = ok && rows.size() == 3 && rows[0].playing == 3 &&
                     rows[1].playing == 12 && rows[0].maxPlayers == 50 &&
                     rows[2].id == "not-a-job";
        printf("  parse=%d rows=%zu first_playing=%d second_playing=%d %s\n",
               ok ? 1 : 0, rows.size(),
               rows.empty() ? -1 : rows[0].playing,
               rows.size() < 2 ? -1 : rows[1].playing,
               shape ? "PASS" : "FAIL");
        if (!shape) ++fails;
    }

    printf("[5] hop filter\n");
    {
        int savedMin = cfgSrvHopMin, savedFresh = cfgSrvHopFresh;
        cfgSrvHopMin = 5;
        cfgSrvHopFresh = 1;
        std::vector<SrvRow> in;
        auto add = [&](const char* id, int pl, int mx, int pg) {
            SrvRow r;
            r.id = id;
            r.playing = pl;
            r.maxPlayers = mx;
            r.ping = pg;
            in.push_back(r);
        };
        add("8b1a4e2c-3d4f-4a5b-9c6d-7e8f90123456", 20, 50, 10); // current
        add("9c2b5f3d-4e5a-4b6c-8d7e-0f1a2b3c4d5e", 3, 50, 5);    // too empty
        add("0f1a2b3c-4d5e-4f60-8a7b-9c8d7e6f5a4b", 50, 50, 5);   // full
        add("11111111-2222-4333-8444-555555555555", 8, 50, 120);
        add("22222222-3333-4444-8555-666666666666", 30, 50, 40);
        std::vector<SrvRow> out =
            srvFilterRows(in, "8b1a4e2c-3d4f-4a5b-9c6d-7e8f90123456");
        bool ok = out.size() == 2 && out[0].ping == 40 && out[1].ping == 120;
        printf("  kept=%zu best_ping=%d %s\n", out.size(),
               out.empty() ? -1 : out[0].ping, ok ? "PASS" : "FAIL");
        cfgSrvHopMin = savedMin;
        cfgSrvHopFresh = savedFresh;
        if (!ok) ++fails;
    }

    printf("\n[6] network (WARN only - offline is not a failure)\n");
    {
        std::vector<SrvRow> rows;
        int code = 0;
        std::string ep;
        DWORD t0 = GetTickCount();
        bool ok = srvFetchList(place, rows, code, ep);
        DWORD dt = GetTickCount() - t0;
        printf("  list: %s endpoint=%s http=%d servers=%zu (%lu ms)\n",
               ok ? "OK" : "FAIL", ep.empty() ? "-" : ep.c_str(), code,
               rows.size(), (unsigned long)dt);
        for (size_t i = 0; i < rows.size() && i < 3; ++i)
            printf("    %s  %d/%d  ping %d\n", srvShort(rows[i].id).c_str(),
                   rows[i].playing, rows[i].maxPlayers, rows[i].ping);
        if (ok) {
            std::vector<SrvRow> filt = srvFilterRows(rows, "");
            printf("  filter(default hopmin=%d fresh=%d): %zu of %zu\n",
                   cfgSrvHopMin, cfgSrvHopFresh, filt.size(), rows.size());
        }
        std::string city, country;
        int gcode = 0;
        bool gok = srvFetchGeo("1.1.1.1", city, country, gcode);
        printf("  geo:  %s http=%d %s%s%s\n", gok ? "OK" : "WARN", gcode,
               city.c_str(), city.empty() ? "" : ", ", country.c_str());
    }

    printf("\n=== done (%s) ===\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
