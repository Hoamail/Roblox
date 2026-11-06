// Config/profile/waypoint IO moved verbatim from overlay.cpp (S2.2).
#include "ovl_cfg.h"
#include "ovl_state.h"
#include "ovl_ui.h"
#include "ovl_server.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---- exe-relative side files ------------------------------------------------
// overlay.cfg, profiles\, logs and the offsets cache live next to the exe, NOT
// in the process CWD. The CWD is not ours to trust: "Run as administrator" from
// Explorer sets it to C:\Windows\System32, which silently made those runs load
// default settings and drop logs into an unwritable dir (observed 2026-10-19).
std::string g_exeDir;
void initExeDir() {
    char path[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p(path, n ? n : 0);
    size_t slash = p.find_last_of("\\/");
    g_exeDir = (slash == std::string::npos) ? "." : p.substr(0, slash);
}
std::string exePath(const char* name) { return g_exeDir + "\\" + name; }

int loadToggleKey() {
    FILE* f = fopen(exePath("overlay.cfg").c_str(), "r");
    if (!f) {
        f = fopen(exePath("overlay.cfg").c_str(), "w");
        if (f) {
            fputs("# toggle key for overlay hide/show.\n"
                  "# F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN,\n"
                  "# SPACE, letters/digits (e.g. G), \\ , or hex (e.g. 0x47)\n"
                  "toggle=G\n"
                  "aim.enabled=1\n"
                  "aim.key=RMB\n"
                  "aim.keymode=0\n"
                  "aim.fov=250\n"
                  "# aim.smoothing: velext model, tau = 8ms*smoothing (1..20; <=1 snaps)\n"
                  "aim.smoothing=5\n"
                  "aim.mode=0\n"
                  "aim.sticky=1\n"
                  "aim.sticktime=4\n"
                  "aim.predict=1\n"
                  "aim.pscale=1.02\n"
                  "aim.dynk=0.42\n"
                  "aim.nearest=1\n"
                  "aim.overextend=0\n"
                  "aim.height=1.5\n"
                  "aim.maxdist=2000\n"
                  "aim.deadzone=1\n"
                  "aim.ignoreteam=0\n"
                  "aim.ignoredead=1\n"
                  "aim.interval=0\n"
                  "# aim.bone: 0 Head, 1 HRP+height, 2/3 arms, 4/5 legs\n"
                  "aim.bone=0\n"
                  "aim.npcs=1\n"
                  "aim.sens=1.0\n"
                  "aim.gamesens=1.0\n"
                  "# aim.pid: 0 = velext filter (default), 1 = legacy PID/1-pole (debug)\n"
                  "aim.pid=0\n"
                  "aim.rate=500\n"
                  "aim.park=0\n"
                  "trig.enabled=0\n"
                  "trig.key=X\n"
                  "trig.keymode=1\n"
                  "trig.delay=45\n"
                  "trig.cooldown=90\n"
                  "trig.press=35\n"
                  "trig.pad=2\n"
                  "autoclick.enabled=0\n"
                  "autoclick.key=V\n"
                  "autoclick.keymode=1\n"
                  "autoclick.interval=100\n"
                  "autoclick.duration=2000\n"
                  "autoclick.press=35\n"
                  "input.chatguard=1\n"
                  "ply.mode=1\n"
                  "ply.speed=0\n"
                  "ply.speedval=32\n"
                  "ply.jump=0\n"
                  "ply.jumpval=100\n"
                  "ply.speedkey=B\n"
                  "ply.jumpkey=N\n"
                  "ply.sdnkey=0xDB\n"
                  "ply.supkey=0xDD\n"
                  "ply.jdnkey=0xBA\n"
                  "ply.jupkey=0xDE\n"
                  "fly.enabled=0\n"
                  "fly.key=F\n"
                  "fly.speed=60\n"
                  "# Stunt tab (index 8): visible root choreography. stunt.key=0 means\n"
                  "# the tab's Run button is hold-to-run (no hotkey to collide with).\n"
                  "# stunt.mode: 0 = dynamic (solver integrates), 1 = kinematic (we\n"
                  "# write Primitive.Rotation ourselves) - A/B switch for E1/E2.\n"
                  "stunt.enabled=0\n"
                  "stunt.preset=0\n"
                  "stunt.speed=1.0\n"
                  "stunt.amp=1.0\n"
                  "stunt.ragdoll=0\n"
                  "stunt.loop=1\n"
                  "stunt.hover=0\n"
                  "stunt.key=0\n"
                  "stunt.mode=0\n"
                  "# teleport is the menu's Teleport tab (players + TP buttons)\n"
                  "tp.npc=0\n"
                  "# guided-dash speed used by the TP buttons (studs/s)\n"
                  "tp.speed=300\n"
                  "# clickable menu panel: the title-bar triangle collapses it,\n"
                  "# pos/size are written back when you drag/resize it\n"
                  "menu.collapsed=0\n"
                  "menu.tab=0\n"
                  "menu.unlockmouse=1\n"
                  "menu.pos=16,360\n"
                  "menu.size=340,420\n"
                  "# World tab (Đợt 2): client-local lighting/atmosphere/terrain\n"
                  "world.full=0\n"
                  "world.bright=2\n"
                  "world.fog=0\n"
                  "world.shadows=0\n"
                  "world.expo=0\n"
                  "world.expoval=0\n"
                  "world.clock=0\n"
                  "world.clockval=14\n"
                  "world.fx=0\n"
                  "world.atmo=0\n"
                  "world.water=0\n"
                  "world.grass=0\n"
                  "world.fovon=0\n"
                  "world.fov=90\n"
                  "world.zoom=0\n", f);
            fclose(f);
        }
        return 'G';
    }
    char line[128];
    int vk = 'G';
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "toggle", 6) == 0) {
            const char* eq = strchr(line, '=');
            if (eq) {
                int v = parseKey(eq + 1);
                if (v || _stricmp(eq + 1, "NONE\n") == 0 ||
                    _stricmp(eq + 1, "NONE\r\n") == 0)
                    vk = v;
            }
        }
    }
    fclose(f);
    return vk;
}

