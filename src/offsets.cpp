// OffsetManager implementation. WinHTTP, no third-party deps.
//
// The payload is JSON, but not a stable schema: theo adds/renames keys and the
// same key name also appears as a *number* inside another section
// (VisualEngine.FakeDataModel, DataModel.Workspace,
// UserInputService.WindowInputState). A text search for the first occurrence
// therefore returns the wrong member. This is a small recursive parser: every
// lookup is scoped to the object that owns the key, so a name collision can
// never silently produce 0 again.
#include "offsets.h"
#include <windows.h>
#include <winhttp.h>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- JSON model
struct JVal {
    enum T { NUL, NUM, STR, BOOL, OBJ, ARR };
    T t = NUL;
    uint64_t unum = 0;  // exact integer value for NUM (offsets are integers)
    double dnum = 0;    // numeric value as written (floats survive here)
    std::string str;
    std::vector<std::pair<std::string, JVal>> obj;
    std::vector<JVal> arr;

    const JVal* get(const char* k) const {
        for (const auto& kv : obj)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
    uint64_t u64() const { return t == NUM ? unum : 0; }
    int i32(int def = 0) const { return t == NUM ? (int)unum : def; }
};

struct JParser {
    const char* p;
    const char* e;

    void ws() {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }
    bool lit(const char* s) {
        size_t n = strlen(s);
        if ((size_t)(e - p) < n || memcmp(p, s, n) != 0) return false;
        p += n;
        return true;
    }
    bool str(std::string& out) {
        if (p >= e || *p != '"') return false;
        ++p;
        while (p < e && *p != '"') {
            char c = *p++;
            if (c == '\\' && p < e) {
                char esc = *p++;
                switch (esc) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': // \uXXXX: no key/version string we read is non-ASCII
                    if (e - p >= 4) p += 4;
                    out += '?';
                    break;
                default: out += esc; break;
                }
            } else {
                out += c;
            }
        }
        if (p >= e) return false;
        ++p; // closing quote
        return out.size() <= 4096;
    }
    bool number(JVal& v) {
        const char* s = p;
        if (p < e && (*p == '-' || *p == '+')) ++p;
        if (e - p > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
            char* end = nullptr;
            unsigned long long n = strtoull(p, &end, 16);
            if (end == p) return false;
            p = end;
            v.t = JVal::NUM;
            v.unum = (uint64_t)n;
            v.dnum = (double)n;
            return true;
        }
        while (p < e && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' ||
                         *p == 'E' || *p == '+' || *p == '-'))
            ++p;
        if (p == s) return false;
        std::string tok(s, p);
        v.t = JVal::NUM;
        v.unum = strtoull(tok.c_str(), nullptr, 10);
        v.dnum = strtod(tok.c_str(), nullptr);
        return true;
    }
    bool value(JVal& v, int depth) {
        if (depth > 32) return false;
        ws();
        if (p >= e) return false;
        char c = *p;
        if (c == '{') {
            ++p;
            v.t = JVal::OBJ;
            ws();
            if (p < e && *p == '}') { ++p; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (p >= e || *p != ':') return false;
                ++p;
                JVal child;
                if (!value(child, depth + 1)) return false;
                v.obj.emplace_back(k, std::move(child));
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++p;
            v.t = JVal::ARR;
            ws();
            if (p < e && *p == ']') { ++p; return true; }
            for (;;) {
                JVal child;
                if (!value(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = JVal::STR; return str(v.str); }
        if (lit("true")) { v.t = JVal::BOOL; v.unum = 1; return true; }
        if (lit("false")) { v.t = JVal::BOOL; v.unum = 0; return true; }
        if (lit("null")) { v.t = JVal::NUL; return true; }
        return number(v);
    }
};

bool parseJson(const std::string& body, JVal& root) {
    JParser ps{ body.c_str(), body.c_str() + body.size() };
    if (!ps.value(root, 0)) return false;
    return root.t == JVal::OBJ;
}

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= (uint64_t)c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string hex64(uint64_t v) {
    char buf[24];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
    return buf;
}

// ------------------------------------------------------------ field helpers
uint64_t numIn(const JVal* sec, const char* key) {
    if (!sec) return 0;
    const JVal* v = sec->get(key);
    return v ? v->u64() : 0;
}
int intIn(const JVal* sec, const char* key, int def = 0) {
    if (!sec) return def;
    const JVal* v = sec->get(key);
    return v ? v->i32(def) : def;
}
std::string strIn(const JVal* sec, const char* key) {
    if (!sec) return "";
    const JVal* v = sec->get(key);
    return v && v->t == JVal::STR ? v->str : "";
}

std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// name -> member of Offsets. Used by both the offline dump and the fixture
// comparison so a new field only has to be added here once.
#define OFF_FIELDS(F) \
    F("RobloxVersion", o.robloxVersion.empty() ? 0 : 1) \
    F("TaskScheduler.Pointer", o.taskSchedulerPtr) \
    F("VisualEngine.Pointer", o.visualEnginePtr) \
    F("VisualEngine.ViewMatrix", o.visualEngineViewMatrix) \
    F("VisualEngine.FakeDataModel", o.visualEngineFakeDataModel) \
    F("FakeDataModel.Pointer", o.fakeDataModelPtr) \
    F("FakeDataModel.RealDataModel", o.fakeRealDataModel) \
    F("DataModel.Workspace", o.dataModelWorkspace) \
    F("DataModel.PlaceId", o.dataModelPlaceId) \
    F("DataModel.GameId", o.dataModelGameId) \
    F("DataModel.GameLoaded", o.dataModelGameLoaded) \
    F("DataModel.JobId", o.dataModelJobId) \
    F("DataModel.ServerIP", o.dataModelServerIP) \
    F("DataModel.PlaceVersion", o.dataModelPlaceVersion) \
    F("Workspace.CurrentCamera", o.workspaceCurrentCamera) \
    F("Instance.NameContainer", o.instanceNameContainer) \
    F("Instance.Name", o.instanceName) \
    F("Instance.ChildrenStart", o.instanceChildrenStart) \
    F("Instance.ChildrenEnd", o.instanceChildrenEnd) \
    F("Instance.Parent", o.instanceParent) \
    F("Instance.ClassDescriptor", o.instanceClassDescriptor) \
    F("Instance.ClassName", o.instanceClassName) \
    F("Player.LocalPlayer", o.playersLocalPlayer) \
    F("Player.ModelInstance", o.playerModelInstance) \
    F("Player.DisplayName", o.playerDisplayName) \
    F("Player.Team", o.playerTeam) \
    F("Humanoid.Health", o.humanoidHealth) \
    F("Humanoid.MaxHealth", o.humanoidMaxHealth) \
    F("Humanoid.Walkspeed", o.humanoidWalkSpeed) \
    F("Humanoid.WalkspeedCheck", o.humanoidWalkSpeedShadow) \
    F("Humanoid.JumpHeight", o.humanoidJumpHeight) \
    F("Humanoid.UseJumpPower", o.humanoidUseJumpPower) \
    F("Humanoid.HumanoidRootPart", o.humanoidHRP) \
    F("BasePart.Primitive", o.basePartPrimitive) \
    F("Primitive.Position", o.primitivePosition) \
    F("Primitive.Size", o.primitiveSize) \
    F("Primitive.Flags", o.primitiveFlags) \
    F("Primitive.Rotation", o.primitiveRotation) \
    F("Primitive.AssemblyAngularVelocity", o.primitiveAngularVelocity) \
    F("Humanoid.PlatformStand", o.humanoidPlatformStand) \
    F("Humanoid.Sit", o.humanoidSit) \
    F("Humanoid.EvaluateStateMachine", o.humanoidEvaluateStateMachine) \
    F("Humanoid.AutoRotate", o.humanoidAutoRotate) \
    F("Humanoid.HipHeight", o.humanoidHipHeight) \
    F("World.Gravity", o.worldGravity) \
    F("Camera.Position", o.cameraPosition) \
    F("Camera.CameraSubject", o.cameraSubject) \
    F("Camera.FieldOfView", o.cameraFOV) \
    F("Camera.Rotation", o.cameraRotation) \
    F("Camera.Viewport", o.cameraViewport) \
    F("Lighting.ClockTime", o.lightingClockTime) \
    F("Lighting.Brightness", o.lightingBrightness) \
    F("Lighting.FogEnd", o.lightingFogEnd) \
    F("Atmosphere.Density", o.atmosphereDensity) \
    F("Terrain.GrassLength", o.terrainGrassLength) \
    F("Terrain.WaterReflectance", o.terrainWaterReflectance) \
    F("StatsItem.Value", o.statsItemValue) \
    F("UserInputService.WindowInputState", o.userInputWS) \
    F("WindowInputState.CurrentTextBox", o.windowTextBox)
static bool offsetByName(const Offsets& o, const std::string& k, uint64_t& v) {
#define F(name, expr) if (k == name) { v = (uint64_t)(expr); return true; }
    OFF_FIELDS(F)
#undef F
    return false;
}

// Stable fingerprint over the consumed fields only (see fill()).
static uint64_t fieldHash(const Offsets& o) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : o.robloxVersion) { h ^= (uint64_t)c; h *= 1099511628211ULL; }
#define F(name, expr)                                                        \
    {                                                                        \
        uint64_t v_ = (uint64_t)(expr);                                       \
        for (int i_ = 0; i_ < 8; ++i_) {                                      \
            h ^= (v_ >> (i_ * 8)) & 0xff;                                     \
            h *= 1099511628211ULL;                                            \
        }                                                                     \
    }
    OFF_FIELDS(F)
#undef F
    return h;
}


// ---------------------------------------------------------------------- fill
// Parse a dump body. A body that is not the expected object shape is rejected
// (returns false) instead of producing a zeroed Offsets.
static bool fill(const std::string& body, Offsets& o, std::string& why) {
    JVal root;
    if (!parseJson(body, root)) {
        why = "JSON parse failed (truncated or not JSON)";
        return false;
    }
    const JVal* off = root.get("Offsets");
    if (!off || off->t != JVal::OBJ) {
        why = "no top-level \"Offsets\" object";
        return false;
    }
    o = Offsets{}; // every default, so a missing key is a known fallback
    o.robloxVersion = strIn(&root, "Roblox Version");
    if (o.robloxVersion.empty()) {
        why = "no \"Roblox Version\" string";
        return false;
    }

    const JVal* ts = off->get("TaskScheduler");
    const JVal* ve = off->get("VisualEngine");
    const JVal* fm = off->get("FakeDataModel");
    const JVal* dm = off->get("DataModel");
    const JVal* ws = off->get("Workspace");
    const JVal* pl = off->get("Player");
    const JVal* hu = off->get("Humanoid");
    const JVal* in = off->get("Instance");
    const JVal* cm = off->get("Camera");
    const JVal* bp = off->get("BasePart");
    const JVal* pr = off->get("Primitive");
    const JVal* pf = off->get("PrimitiveFlags");
    const JVal* uis = off->get("UserInputService");
    const JVal* wis = off->get("WindowInputState");

    o.taskSchedulerPtr      = numIn(ts, "Pointer");
    o.dataModelWorkspace    = intIn(dm, "Workspace");
    o.workspaceCurrentCamera = intIn(ws, "CurrentCamera");
    o.playerLocalPlayer     = intIn(pl, "LocalPlayer");
    o.humanoidHealth        = intIn(hu, "Health");
    o.humanoidMaxHealth     = intIn(hu, "MaxHealth");
    o.humanoidWalkSpeed     = intIn(hu, "Walkspeed");
    o.humanoidWalkSpeedShadow = intIn(hu, "WalkspeedCheck");
    if (!o.humanoidWalkSpeedShadow) o.humanoidWalkSpeedShadow = 0x3A4; // legacy 932
    o.humanoidJumpPower     = intIn(hu, "JumpPower");
    o.humanoidJumpHeight    = intIn(hu, "JumpHeight");
    if (!o.humanoidJumpHeight) o.humanoidJumpHeight = 0x19C; // legacy 412
    o.humanoidUseJumpPower  = intIn(hu, "UseJumpPower");
    if (!o.humanoidUseJumpPower) o.humanoidUseJumpPower = 0x1DD; // legacy 477
    o.humanoidHRP           = intIn(hu, "HumanoidRootPart");
    o.visualEngineViewMatrix = intIn(ve, "ViewMatrix");
    o.visualEngineDimensions = intIn(ve, "Dimensions");
    o.visualEngineFakeDataModel = intIn(ve, "FakeDataModel");
    o.visualEnginePtr       = numIn(ve, "Pointer");
    o.instanceChildrenStart = intIn(in, "ChildrenStart");
    o.instanceParent        = intIn(in, "Parent");
    o.instanceChildrenEnd   = intIn(in, "ChildrenEnd", 8);
    if (!o.instanceChildrenEnd) o.instanceChildrenEnd = 8;
    o.instanceName          = intIn(in, "Name");
    o.instanceNameContainer = intIn(in, "NameContainer");
    o.instanceClassDescriptor = intIn(in, "ClassDescriptor", 24);
    o.instanceClassName     = intIn(in, "ClassName", 8);
    o.fakeDataModelPtr      = numIn(fm, "Pointer");
    o.fakeRealDataModel     = intIn(fm, "RealDataModel");
    o.dataModelPlaceId      = intIn(dm, "PlaceId");
    o.dataModelGameId       = intIn(dm, "GameId");
    o.dataModelGameLoaded   = intIn(dm, "GameLoaded");
    o.dataModelJobId        = intIn(dm, "JobId");
    o.dataModelServerIP     = intIn(dm, "ServerIP");
    o.dataModelPlaceVersion = intIn(dm, "PlaceVersion");
    o.statsItemValue        = intIn(off->get("StatsItem"), "Value");
    o.cameraPosition        = intIn(cm, "Position");
    o.cameraSubject         = intIn(cm, "CameraSubject");
    o.cameraFOV             = intIn(cm, "FieldOfView");
    o.cameraRotation        = intIn(cm, "Rotation");
    o.cameraViewport        = intIn(cm, "Viewport");
    o.playersLocalPlayer    = intIn(pl, "LocalPlayer");
    o.playerModelInstance   = intIn(pl, "ModelInstance");
    o.playerUserId          = intIn(pl, "UserId");
    o.playerDisplayName     = intIn(pl, "DisplayName");
    o.playerTeam            = intIn(pl, "Team");
    o.playerMinZoomDistance = intIn(pl, "MinZoomDistance");
    o.playerMaxZoomDistance = intIn(pl, "MaxZoomDistance");
    o.basePartPrimitive     = intIn(bp, "Primitive");
    o.primitivePosition     = intIn(pr, "Position");
    o.primitiveVelocity     = intIn(pr, "AssemblyLinearVelocity");
    o.primitiveSize         = intIn(pr, "Size");
    o.primitiveFlags        = intIn(pr, "Flags");
    o.primitiveRotation     = intIn(pr, "Rotation");
    o.primitiveAngularVelocity = intIn(pr, "AssemblyAngularVelocity");
    o.humanoidPlatformStand = intIn(hu, "PlatformStand");
    o.humanoidSit           = intIn(hu, "Sit");
    o.humanoidEvaluateStateMachine = intIn(hu, "EvaluateStateMachine");
    o.humanoidAutoRotate    = intIn(hu, "AutoRotate");
    o.humanoidHipHeight     = intIn(hu, "HipHeight");
    o.worldGravity          = intIn(off->get("World"), "Gravity");
    { int cc = intIn(pf, "CanCollide"); if (cc) o.primitiveCanCollideBit = cc;
      int an = intIn(pf, "Anchored"); if (an) o.primitiveAnchoredBit = an; }
    o.userInputWS           = intIn(uis, "WindowInputState");
    o.windowTextBox         = intIn(wis, "CurrentTextBox");
    // ---- World tab ----
    const JVal* li = off->get("Lighting");
    const JVal* atm = off->get("Atmosphere");
    const JVal* bfx = off->get("BloomEffect");
    const JVal* ter = off->get("Terrain");
    o.lightingClockTime            = intIn(li, "ClockTime");
    o.lightingBrightness           = intIn(li, "Brightness");
    o.lightingAmbient              = intIn(li, "Ambient");
    o.lightingOutdoorAmbient       = intIn(li, "OutdoorAmbient");
    o.lightingExposureCompensation = intIn(li, "ExposureCompensation");
    o.lightingFogStart             = intIn(li, "FogStart");
    o.lightingFogEnd               = intIn(li, "FogEnd");
    o.lightingGlobalShadows        = intIn(li, "GlobalShadows");
    o.atmosphereDensity            = intIn(atm, "Density");
    o.atmosphereHaze               = intIn(atm, "Haze");
    o.atmosphereGlare              = intIn(atm, "Glare");
    o.effectEnabled                = intIn(bfx, "Enabled");
    o.terrainGrassLength           = intIn(ter, "GrassLength");
    o.terrainWaterReflectance      = intIn(ter, "WaterReflectance");
    o.terrainWaterTransparency     = intIn(ter, "WaterTransparency");
    o.terrainWaterWaveSize         = intIn(ter, "WaterWaveSize");
    // Fingerprint of the fields the tool actually consumes, so a genuine layout
    // change is distinguishable from theo's noise (Humanoid.PlatformStatePointer
    // is a live pointer and changes on every request).
    o.srcHash = fieldHash(o);
    return true;
}

// ------------------------------------------------------------------ validate
// Required keys: a 0 / out-of-range / misaligned value here means the dump
// shape changed (renamed key, moved section, or an outright dumper bug).
// Optional keys only produce a note: the feature that needs them self-disables.
bool validateOffsets(const Offsets& o, std::string& why) {
    struct Chk { const char* name; uint64_t v; uint64_t lo, hi; uint64_t align; };
    const Chk req[] = {
        { "TaskScheduler.Pointer",      o.taskSchedulerPtr,      0x10000, 0x40000000, 8 },
        { "VisualEngine.Pointer",       o.visualEnginePtr,       0x10000, 0x40000000, 8 },
        { "FakeDataModel.Pointer",     o.fakeDataModelPtr,       0x10000, 0x40000000, 8 },
        { "FakeDataModel.RealDataModel", (uint64_t)o.fakeRealDataModel, 0x20, 0x4000, 4 },
        { "DataModel.Workspace",        (uint64_t)o.dataModelWorkspace, 4, 0x4000, 4 },
        { "DataModel.PlaceId",          (uint64_t)o.dataModelPlaceId, 4, 0x4000, 4 },
        { "Workspace.CurrentCamera",    (uint64_t)o.workspaceCurrentCamera, 4, 0x4000, 4 },
        { "Instance.NameContainer",     (uint64_t)o.instanceNameContainer, 4, 0x4000, 4 },
        { "Instance.Name",              (uint64_t)o.instanceName, 0, 0x4000, 1 },
        { "Instance.ChildrenStart",     (uint64_t)o.instanceChildrenStart, 4, 0x4000, 4 },
        { "Instance.Parent",            (uint64_t)o.instanceParent, 4, 0x4000, 4 },
        { "Player.ModelInstance",       (uint64_t)o.playerModelInstance, 4, 0x8000, 4 },
        { "Player.LocalPlayer",         (uint64_t)o.playersLocalPlayer, 4, 0x8000, 4 },
        { "BasePart.Primitive",         (uint64_t)o.basePartPrimitive, 4, 0x4000, 4 },
        { "Primitive.Position",         (uint64_t)o.primitivePosition, 4, 0x4000, 4 },
        { "Humanoid.Health",            (uint64_t)o.humanoidHealth, 4, 0x4000, 4 },
        { "Humanoid.Walkspeed",         (uint64_t)o.humanoidWalkSpeed, 4, 0x4000, 4 },
    };
    std::string bad;
    for (const Chk& c : req) {
        if (c.v < c.lo || c.v >= c.hi || (c.align > 1 && (c.v % c.align) != 0)) {
            if (!bad.empty()) bad += ", ";
            bad += std::string(c.name) + "=" + std::to_string((unsigned long long)c.v);
        }
    }
    if (!bad.empty()) {
        why = "offsets rejected (" + bad + ") - dump shape changed or key renamed";
        return false;
    }
    // Non-fatal: optional/derived keys that are 0 or implausible. These disable
    // one widget each, never the whole overlay - but they must be visible.
    std::string notes;
    auto opt = [&](const char* name, uint64_t v, uint64_t hi, bool align4) {
        if (!v || v >= hi || (align4 && (v % 4) != 0)) {
            if (!notes.empty()) notes += ", ";
            notes += std::string(name) + "=" + std::to_string((unsigned long long)v);
        }
    };
    opt("StatsItem.Value", (uint64_t)o.statsItemValue, 0x20000, true);
    opt("VisualEngine.ViewMatrix", (uint64_t)o.visualEngineViewMatrix, 0x4000, true);
    opt("Camera.Position", (uint64_t)o.cameraPosition, 0x4000, true);
    opt("Camera.Rotation", (uint64_t)o.cameraRotation, 0x4000, true);
    opt("Camera.CameraSubject", (uint64_t)o.cameraSubject, 0x4000, true);
    opt("Lighting.ClockTime", (uint64_t)o.lightingClockTime, 0x4000, true);
    opt("Terrain.GrassLength", (uint64_t)o.terrainGrassLength, 0x2000, true);
    opt("DataModel.JobId", (uint64_t)o.dataModelJobId, 0x4000, true);
    opt("DataModel.ServerIP", (uint64_t)o.dataModelServerIP, 0x4000, true);
    if (!notes.empty()) why = "optional keys unusable (feature shows n/a): " + notes;
    return true;
}

// --------------------------------------------------------- offline self-test
int runOffDump(const std::string& jsonPath) {
    std::string body = readFile(jsonPath);
    if (body.empty()) {
        printf("offdump: cannot read %s\n", jsonPath.c_str());
        return 1;
    }
    Offsets o;
    std::string why;
    if (!fill(body, o, why)) {
        printf("offdump: %s\n", why.c_str());
        return 1;
    }
    std::string note;
    bool ok = validateOffsets(o, note);
    printf("# %s version=%s hash=%016llx validate=%s\n", jsonPath.c_str(),
           o.robloxVersion.c_str(), (unsigned long long)o.srcHash, ok ? "OK" : "FAIL");
    if (!note.empty()) printf("# %s\n", note.c_str());
#define F(name, expr) printf("%s=%llu\n", name, (unsigned long long)(expr));
    OFF_FIELDS(F)
#undef F
    return ok ? 0 : 2;
}

namespace {
std::string stripName(const std::string& path) {
    size_t a = path.find_last_of("\\/");
    std::string f = a == std::string::npos ? path : path.substr(a + 1);
    size_t d = f.find_last_of('.');
    return d == std::string::npos ? f : f.substr(0, d);
}
std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char c = dir.back();
    return (c == '\\' || c == '/') ? dir + name : dir + "\\" + name;
}
} // namespace

