// OffsetManager implementation. WinHTTP, no third-party deps.
// Tiny JSON reader: brace-matched sections + "Key":number pairs.
#include "offsets.h"
#include <windows.h>
#include <winhttp.h>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace {

// The offsets fetcher's call shape; the single implementation is below and is
// shared with the Server tab (Đợt 4) so the HTTP code cannot drift.
std::string httpGet(const std::wstring& host, const std::wstring& path) {
    return httpGetEx(host, path, 0, L"ext-p0/1.0", nullptr);
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Extract {...} of "Name":{...} with brace matching. Empty if missing.
std::string section(const std::string& json, const std::string& name) {
    std::string key = "\"" + name + "\":{";
    size_t p = json.find(key);
    if (p == std::string::npos) return "";
    size_t i = p + key.size() - 1; // at '{'
    int depth = 0;
    size_t start = i;
    for (; i < json.size(); ++i) {
        if (json[i] == '{') ++depth;
        else if (json[i] == '}') {
            if (--depth == 0) return json.substr(start, i - start + 1);
        }
    }
    return "";
}

uint64_t num(const std::string& sec, const std::string& key) {
    std::string k = "\"" + key + "\":";
    size_t p = sec.find(k);
    if (p == std::string::npos) return 0;
    return std::strtoull(sec.c_str() + p + k.size(), nullptr, 10);
}

std::string strVal(const std::string& json, const std::string& key) {
    std::string k = "\"" + key + "\":\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return "";
    size_t s = p + k.size();
    size_t e = json.find('"', s);
    return e == std::string::npos ? "" : json.substr(s, e - s);
}

bool fill(const std::string& json, Offsets& o) {
    if (json.find("\"Offsets\"") == std::string::npos) return false;
    o.robloxVersion = strVal(json, "Roblox Version");
    std::string ts = section(json, "TaskScheduler");
    std::string ve = section(json, "VisualEngine");
    std::string dm = section(json, "DataModel");
    std::string ws = section(json, "Workspace");
    std::string pl = section(json, "Player");
    std::string hu = section(json, "Humanoid");
    std::string in = section(json, "Instance");
    std::string fm = section(json, "FakeDataModel");
    std::string cm = section(json, "Camera");
    o.taskSchedulerPtr      = num(ts, "Pointer");
    o.dataModelWorkspace    = (int)num(dm, "Workspace");
    o.workspaceCurrentCamera = (int)num(ws, "CurrentCamera");
    o.playerLocalPlayer     = (int)num(pl, "LocalPlayer");
    o.humanoidHealth        = (int)num(hu, "Health");
    o.humanoidMaxHealth     = (int)num(hu, "MaxHealth");
    o.humanoidWalkSpeed     = (int)num(hu, "Walkspeed");
    o.humanoidWalkSpeedShadow = (int)num(hu, "WalkspeedCheck");
    if (!o.humanoidWalkSpeedShadow) o.humanoidWalkSpeedShadow = 0x3A4; // 932
    o.humanoidJumpPower     = (int)num(hu, "JumpPower");
    o.humanoidJumpHeight    = (int)num(hu, "JumpHeight");
    if (!o.humanoidJumpHeight) o.humanoidJumpHeight = 0x19C; // 412
    o.humanoidUseJumpPower  = (int)num(hu, "UseJumpPower");
    if (!o.humanoidUseJumpPower) o.humanoidUseJumpPower = 0x1DD; // 477
    o.visualEngineViewMatrix = (int)num(ve, "ViewMatrix");
    o.instanceChildrenStart = (int)num(in, "ChildrenStart");
    o.instanceParent      = (int)num(in, "Parent");
    o.instanceChildrenEnd   = (int)num(in, "ChildrenEnd");
    o.instanceName          = (int)num(in, "Name");
    o.fakeDataModelPtr  = num(fm, "Pointer");
    o.fakeRealDataModel = (int)num(fm, "RealDataModel");
    o.dataModelPlaceId  = (int)num(dm, "PlaceId");
    o.dataModelGameId   = (int)num(dm, "GameId");
    o.dataModelGameLoaded = (int)num(dm, "GameLoaded");
    // Đợt 4 (Server tab): session identity + ping. A build without these keys
    // parses them as 0 and every dependent widget stays "not found" (no crash).
    o.dataModelJobId       = (int)num(dm, "JobId");
    o.dataModelServerIP    = (int)num(dm, "ServerIP");
    o.dataModelPlaceVersion = (int)num(dm, "PlaceVersion");
    {
        std::string st = section(json, "StatsItem");
        o.statsItemValue = (int)num(st, "Value");
    }
    o.cameraPosition    = (int)num(cm, "Position");
    o.cameraFOV         = (int)num(cm, "FieldOfView");
    { int r = (int)num(cm, "Rotation"); if (r) o.cameraRotation = r; }
    std::string vef = section(json, "VisualEngine");
    std::string plo = section(json, "Player");
    std::string huo = section(json, "Humanoid");
    std::string bpo = section(json, "BasePart");
    std::string pro = section(json, "Primitive");
    o.visualEnginePtr     = num(vef, "Pointer");
    o.visualEngineDimensions = (int)num(vef, "Dimensions");
    o.cameraViewport      = (int)num(cm, "Viewport");
    o.playersLocalPlayer  = (int)num(plo, "LocalPlayer");
    o.playerModelInstance = (int)num(plo, "ModelInstance");
    o.playerUserId        = (int)num(plo, "UserId");
    o.playerDisplayName   = (int)num(plo, "DisplayName");
    o.playerTeam          = (int)num(plo, "Team");
    o.humanoidHRP         = (int)num(huo, "HumanoidRootPart");
    o.basePartPrimitive   = (int)num(bpo, "Primitive");
    o.primitivePosition   = (int)num(pro, "Position");
    o.primitiveVelocity   = (int)num(pro, "AssemblyLinearVelocity");
    o.primitiveSize       = (int)num(pro, "Size");
    o.primitiveFlags      = (int)num(pro, "Flags");
    { std::string prf = section(json, "PrimitiveFlags");
      int cc = (int)num(prf, "CanCollide");
      if (cc) o.primitiveCanCollideBit = cc;
      int an = (int)num(prf, "Anchored");
      if (an) o.primitiveAnchoredBit = an; }
    std::string uis = section(json, "UserInputService");
    std::string wstate = section(json, "WindowInputState");
    o.userInputWS         = (int)num(uis, "WindowInputState");
    o.windowTextBox       = (int)num(wstate, "CurrentTextBox");
    // ---- Đợt 2: World tab (Lighting / Atmosphere / effects / Terrain) ------
    std::string li = section(json, "Lighting");
    std::string atm = section(json, "Atmosphere");
    std::string bfx = section(json, "BloomEffect");
    std::string lfx = section(json, "BlurEffect");
    std::string ter = section(json, "Terrain");
    o.lightingClockTime           = (int)num(li, "ClockTime");
    o.lightingBrightness          = (int)num(li, "Brightness");
    o.lightingAmbient             = (int)num(li, "Ambient");
    o.lightingOutdoorAmbient      = (int)num(li, "OutdoorAmbient");
    o.lightingExposureCompensation = (int)num(li, "ExposureCompensation");
    o.lightingFogStart            = (int)num(li, "FogStart");
    o.lightingFogEnd              = (int)num(li, "FogEnd");
    o.lightingGlobalShadows       = (int)num(li, "GlobalShadows");
    o.atmosphereDensity           = (int)num(atm, "Density");
    o.atmosphereHaze              = (int)num(atm, "Haze");
    o.atmosphereGlare             = (int)num(atm, "Glare");
    o.effectEnabled               = (int)num(bfx, "Enabled");
    o.terrainGrassLength          = (int)num(ter, "GrassLength");
    o.terrainWaterReflectance     = (int)num(ter, "WaterReflectance");
    o.terrainWaterTransparency    = (int)num(ter, "WaterTransparency");
    o.terrainWaterWaveSize        = (int)num(ter, "WaterWaveSize");
    o.playerMinZoomDistance       = (int)num(plo, "MinZoomDistance");
    o.playerMaxZoomDistance       = (int)num(plo, "MaxZoomDistance");
    (void)lfx; // BlurEffect.Size shares the Bloom slot; only Enabled is used
    return !o.robloxVersion.empty() && o.taskSchedulerPtr != 0;
}

std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// Single HTTPS-GET path for the whole project. The Server tab (Đợt 4) passes a
// 4 s budget + its own UA; the offsets fetcher keeps WinHTTP's defaults.
std::string httpGetEx(const std::wstring& host, const std::wstring& path,
                      int timeoutMs, const wchar_t* userAgent, int* status) {
    if (status) *status = -1;
    std::string out;
    HINTERNET s = WinHttpOpen(userAgent ? userAgent : L"ext-p0/1.0",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return out;
    if (timeoutMs > 0) {
        // resolve / connect / send / receive: same budget for all four so one
        // dead endpoint can never stall a caller for more than ~4 * timeout.
        WinHttpSetTimeouts(s, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    }
    HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!c) { WinHttpCloseHandle(s); return out; }
    HINTERNET r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
    if (!r) { WinHttpCloseHandle(c); WinHttpCloseHandle(s); return out; }
    if (!WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(r, nullptr)) {
        WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s);
        return out;
    }
    if (status) {
        DWORD code = 0, len = sizeof(code);
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                                WINHTTP_NO_HEADER_INDEX))
            *status = (int)code;
    }
    DWORD avail = 0, got = 0;
    char buf[8192];
    while (WinHttpQueryDataAvailable(r, &avail) && avail > 0) {
        DWORD chunk = avail > sizeof(buf) ? sizeof(buf) : avail;
        if (!WinHttpReadData(r, buf, chunk, &got) || got == 0) break;
        out.append(buf, got);
        if (out.size() > 4u * 1024 * 1024) break; // a rogue endpoint can't blow RAM
    }
    WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return out;
}

