#pragma once
// Struct chia sẻ giữa overlay.cpp và các tab. Move nguyên văn từ overlay.cpp.
#include "sdk.h"        // Vec3, DWORD via memory.h
#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <mutex>

struct HitRect { float l, t, r, b; };

struct Waypoint {
    std::string name;
    Vec3 p;
};

    struct Ent {
        std::string tag; // DisplayName, fallback to Name
        uint64_t player = 0;  // Player object (0 for NPCs)
        uint64_t model = 0;
        uint64_t prim = 0; // HRP Primitive (Position at +posOff)
        uint64_t headPrim = 0; // Head Primitive (bone 0), 0 if unresolved
        uint64_t limbPrim[4]{}; // left/right arm and leg anchors
        uint64_t humanoid = 0;
        uint64_t team = 0;
        bool self = false;
        bool isNPC = false; // Model-with-Humanoid under Workspace (mob/NPC)
    };

struct SrvRow {
    std::string id;      // jobId (UUID)
    int playing = 0;
    int maxPlayers = 0;
    int ping = 0;
};

struct SrvJoin {
    std::string job;
    uint64_t place = 0;
};

struct SrvState {
    uint64_t place = 0;
    uint64_t gameId = 0;
    int placeVersion = 0;
    std::string jobId;
    std::string serverIP;
    int players = 0;
    int ping = -1;         // -1 = unknown / not read yet
    DWORD jobSeen = 0;     // uptime start for this jobId (this window only)
    DWORD lastInfo = 0;    // 1 Hz info refresh
    std::string region;    // only when srv.geo is on
    std::string regionIp;
    DWORD geoTry = 0;
    bool wasLoaded = false;   // GameLoaded has been 1 at least once
    bool loaded = false;      // last GameLoaded sample
    DWORD unloadTick = 0;     // when GameLoaded==0 + 0 ents started
    bool arFired = false;     // auto-rejoin already fired for this unload
    DWORD joinCd = 0;         // last join/hop/rejoin (60 s cooldown)
    DWORD afkNext = 0;        // next anti-AFK nudge
    bool firstRoster = true;
    std::set<std::string> roster;
};

// Worker -> UI/main handoff (same shape as g_snapMu for the aim snapshot).
struct SrvWire {
    std::mutex mu;
    std::vector<SrvRow> rows;
    int listState = 0;     // 0 idle, 1 loading, 2 ok, 3 failed
    int listCode = 0;
    int listSeq = 0;       // bumped on every completed fetch
    std::string listEndpoint; // "v1" or "v2" (which URL answered)
    int geoState = 0;
    int geoSeq = 0;
    std::string geoIp, geoCity, geoCountry;
};