int runOffTest(const std::string& dir, std::string& log) {
    std::string pattern = joinPath(dir, "*.json");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        log += "offtest: no fixtures in " + dir + "\n";
        return 1;
    }
    int files = 0, fails = 0, compared = 0, keys = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string path = joinPath(dir, fd.cFileName);
        std::string body = readFile(path);
        Offsets o;
        std::string why;
        ++files;
        if (!fill(body, o, why)) {
            log += std::string("FAIL ") + fd.cFileName + ": " + why + "\n";
            ++fails;
            continue;
        }
        std::string note;
        bool ok = validateOffsets(o, note);
        log += std::string(ok ? "ok   " : "FAIL ") + fd.cFileName + " version=" +
               o.robloxVersion + (note.empty() ? "" : ("  [" + note + "]")) + "\n";
        if (!ok) ++fails;
        std::string exp = joinPath(dir, stripName(path) + ".expect");
        std::string body2 = readFile(exp);
        if (body2.empty()) continue;
        ++compared;
        std::istringstream ss(body2);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            uint64_t want = strtoull(line.c_str() + eq + 1, nullptr, 10);
            uint64_t got = 0;
            if (!offsetByName(o, k, got)) {
                log += "FAIL " + k + ": unknown field\n";
                ++fails;
            } else if (got != want) {
                log += "FAIL " + k + ": want " + std::to_string((unsigned long long)want) +
                       " got " + std::to_string((unsigned long long)got) + "\n";
                ++fails;
            } else {
                ++keys;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    log += "offtest: files=" + std::to_string(files) + " compared=" +
           std::to_string(compared) + " keys=" + std::to_string(keys) +
           " fails=" + std::to_string(fails) + "\n";
    return fails ? 1 : 0;
}