bool OffsetManager::ensure(const std::string& runningVersion,
                           const std::string& cachePath,
                           Offsets& out,
                           std::string& log) {
    const std::wstring host = L"offsets.imtheo.lol";
    std::string latest = trim(httpGet(host, L"/roblox/version"));
    if (!latest.empty()) log += "server roblox version: " + latest + "\n";
    else log += "WARN: version endpoint unreachable, using cache\n";

    // 1) Running build matches server latest -> /offsets.json.
    // 2) Else pinned per-version JSON: /<runningVersion>/offsets.json.
    // 3) Else newest offsets.json anyway (user pins Roblox to it).
    // 4) Else local cache.
    std::string body;
    if (!latest.empty() && (runningVersion.empty() || runningVersion == latest)) {
        body = httpGet(host, L"/offsets.json");
        if (!fill(body, out)) {
            log += "WARN: offsets.json fetch/parse failed\n";
            body.clear();
        } else {
            log += "offsets: live offsets.json (" + out.robloxVersion + ")\n";
        }
    }
    if (body.empty() && !runningVersion.empty()) {
        std::wstring p = L"/" +
            std::wstring(runningVersion.begin(), runningVersion.end()) +
            L"/offsets.json";
        std::string pinned = httpGet(host, p);
        if (!fill(pinned, out)) {
            log += "WARN: no pinned offsets.json for " + runningVersion + "\n";
        } else {
            body = pinned;
            log += "offsets: pinned " + out.robloxVersion + "\n";
        }
    }
    if (body.empty() && !latest.empty()) {
        body = httpGet(host, L"/offsets.json");
        if (!fill(body, out)) {
            log += "WARN: latest offsets.json unusable\n";
            body.clear();
        } else {
            log += "offsets: newest available (" + out.robloxVersion + ")\n";
            if (out.robloxVersion != runningVersion)
                log += "NOTE: pin Roblox via ExploitStrap to " + out.robloxVersion +
                       " (running " + runningVersion + ")\n";
        }
    }
    if (body.empty()) {
        body = readFile(cachePath);
        if (!fill(body, out)) {
            log += "FAIL: no usable offsets (offline + empty cache)\n";
            return false;
        }
        log += "offsets: local cache (" + out.robloxVersion + ")\n";
        if (!latest.empty() && out.robloxVersion != latest)
            log += "NOTE: cache is stale vs server; pin Roblox via ExploitStrap to " +
                   out.robloxVersion + "\n";
        return true;
    }
    // Fresh body parsed OK -> refresh cache.
    std::ofstream f(cachePath, std::ios::binary | std::ios::trunc);
    f << body;
    if (!latest.empty() && out.robloxVersion != latest && out.robloxVersion != runningVersion)
        log += "NOTE: server data is for " + out.robloxVersion + "\n";
    return true;
}