bool upsertCfgKey(const char* key, const char* value) {
    FILE* f = fopen(exePath("overlay.cfg").c_str(), "r");
    std::string out;
    bool found = false;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(key);
            if (strncmp(line, key, n) == 0 && line[n] == '=') {
                out += key;
                out += '=';
                out += value;
                out += '\n';
                found = true;
            } else {
                out += line;
            }
        }
        fclose(f);
    }
    if (!found) {
        out += key;
        out += '=';
        out += value;
        out += '\n';
    }
    f = fopen(exePath("overlay.cfg").c_str(), "w");
    if (!f) return false;
    fputs(out.c_str(), f);
    fclose(f);
    return true;
}

// ---- Server tab: last joined job/place (moved from overlay.cpp) ----
std::string cfgSrvLastJob;       // srv.lastjob    last job we joined
uint64_t cfgSrvLastPlace = 0;    // srv.lastplace  its placeId

static int cfgInt(const char* line, const char* key, int dflt) {
    size_t n = strlen(key);
    if (strncmp(line, key, n) == 0 && line[n] == '=') return atoi(line + n + 1);
    return dflt;
}

static float cfgFloat(const char* line, const char* key, float dflt) {
    size_t n = strlen(key);
    if (strncmp(line, key, n) == 0 && line[n] == '=') return (float)atof(line + n + 1);
    return dflt;
}

static bool isNoneKey(const char* raw) {
    while (*raw == ' ' || *raw == '\t') ++raw;
    return (_strnicmp(raw, "NONE", 4) == 0 &&
            (raw[4] == '\0' || raw[4] == '\r' || raw[4] == '\n' ||
             raw[4] == ' ' || raw[4] == '\t'));
}

