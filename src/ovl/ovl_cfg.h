#pragma once
// overlay.cfg / profiles / waypoints IO. Definitions live in ovl_cfg.cpp.
#include "ovl_types.h"
#include <cstdint>
#include <string>

extern std::string g_exeDir;

void initExeDir();
std::string exePath(const char* name);
int  loadToggleKey();
void loadAimCfg();
bool upsertCfgKey(const char* key, const char* value);
std::string cfgStrValue(const std::string& key);
bool profileExists(const std::string& slot);
bool saveProfileSlot(const std::string& slot);
bool loadProfileSlot(const std::string& slot);
void setPlaceProfile(uint64_t place, const std::string& slot);
void loadWaypoints();
// saveWaypoints: prototype stays in ovl_state.h (tab Teleport already uses it).

// 41 vars were static in overlay.cpp; the parser needs to read them.
// Definitions stay in overlay.cpp, only `static` is removed.
extern float g_menuW, g_menuH, g_savedMenuW, g_savedMenuH;
extern int cfgACKeyMode, cfgACInterval, cfgACDuration, cfgACPress;
extern int cfgAimKeyMode, cfgAimSmooth, cfgAimMode, cfgAimSticky;
extern float cfgAimStickTime;
extern int cfgAimPredict;
extern float cfgAimPScale, cfgAimDynK;
extern int cfgAimNearest;
extern float cfgAimOverextend, cfgAimHeight, cfgAimMaxDist, cfgAimDeadzone;
extern int cfgAimIgnoreTeam, cfgAimInterval, cfgAimBone, cfgAimNPCs;
extern float cfgAimTSmooth, cfgAimSens, cfgAimGameSens;
extern int cfgAimPid;
extern float cfgAimMemSmooth;
extern int cfgAimRate;
extern int cfgTrigDelay, cfgTrigCooldown, cfgTrigPress, cfgTrigPad;
extern int cfgChatGuard;
extern int cfgPlySdnKey, cfgPlySupKey, cfgPlyJdnKey, cfgPlyJupKey;
extern int cfgMenuCollapsed;
extern std::string cfgSrvLastJob;
extern uint64_t cfgSrvLastPlace;
// ---- Stunt tab (index 8): parsed by loadAimCfg, defined in overlay.cpp ----
extern int cfgStuntEnabled, cfgStuntPreset, cfgStuntRagdoll, cfgStuntLoop,
    cfgStuntHover, cfgStuntKey, cfgStuntMode;
extern float cfgStuntSpeed, cfgStuntAmp;