// --------------------------------------------------------------- HTTP + flow
static std::string httpGet(const std::wstring& host, const std::wstring& path) {
    return httpGetEx(host, path, 0, L"ext-p0/1.0", nullptr);
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

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

static bool writeFile(const std::string& p, const std::string& body) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << body;
    f.flush();
    return (bool)f;
}

bool OffsetManager::ensure(const std::string& runningVersion,
                           const std::string& cachePath,
                           Offsets& out,
                           std::string& log,
                           const std::string& goodPath) {
    const std::wstring host = L"offsets.imtheo.lol";
    std::string latest = trim(httpGet(host, L"/roblox/version"));
    if (!latest.empty()) log += "server roblox version: " + latest + "\n";
    else log += "WARN: version endpoint unreachable, using cache\n";

    // Every candidate goes through parse + validate before it is accepted, and
    // the reason for a rejection is logged: that "why" is the whole point - a
    // silent 0 offset costs more time than a loud failed start.
    std::string accepted; // body of the candidate that passed, for cache refresh
    auto accept = [&](const std::string& body, const char* source) -> bool {
        if (body.empty()) return false;
        Offsets tmp;
        std::string why;
        if (!fill(body, tmp, why)) {
            log += std::string("WARN: ") + source + " rejected: " + why + "\n";
            return false;
        }
        if (!validateOffsets(tmp, why)) {
            log += std::string("WARN: ") + source + " rejected: " + why + "\n";
            return false;
        }
        tmp.source = source;
        out = tmp;
        if (!why.empty()) log += "WARN: " + why + "\n";
        log += std::string("offsets: ") + source + " (" + out.robloxVersion + ") hash=" +
               hex64(out.srcHash) + "\n";
        accepted = body;
        return true;
    };

    bool ok = false;
    // 1) Running build matches server latest -> /offsets.json.
    if (!latest.empty() && (runningVersion.empty() || runningVersion == latest))
        ok = accept(httpGet(host, L"/offsets.json"), "live");
    // 2) Pinned per-version JSON.
    if (!ok && !runningVersion.empty()) {
        std::wstring p = L"/" +
            std::wstring(runningVersion.begin(), runningVersion.end()) +
            L"/offsets.json";
        ok = accept(httpGet(host, p), "pinned");
    }
    // 3) Newest offsets.json anyway (user pins Roblox to it).
    if (!ok && !latest.empty()) {
        ok = accept(httpGet(host, L"/offsets.json"), "newest");
        if (ok && out.robloxVersion != runningVersion)
            log += "NOTE: data is for " + out.robloxVersion + " (running " +
                   runningVersion + ") - write features stay off\n";
    }
    // 4) Last known good (validated in an earlier session).
    if (!ok && !goodPath.empty()) ok = accept(readFile(goodPath), "good");
    // 5) Local cache.
    if (!ok) ok = accept(readFile(cachePath), "cache");
    if (!ok) {
        log += "FAIL: no usable offsets (all sources rejected)\n";
        return false;
    }
    // Refresh both files from the body we actually accepted, so the next
    // offline start has something that is known to parse.
    if (!writeFile(cachePath, accepted))
        log += "WARN: cannot refresh cache: " + cachePath + "\n";
    if (!goodPath.empty() && !writeFile(goodPath, accepted))
        log += "WARN: cannot write good-offsets file: " + goodPath + "\n";
    return true;
}