void loadAimCfg() {
    FILE* f = fopen(exePath("overlay.cfg").c_str(), "r");
    if (!f) return;
    char line[128];
    bool legacySmooth = false; // old smoothing keys present -> one log line
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "aim.enabled=", 12) == 0)
            cfgAimEnabled = atoi(line + 12) ? true : false;
        if (strncmp(line, "aim.key=", 8) == 0) {
            int v = parseKey(line + 8);
            if (v || isNoneKey(line + 8)) cfgAimKey = v;
        }
        cfgAimKeyMode = cfgInt(line, "aim.keymode", cfgAimKeyMode);
        cfgAimFov = cfgInt(line, "aim.fov", cfgAimFov);
        cfgAimSmooth = cfgInt(line, "aim.smooth", cfgAimSmooth);
        cfgAimMode = cfgInt(line, "aim.mode", cfgAimMode);
        cfgAimSticky = cfgInt(line, "aim.sticky", cfgAimSticky);
        cfgAimStickTime = cfgFloat(line, "aim.sticktime", cfgAimStickTime);
        cfgAimPredict = cfgInt(line, "aim.predict", cfgAimPredict);
        cfgAimPScale = cfgFloat(line, "aim.pscale", cfgAimPScale);
        cfgAimDynK = cfgFloat(line, "aim.dynk", cfgAimDynK);
        cfgAimNearest = cfgInt(line, "aim.nearest", cfgAimNearest);
        cfgAimOverextend = cfgFloat(line, "aim.overextend", cfgAimOverextend);
        cfgAimHeight = cfgFloat(line, "aim.height", cfgAimHeight);
        cfgAimMaxDist = cfgFloat(line, "aim.maxdist", cfgAimMaxDist);
        cfgAimDeadzone = cfgFloat(line, "aim.deadzone", cfgAimDeadzone);
        cfgAimIgnoreTeam = cfgInt(line, "aim.ignoreteam", cfgAimIgnoreTeam);
        cfgAimInterval = cfgInt(line, "aim.interval", cfgAimInterval);
        cfgAimBone = cfgInt(line, "aim.bone", cfgAimBone);
        cfgAimNPCs = cfgInt(line, "aim.npcs", cfgAimNPCs);
        cfgAimTSmooth = cfgFloat(line, "aim.tsmooth", cfgAimTSmooth);
        cfgAimSens = cfgFloat(line, "aim.sens", cfgAimSens);
        cfgAimGameSens = cfgFloat(line, "aim.gamesens", cfgAimGameSens);
        cfgAimPid = cfgInt(line, "aim.pid", cfgAimPid);
        cfgAimMemSmooth = cfgFloat(line, "aim.memsmooth", cfgAimMemSmooth);
        cfgAimRate = cfgInt(line, "aim.rate", cfgAimRate);
        cfgAimSmoothing = cfgFloat(line, "aim.smoothing", cfgAimSmoothing);
        // "aim.smooth=" is length 11 and does not collide with "aim.smoothing="
        if (!legacySmooth &&
            (strncmp(line, "aim.tsmooth=", 12) == 0 ||
             strncmp(line, "aim.memsmooth=", 14) == 0 ||
             strncmp(line, "aim.smooth=", 11) == 0))
            legacySmooth = true;
        if (strncmp(line, "trig.enabled=", 13) == 0)
            cfgTrigEnabled = atoi(line + 13) ? 1 : 0;
        if (strncmp(line, "trig.key=", 9) == 0) {
            int v = parseKey(line + 9);
            if (v || isNoneKey(line + 9)) cfgTrigKey = v;
        }
        cfgTrigKeyMode = cfgInt(line, "trig.keymode", cfgTrigKeyMode);
        cfgTrigDelay = cfgInt(line, "trig.delay", cfgTrigDelay);
        cfgTrigCooldown = cfgInt(line, "trig.cooldown", cfgTrigCooldown);
        cfgTrigPress = cfgInt(line, "trig.press", cfgTrigPress);
        cfgTrigPad = cfgInt(line, "trig.pad", cfgTrigPad);
        if (strncmp(line, "autoclick.key=", 14) == 0) {
            int v = parseKey(line + 14);
            if (v || isNoneKey(line + 14)) cfgACKey = v;
        }
        if (strncmp(line, "autoclick.enabled=", 18) == 0)
            cfgACEnabled = atoi(line + 18) ? 1 : 0;
        cfgACKeyMode = cfgInt(line, "autoclick.keymode", cfgACKeyMode);
        cfgACInterval = cfgInt(line, "autoclick.interval", cfgACInterval);
        cfgACDuration = cfgInt(line, "autoclick.duration", cfgACDuration);
        cfgACPress = cfgInt(line, "autoclick.press", cfgACPress);
        if (strncmp(line, "input.chatguard=", 16) == 0)
            cfgChatGuard = atoi(line + 16) ? 1 : 0;
        if (strncmp(line, "ply.speed=", 10) == 0)
            cfgPlySpeed = atoi(line + 10) ? 1 : 0;
        cfgPlySpeedVal = cfgFloat(line, "ply.speedval", cfgPlySpeedVal);
        cfgPlyMode = cfgInt(line, "ply.mode", cfgPlyMode);
        if (strncmp(line, "ply.jump=", 9) == 0)
            cfgPlyJump = atoi(line + 9) ? 1 : 0;
        cfgPlyJumpVal = cfgFloat(line, "ply.jumpval", cfgPlyJumpVal);
        cfgPlyJumpMode = cfgInt(line, "ply.jumpmode", cfgPlyJumpMode);
        if (strncmp(line, "ply.speedkey=", 13) == 0) {
            int v = parseKey(line + 13);
            if (v || isNoneKey(line + 13)) cfgPlySpeedKey = v;
        }
        if (strncmp(line, "ply.jumpkey=", 12) == 0) {
            int v = parseKey(line + 12);
            if (v || isNoneKey(line + 12)) cfgPlyJumpKey = v;
        }
        if (strncmp(line, "ply.sdnkey=", 11) == 0) {
            int v = parseKey(line + 11);
            if (v || isNoneKey(line + 11)) cfgPlySdnKey = v;
        }
        if (strncmp(line, "ply.supkey=", 11) == 0) {
            int v = parseKey(line + 11);
            if (v || isNoneKey(line + 11)) cfgPlySupKey = v;
        }
        if (strncmp(line, "ply.jdnkey=", 11) == 0) {
            int v = parseKey(line + 11);
            if (v || isNoneKey(line + 11)) cfgPlyJdnKey = v;
        }
        if (strncmp(line, "ply.jupkey=", 11) == 0) {
            int v = parseKey(line + 11);
            if (v || isNoneKey(line + 11)) cfgPlyJupKey = v;
        }
        if (strncmp(line, "fly.enabled=", 12) == 0)
            cfgFlyEnabled = atoi(line + 12) ? 1 : 0;
        if (strncmp(line, "fly.key=", 8) == 0) {
            // written by the menu's "hotkey" checkbox: NONE disables it on purpose
            cfgFlyKey = parseKey(line + 8);
        }
        cfgFlySpeed = cfgFloat(line, "fly.speed", cfgFlySpeed);
        // ---- Stunt tab (index 8) ----
        if (strncmp(line, "stunt.enabled=", 14) == 0)
            cfgStuntEnabled = atoi(line + 14) ? 1 : 0;
        cfgStuntPreset = cfgInt(line, "stunt.preset", cfgStuntPreset);
        cfgStuntSpeed = cfgFloat(line, "stunt.speed", cfgStuntSpeed);
        cfgStuntAmp = cfgFloat(line, "stunt.amp", cfgStuntAmp);
        cfgStuntRagdoll = cfgInt(line, "stunt.ragdoll", cfgStuntRagdoll);
        cfgStuntLoop = cfgInt(line, "stunt.loop", cfgStuntLoop);
        cfgStuntHover = cfgInt(line, "stunt.hover", cfgStuntHover);
        cfgStuntMode = cfgInt(line, "stunt.mode", cfgStuntMode);
        if (strncmp(line, "stunt.key=", 10) == 0) {
            int v = parseKey(line + 10);
            if (v || isNoneKey(line + 10)) cfgStuntKey = v;
        }
        // menu.show / menu.key (INSERT) are legacy since 2026-10-20: the
        // title-bar collapse triangle replaced them. Old cfg lines are ignored.
        if (strncmp(line, "menu.collapsed=", 15) == 0)
            cfgMenuCollapsed = atoi(line + 15) ? 1 : 0;
        if (strncmp(line, "menu.pos=", 9) == 0)
            sscanf(line + 9, "%f,%f", &cfgMenuX, &cfgMenuY);
        if (strncmp(line, "menu.size=", 10) == 0) {
            if (sscanf(line + 10, "%f,%f", &g_menuW, &g_menuH) == 2) {
                g_savedMenuW = g_menuW;
                g_savedMenuH = g_menuH;
            }
        }
        if (strncmp(line, "menu.tab=", 9) == 0) {
            int v = atoi(line + 9);
            cfgMenuTab = (v >= 0 && v < 9) ? v : 0; // 0..7 tabs + 8 = Stunt
        }
        if (strncmp(line, "menu.unlockmouse=", 17) == 0)
            cfgMenuUnlockMouse = atoi(line + 17) ? 1 : 0;
        if (strncmp(line, "tp.npc=", 7) == 0)
            cfgTpNpc = atoi(line + 7) ? 1 : 0;
        if (strncmp(line, "tp.speed=", 9) == 0)
            cfgTpSpeed = atoi(line + 9);
        if (strncmp(line, "hud.on=", 7) == 0)
            cfgHudOn = atoi(line + 7) ? 1 : 0;
        if (strncmp(line, "hud.feats=", 10) == 0)
            cfgHudFeats = atoi(line + 10) ? 1 : 0;
        if (strncmp(line, "misc.wda=", 9) == 0)
            cfgWda = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "profile.auto=", 13) == 0)
            cfgAutoProfile = atoi(line + 13) ? 1 : 0;
        if (strncmp(line, "panic.key=", 10) == 0) {
            // parseKey returns 0 for "none": keep that meaning for panic
            cfgPanicKey = parseKey(line + 10);
        }
        if (strncmp(line, "esp.on=", 7) == 0)
            cfgEspOn = atoi(line + 7) ? 1 : 0;
        if (strncmp(line, "esp.box=", 8) == 0)
            cfgEspBox = atoi(line + 8) ? 1 : 0;
        if (strncmp(line, "esp.fill=", 9) == 0)
            cfgEspFill = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "esp.name=", 9) == 0)
            cfgEspName = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "esp.dist=", 9) == 0)
            cfgEspDist = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "esp.hp=", 7) == 0)
            cfgEspHp = atoi(line + 7) ? 1 : 0;
        if (strncmp(line, "esp.head=", 9) == 0)
            cfgEspHead = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "esp.line=", 9) == 0)
            cfgEspLine = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "esp.skel=", 9) == 0)
            cfgEspSkel = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "esp.oof=", 8) == 0)
            cfgEspOof = atoi(line + 8) ? 1 : 0;
        if (strncmp(line, "esp.npc=", 8) == 0)
            cfgEspNpc = atoi(line + 8) ? 1 : 0;
        if (strncmp(line, "esp.maxdist=", 12) == 0)
            cfgEspMaxDist = atoi(line + 12);
        if (strncmp(line, "esp.colenemy=", 13) == 0)
            cfgEspColEnemy = atoi(line + 13) & 0xFFFFFF;
        if (strncmp(line, "esp.colteam=", 12) == 0)
            cfgEspColTeam = atoi(line + 12) & 0xFFFFFF;
        if (strncmp(line, "esp.colnpc=", 11) == 0)
            cfgEspColNpc = atoi(line + 11) & 0xFFFFFF;
        if (strncmp(line, "radar.on=", 9) == 0)
            cfgRadarOn = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "radar.range=", 12) == 0)
            cfgRadarRange = atoi(line + 12);
        if (strncmp(line, "radar.size=", 11) == 0)
            cfgRadarSize = atoi(line + 11);
        // ---- World tab (Đợt 2) ----
        if (strncmp(line, "world.full=", 11) == 0)
            cfgWorldFull = atoi(line + 11) ? 1 : 0;
        cfgWorldBright = cfgFloat(line, "world.bright", cfgWorldBright);
        if (strncmp(line, "world.fog=", 10) == 0)
            cfgWorldFog = atoi(line + 10) ? 1 : 0;
        if (strncmp(line, "world.shadows=", 14) == 0)
            cfgWorldShadows = atoi(line + 14) ? 1 : 0;
        if (strncmp(line, "world.expo=", 11) == 0)
            cfgWorldExpo = atoi(line + 11) ? 1 : 0;
        cfgWorldExpoVal = cfgFloat(line, "world.expoval", cfgWorldExpoVal);
        if (strncmp(line, "world.clock=", 12) == 0)
            cfgWorldClock = atoi(line + 12) ? 1 : 0;
        cfgWorldClockVal = cfgFloat(line, "world.clockval", cfgWorldClockVal);
        if (strncmp(line, "world.fx=", 9) == 0)
            cfgWorldFx = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "world.atmo=", 11) == 0)
            cfgWorldAtmo = atoi(line + 11) ? 1 : 0;
        if (strncmp(line, "world.water=", 12) == 0)
            cfgWorldWater = atoi(line + 12) ? 1 : 0;
        if (strncmp(line, "world.grass=", 12) == 0)
            cfgWorldGrass = atoi(line + 12) ? 1 : 0;
        if (strncmp(line, "world.fovon=", 12) == 0)
            cfgWorldFov = atoi(line + 12) ? 1 : 0;
        cfgWorldFovVal = cfgFloat(line, "world.fov", cfgWorldFovVal);
        if (strncmp(line, "world.zoom=", 11) == 0)
            cfgWorldZoom = atoi(line + 11) ? 1 : 0;
        // ---- Server tab (Đợt 4) ----
        if (strncmp(line, "srv.antiafk=", 12) == 0)
            cfgSrvAntiAfk = atoi(line + 12) ? 1 : 0;
        cfgSrvAfkMin = cfgInt(line, "srv.afkmin", cfgSrvAfkMin);
        if (strncmp(line, "srv.autorejoin=", 15) == 0)
            cfgSrvAutoRejoin = atoi(line + 15) ? 1 : 0;
        cfgSrvHopMin = cfgInt(line, "srv.hopmin", cfgSrvHopMin);
        if (strncmp(line, "srv.hopfresh=", 13) == 0)
            cfgSrvHopFresh = atoi(line + 13) ? 1 : 0;
        if (strncmp(line, "srv.geo=", 8) == 0)
            cfgSrvGeo = atoi(line + 8) ? 1 : 0;
        if (strncmp(line, "srv.lastjob=", 12) == 0) {
            std::string j(line + 12);
            while (!j.empty() && (j.back() == '\n' || j.back() == '\r' ||
                                  j.back() == ' '))
                j.pop_back();
            if (srvValidJob(j)) cfgSrvLastJob = j;
        }
        if (strncmp(line, "srv.lastplace=", 14) == 0)
            cfgSrvLastPlace = strtoull(line + 14, nullptr, 10);
    }
    fclose(f);
    if (legacySmooth)
        printf("[aimcfg] legacy smoothing key ignored (aim.smooth/aim.tsmooth/"
               "aim.memsmooth); using aim.smoothing=%.2f\n", cfgAimSmoothing);
    if (cfgAimFov < 20) cfgAimFov = 20;
    if (cfgAimFov > 800) cfgAimFov = 800;
    if (cfgAimSmooth < 1) cfgAimSmooth = 1;
    if (cfgAimSmooth > 30) cfgAimSmooth = 30;
    if (cfgAimSmoothing < 1.0f) cfgAimSmoothing = 1.0f;
    if (cfgAimSmoothing > 20.0f) cfgAimSmoothing = 20.0f;
    if (cfgAimPScale < 0) cfgAimPScale = 0;
    if (cfgAimPScale > 10) cfgAimPScale = 10;
    if (cfgAimDynK < 0) cfgAimDynK = 0;
    if (cfgAimDynK > 5) cfgAimDynK = 5;
    if (cfgAimNearest != 0) cfgAimNearest = 1;
    if (cfgAimOverextend < 0) cfgAimOverextend = 0;
    if (cfgAimOverextend > 1) cfgAimOverextend = 1;
    if (cfgAimHeight < -3) cfgAimHeight = -3;
    if (cfgAimHeight > 6) cfgAimHeight = 6;
    if (cfgAimKeyMode != 0) cfgAimKeyMode = 1;
    if (cfgAimBone < 0 || cfgAimBone > 5) cfgAimBone = 0;
    if (cfgAimNPCs != 0) cfgAimNPCs = 1;
    if (cfgAimStickTime < 0) cfgAimStickTime = 0;
    if (cfgAimStickTime > 30) cfgAimStickTime = 30;
    if (cfgAimMode < 0 || cfgAimMode > 2) cfgAimMode = 0;
    if (cfgTrigKeyMode != 0) cfgTrigKeyMode = 1;
    if (cfgTrigDelay < 0) cfgTrigDelay = 0;
    if (cfgTrigDelay > 1000) cfgTrigDelay = 1000;
    if (cfgTrigCooldown < 0) cfgTrigCooldown = 0;
    if (cfgTrigPress < 5) cfgTrigPress = 5;
    if (cfgTrigPad < 0) cfgTrigPad = 0;
    if (cfgAimInterval < 0) cfgAimInterval = 0;
    if (cfgACInterval < 10) cfgACInterval = 10;
    if (cfgACInterval > 5000) cfgACInterval = 5000;
    if (cfgACDuration < 0) cfgACDuration = 0;
    if (cfgACDuration > 3600000) cfgACDuration = 3600000;
    if (cfgACPress < 5) cfgACPress = 5;
    if (cfgACPress > 500) cfgACPress = 500;
    if (cfgACPress >= cfgACInterval) cfgACPress = cfgACInterval / 2;
    if (cfgACKeyMode != 0) cfgACKeyMode = 1;
    if (cfgPlySpeedVal < 0) cfgPlySpeedVal = 0;
    if (cfgPlySpeedVal > 500) cfgPlySpeedVal = 500;
    if (cfgPlyJumpVal < 0) cfgPlyJumpVal = 0;
    if (cfgPlyJumpVal > 500) cfgPlyJumpVal = 500;
    if (cfgPlyMode < 0 || cfgPlyMode > 1) cfgPlyMode = 1;
    if (cfgFlySpeed < 5) cfgFlySpeed = 5;
    if (cfgFlySpeed > 500) cfgFlySpeed = 500;
    if (cfgFlyEnabled != 0) cfgFlyEnabled = 1;
    if (cfgTpSpeed < 60) cfgTpSpeed = 60;
    if (cfgTpSpeed > 1200) cfgTpSpeed = 1200;
    // Stunt tab: keep every persisted value inside its slider/table range. The
    // preset clamp must move with the table (Stunt::presetCount) so an old
    // stunt.preset=13 can never point outside the array.
    if (cfgStuntPreset < 0 || cfgStuntPreset >= Stunt::presetCount())
        cfgStuntPreset = 0;
    if (cfgStuntSpeed < 0.25f) cfgStuntSpeed = 0.25f;
    if (cfgStuntSpeed > 2.0f) cfgStuntSpeed = 2.0f;
    if (cfgStuntAmp < 0.1f) cfgStuntAmp = 0.1f;
    if (cfgStuntAmp > 3.0f) cfgStuntAmp = 3.0f;
    if (cfgStuntEnabled != 0) cfgStuntEnabled = 1;
    if (cfgStuntRagdoll != 0) cfgStuntRagdoll = 1;
    if (cfgStuntLoop != 0) cfgStuntLoop = 1;
    if (cfgStuntHover != 0) cfgStuntHover = 1;
    if (cfgStuntMode != 0) cfgStuntMode = 1;
    if (cfgAimTSmooth < 0) cfgAimTSmooth = 0;
    if (cfgAimTSmooth > 0.99f) cfgAimTSmooth = 0.99f;
    if (cfgAimSens <= 0) cfgAimSens = 1.0f;
    if (cfgAimGameSens < 0) cfgAimGameSens = 0;
    if (cfgAimPid != 0) cfgAimPid = 1;
    if (cfgAimMemSmooth < 0) cfgAimMemSmooth = 0;
    if (cfgAimMemSmooth > 0.99f) cfgAimMemSmooth = 0.99f;
    if (cfgAimRate < 30) cfgAimRate = 30;
    if (cfgAimRate > 1000) cfgAimRate = 1000;
    // World tab: keep every persisted value inside its slider range.
    if (cfgWorldBright < 1.0f) cfgWorldBright = 1.0f;
    if (cfgWorldBright > 3.0f) cfgWorldBright = 3.0f;
    if (cfgWorldExpoVal < -2.0f) cfgWorldExpoVal = -2.0f;
    if (cfgWorldExpoVal > 2.0f) cfgWorldExpoVal = 2.0f;
    if (cfgWorldClockVal < 0.0f) cfgWorldClockVal = 0.0f;
    if (cfgWorldClockVal > 24.0f) cfgWorldClockVal = 24.0f;
    if (cfgWorldFovVal < 70.0f) cfgWorldFovVal = 70.0f;
    if (cfgWorldFovVal > 120.0f) cfgWorldFovVal = 120.0f;
    // Server tab: same rule for the persisted hop/anti-AFK settings.
    if (cfgSrvAfkMin < 5) cfgSrvAfkMin = 5;
    if (cfgSrvAfkMin > 15) cfgSrvAfkMin = 15;
    if (cfgSrvHopMin < 0) cfgSrvHopMin = 0;
    if (cfgSrvHopMin > 50) cfgSrvHopMin = 50;
    if (cfgSrvAntiAfk != 0) cfgSrvAntiAfk = 1;
    if (cfgSrvAutoRejoin != 0) cfgSrvAutoRejoin = 1;
    if (cfgSrvHopFresh != 0) cfgSrvHopFresh = 1;
    if (cfgSrvGeo != 0) cfgSrvGeo = 1;
}

