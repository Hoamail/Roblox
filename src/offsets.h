#pragma once
// OffsetManager: fetch theo's offsets with local cache + version validation.
// Sources (verified live):
//   GET https://offsets.imtheo.lol/roblox/version   -> "version-<hex>"
//   GET https://offsets.imtheo.lol/offsets.json     -> latest (392 offsets)
//   GET https://offsets.imtheo.lol/version-<id>/offsets.hpp -> pinned per version
#include <string>
#include <cstdint>

struct Offsets {
    std::string robloxVersion;
    uint64_t taskSchedulerPtr = 0;
    int dataModelWorkspace = 0;
    int workspaceCurrentCamera = 0;
    int playerLocalPlayer = 0;
    int humanoidHealth = 0;
    int humanoidMaxHealth = 0;
    int humanoidWalkSpeed = 0;
    // Roblox keeps a second copy of WalkSpeed on the Humanoid ("WalkSpeedShadow",
    // theo calls it "WalkspeedCheck"). Writing WalkSpeed alone leaves the pair
    // inconsistent -> Hyperion client-behaviour kick (Error 268). Velocity writes
    // both with the identical value.
    int humanoidWalkSpeedShadow = 0;
    int humanoidJumpPower = 0;
    // Games with Humanoid.UseJumpPower == false (this one) ignore JumpPower and
    // jump from JumpHeight instead: v0 = sqrt(2 * Gravity * JumpHeight).
    // JumpHeight has NO shadow copy (unlike WalkSpeed), verified by scanning the
    // whole Humanoid for a second slot holding its value -> only found in the
    // neighbouring NPC Humanoid.
    int humanoidJumpHeight = 0;
    int humanoidUseJumpPower = 0;
    int visualEngineViewMatrix = 0;
    int instanceChildrenStart = 0;
    int instanceParent = 0;
    int instanceChildrenEnd = 0;
    int instanceName = 0;
    uint64_t fakeDataModelPtr = 0;
    int fakeRealDataModel = 0;
    int dataModelPlaceId = 0;
    int dataModelGameId = 0;
    int dataModelGameLoaded = 0;
    int cameraPosition = 0;
    int cameraFOV = 0;
    int cameraRotation = 200; // fallback; parsed when present
    uint64_t visualEnginePtr = 0;
    int userInputWS = 0;
    int windowTextBox = 0;
    int visualEngineDimensions = 0;
    int cameraViewport = 0;
    int playersLocalPlayer = 0;
    int playerModelInstance = 0;
    int playerUserId = 0;
    int playerDisplayName = 0;
    int playerTeam = 0;
    int humanoidHRP = 0;
    int basePartPrimitive = 0;
    int primitivePosition = 0;
    int primitiveVelocity = 0;
    int primitiveSize = 0;     // Vector3 hitbox dims on Primitive
    int primitiveFlags = 0;    // bitfield: Anchored/CanCollide/...
    int primitiveCanCollideBit = 8; // mask inside Flags (theo PrimitiveFlags)
    int primitiveAnchoredBit = 2;    // mask inside Flags (theo PrimitiveFlags)
    // ---- Đợt 2 (World tab): Lighting / Atmosphere / effects / Terrain / camera.
    // All client-local render fields. A key missing from the dump parses as 0
    // and the feature that needs it self-disables (no write, no crash).
    int lightingClockTime = 0;             // 184  float (hours 0..24)
    int lightingBrightness = 0;            // 264  float
    int lightingAmbient = 0;               // 192  Color3 (3 floats)
    int lightingOutdoorAmbient = 0;        // 240  Color3
    int lightingExposureCompensation = 0;  // 276  float
    int lightingFogStart = 0;              // 288  float
    int lightingFogEnd = 0;                // 284  float
    int lightingGlobalShadows = 0;         // 308  bool (1 byte)
    int atmosphereDensity = 0;             // 192  float
    int atmosphereHaze = 0;                // 200  float
    int atmosphereGlare = 0;               // 196  float
    int effectEnabled = 0;                 // Bloom/Blur/ColorCorrectionEffect: 160 bool
    int terrainGrassLength = 0;            // 480  float
    int terrainWaterReflectance = 0;       // 488  float
    int terrainWaterTransparency = 0;      // 492  float
    int terrainWaterWaveSize = 0;          // 496  float
    int playerMinZoomDistance = 0;         // 876  float
    int playerMaxZoomDistance = 0;         // 872  float
    // ---- Đợt 4 (Server tab): session identity + real ping. Read-only.
    int dataModelJobId = 0;        // 272  TString: 36-char UUID (join id)
    int dataModelServerIP = 0;     // 1464 TString: "ip", "ip|port" or "ip:port"
    int dataModelPlaceVersion = 0; // 420  int
    int statsItemValue = 0;        // 1280 StatsItem.Value (Stats -> "Data Ping")
};

// One HTTPS-GET implementation shared by OffsetManager and the Server tab
// (a second WinHTTP copy would drift). timeoutMs 0 = WinHTTP defaults; *status
// receives the HTTP status (-1 when no response was received). Returns the body
// ("" on any failure). Bodies above 4 MB are truncated.
std::string httpGetEx(const std::wstring& host, const std::wstring& path,
                      int timeoutMs, const wchar_t* userAgent, int* status);

class OffsetManager {
public:
    // Returns true when offsets are ready (fresh fetch or valid cache).
    // runningVersion: e.g. "version-cec3ad5889b447cf" of the local Roblox build.
    static bool ensure(const std::string& runningVersion,
                       const std::string& cachePath,
                       Offsets& out,
                       std::string& log);
};
