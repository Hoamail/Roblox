#pragma once
// State chia sẻ: overlay.cpp sở hữu định nghĩa, tab chỉ đọc/ghi qua extern.
// Sinh tự động (Appendix B) + prototype thêm tay (S1.3).
#include "ovl_types.h"
#include "ovl_stunt.h"   // StuntParams (Stunt tab, index 8)
#include <string>
#include <vector>

// ---- runtime state (đã move sớm ở S1.2) ----
extern bool g_diagState;
extern SrvState srv;

// ---- cfg + runtime state (85 biến) ----
extern int cfgACEnabled;
extern int cfgACKey;
extern bool cfgAimEnabled;
extern int cfgAimFov;
extern int cfgAimKey;
extern float cfgAimSmoothing;
extern int cfgAutoProfile;
extern int cfgEspBox;
extern int cfgEspColEnemy;
extern int cfgEspColNpc;
extern int cfgEspColTeam;
extern int cfgEspDist;
extern int cfgEspFill;
extern int cfgEspHead;
extern int cfgEspHp;
extern int cfgEspLine;
extern int cfgEspMaxDist;
extern int cfgEspName;
extern int cfgEspNpc;
extern int cfgEspOn;
extern int cfgEspOof;
extern int cfgEspSkel;
extern int cfgFlyEnabled;
extern int cfgFlyKey;
extern float cfgFlySpeed;
extern int cfgHitboxEnabled;
extern int cfgHudFeats;
extern int cfgHudOn;
extern int cfgMenuTab;
extern int cfgMenuUnlockMouse;
extern float cfgMenuX;
extern float cfgMenuY;
extern int cfgPanicKey;
extern int cfgPlyJump;
extern int cfgPlyJumpKey;
extern int cfgPlyJumpMode;
extern float cfgPlyJumpVal;
extern int cfgPlyMode;
extern int cfgPlySpeed;
extern int cfgPlySpeedKey;
extern float cfgPlySpeedVal;
extern int cfgRadarOn;
extern int cfgRadarRange;
extern int cfgRadarSize;
extern int cfgSrvAfkMin;
extern int cfgSrvAntiAfk;
extern int cfgSrvAutoRejoin;
extern int cfgSrvGeo;
extern int cfgSrvHopFresh;
extern int cfgSrvHopMin;
extern int cfgTpNpc;
extern int cfgTpSpeed;
extern int cfgTpInstant; // 1 = Piggyback snap, 0 = travel at cfgTpSpeed
// Teleport tab: saved-waypoint highlighter (drawn by ovl_esp.cpp).
extern int cfgWpHl;      // 1 = draw a marker over every saved waypoint
extern int cfgWpHlColor; // packed 0xRRGGBB
extern int cfgWpHlMax;   // studs, 0 = no limit
extern int g_specWant;        // Teleport tab: 1 = spectate g_specKey
extern int g_specIsPlayer;    // 1 = Player instance, 0 = NPC Model
extern uint64_t g_specKey;
extern char g_specName[48];
extern int cfgTrigEnabled;
extern int cfgTrigKey;
extern int cfgTrigKeyMode;
extern int cfgWda;
extern int cfgWorldAtmo;
extern float cfgWorldBright;
extern int cfgWorldClock;
extern float cfgWorldClockVal;
extern int cfgWorldExpo;
extern float cfgWorldExpoVal;
extern int cfgWorldFog;
extern int cfgWorldFov;
extern float cfgWorldFovVal;
extern int cfgWorldFull;
extern int cfgWorldFx;
extern int cfgWorldGrass;
extern int cfgWorldShadows;
extern int cfgWorldWater;
extern int cfgWorldZoom;
extern const char* g_colHover;
extern int g_flyActive;
extern int g_jumpActive;
extern bool g_menuPosForce;
extern float g_plyCurSpeed;
extern int g_speedActive;
extern int g_srvSel;
extern SrvWire g_srvWire;
extern int g_toggleVk;
extern char g_tpPosName[48];
extern DWORD g_tpPosTick;
extern DWORD g_tpRequestTick;
extern Vec3 g_tpTargetPos;
extern uint64_t g_tpTargetPrim;
extern std::vector<Waypoint> g_ways;

// ---- Stunt tab (index 8) ----
// cfg is the single source of truth; the main loop copies it into
// g_stuntParams each frame and hands that to the writer thread.
extern int cfgStuntEnabled, cfgStuntPreset, cfgStuntRagdoll, cfgStuntLoop,
    cfgStuntHover, cfgStuntKey, cfgStuntMode;
extern float cfgStuntSpeed, cfgStuntAmp;
extern int g_stuntActive;        // hotkey state (session only, like g_flyActive)
extern int g_stuntHold;          // "Run (hold)" button, used when stunt.key=0
extern uint64_t g_stuntTargetPrim;
extern StuntParams g_stuntParams;

// ---- World tab (phát hiện bởi worldTick trong overlay.cpp) ----
extern uint64_t wLighting, wTerrain, wAtmosphere;
extern uint64_t wBloom, wBlur, wCC, wLocalPlr;

float velextAlphaFromDt(float smoothing, float dt);
// ---- Visual tab: bảng màu (11 phần tử) ----
extern const int kColorPal[11];

void saveWaypoints();
// ---- hàm overlay.cpp cung cấp cho tab ----
std::string srvShort(const std::string& j);
std::vector<SrvRow> srvFilterRows(const std::vector<SrvRow>& in,
                                  const std::string& curJob);
void srvRequestList(uint64_t place);
void srvOpenJoin(uint64_t place, const std::string& job, const char* why);
bool srvClipGet(std::string& out);
bool srvClipSet(const std::string& s);
bool srvParseJoin(const std::string& raw, uint64_t curPlace, SrvJoin& out);
std::string srvBuildLink(uint64_t place, const std::string& job);
bool profileExists(const std::string& slot);
bool saveProfileSlot(const std::string& slot);
bool loadProfileSlot(const std::string& slot);
void setPlaceProfile(uint64_t place, const std::string& slot);
// ---- hitbox profile (EntityCtx::refreshEnts gọi khi đổi PlaceId) ----
void loadHitboxProfile(uint64_t place);
void hitboxRestoreAll(Memory& m, const Offsets& o);