// ---- profiles ---------------------------------------------------------------
void loadWaypoints(); // defined below, called by loadProfileSlot
// A profile is a byte copy of the live overlay.cfg, so it carries every setting
// without a hand-written serializer that could drift. Slots are fixed names
// (p1..p9) because the panel cannot host a text box - see the g_capVk note.
static bool copyFileRaw(const char* src, const char* dst) {
    FILE* a = fopen(src, "rb");
    if (!a) return false;
    FILE* b = fopen(dst, "wb");
    if (!b) {
        fclose(a);
        return false;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        if (fwrite(buf, 1, n, b) != n) {
            fclose(a);
            fclose(b);
            return false;
        }
    fclose(a);
    fclose(b);
    return true;
}
static std::string profileFile(const std::string& slot) {
    return g_exeDir + "\\profiles\\" + slot + ".cfg";
}
bool profileExists(const std::string& slot) {
    FILE* f = fopen(profileFile(slot).c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
bool saveProfileSlot(const std::string& slot) {
    CreateDirectoryA(exePath("profiles").c_str(), nullptr);
    return copyFileRaw(exePath("overlay.cfg").c_str(), profileFile(slot).c_str());
}
bool loadProfileSlot(const std::string& slot) {
    if (!copyFileRaw(profileFile(slot).c_str(), exePath("overlay.cfg").c_str())) return false;
    loadAimCfg();                 // re-parse the file we just swapped in
    g_toggleVk = loadToggleKey();
    loadWaypoints();
    return true;
}
// First value of a string key in overlay.cfg ("" when absent).
std::string cfgStrValue(const std::string& key) {
    FILE* f = fopen(exePath("overlay.cfg").c_str(), "r");
    if (!f) return "";
    std::string pat = key + "=";
    std::string v;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, pat.c_str(), pat.size()) == 0) {
            v = line + pat.size();
            while (!v.empty() && (v.back() == '\n' || v.back() == '\r' ||
                                  v.back() == ' ' || v.back() == '\t'))
                v.pop_back();
        }
    }
    fclose(f);
    return v;
}
void setPlaceProfile(uint64_t place, const std::string& slot) {
    char key[48];
    snprintf(key, sizeof key, "profile.place.%llu", (unsigned long long)place);
    upsertCfgKey(key, slot.c_str());
}

// ---- waypoints (overlay.cfg: wp.<n>=<name>|<x>,<y>,<z>) ---------------------
void saveWaypoints() {
    FILE* f = fopen(exePath("overlay.cfg").c_str(), "r");
    std::string out;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if (strncmp(line, "wp.", 3) != 0) out += line;
        fclose(f);
    }
    for (size_t i = 0; i < g_ways.size(); ++i) {
        char b[192];
        snprintf(b, sizeof b, "wp.%d=%s|%.1f,%.1f,%.1f\n", (int)i,
                 g_ways[i].name.c_str(), g_ways[i].p.x, g_ways[i].p.y,
                 g_ways[i].p.z);
        out += b;
    }
    f = fopen(exePath("overlay.cfg").c_str(), "w");
    if (!f) return;
    fputs(out.c_str(), f);
    fclose(f);
}
void loadWaypoints() {
    g_ways.clear();
    FILE* f = fopen(exePath("overlay.cfg").c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "wp.", 3) != 0) continue;
        const char* eq = strchr(line, '=');
        if (!eq) continue;
        char nm[64] = { 0 };
        float x = 0, y = 0, z = 0;
        if (sscanf(eq + 1, "%63[^|]|%f,%f,%f", nm, &x, &y, &z) == 4) {
            Waypoint w;
            w.name = nm;
            w.p = Vec3{ x, y, z };
            g_ways.push_back(w);
        }
    }
    fclose(f);
    if (g_ways.size() > 64) g_ways.resize(64);
}
