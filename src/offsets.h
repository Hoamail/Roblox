#pragma once
// OffsetManager: fetch theo's offsets with local cache + validation.
// Sources (verified live):
//   GET https://offsets.imtheo.lol/roblox/version   -> "version-<hex>"
//   GET https://offsets.imtheo.lol/offsets.json     -> latest (392 offsets)
//   GET https://offsets.imtheo.lol/version-<id>/offsets.json -> pinned per version
//
// Rule for every number in this struct: if theo's dump publishes it, read it
// from the dump (never hardcode). Hardcoded copies of per-version layout are the
// only thing that forces manual RE after a Roblox update.
#include <string>
#include <cstdint>

struct Offsets {
    std::string robloxVersion;
    // Which body produced this struct: "live", "pinned", "newest", "good",
    // "cache". Printed at startup + diag so a stale source is obvious.
    std::string source;
    // FNV-1a of the JSON body that was accepted. Same hash = same dump, so a
    // post-mortem can tell whether a new dump landed between two runs.
    uint64_t srcHash = 0;

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
    int humanoidPlatformStand = 0;        // 473 bool (1 byte)
    int humanoidSit = 0;                 // 474 bool
    int humanoidEvaluateStateMachine = 0; // 468 bool
    int humanoidAutoRotate = 0;          // 465 bool
    int humanoidHipHeight = 0;           // 400 float
    int humanoidJumpPower = 0;
    // Games with Humanoid.UseJumpPower == false (this one) ignore JumpPower and
    // jump from JumpHeight instead: v0 = sqrt(2 * Gravity * JumpHeight).
    // JumpHeight has NO shadow copy (unlike WalkSpeed), verified by scanning the
    // whole Humanoid for a second slot holding its value -> only found in the
    // neighbouring NPC Humanoid.
    int humanoidJumpHeight = 0;
    int humanoidUseJumpPower = 0;
    int visualEngineViewMatrix = 0;
    // VisualEngine.FakeDataModel: the *second* independent route to DataModel
    // (VisualEngine -> FakeDataModel -> RealDataModel). SDK::dataModel() uses it
    // as a fallback and as a cross-check.
    int visualEngineFakeDataModel = 0;
    int instanceChildrenStart = 0;
    int instanceParent = 0;
    int instanceChildrenEnd = 0;
    int instanceName = 0;
    // Instance::NameContainer (104 on 9ac8595712b447a7): pointer to the name
    // TString, whose own start is Instance::Name (0). Older dumps had the
    // container at 112 and the TString at +8; readName() keeps that as a
    // fallback so a renamed key can never blank every Entity name again.
    int instanceNameContainer = 0;
    // Instance::ClassDescriptor / ClassName (24 / 8). Same story as the name:
    // parsed so a layout shift is a data change, not a code change.
    int instanceClassDescriptor = 24;
    int instanceClassName = 8;
    uint64_t fakeDataModelPtr = 0;
    int fakeRealDataModel = 0;
    int dataModelPlaceId = 0;
    int dataModelGameId = 0;
    int dataModelGameLoaded = 0;
    int cameraPosition = 0;
    int cameraFOV = 0;
    // Camera.Rotation MUST come from the dump. The old value here was 200, a
    // stale build's offset: the aim writer dumps 9 floats there, and in the
    // current layout 200..235 covers Camera.Rotation[2..8] *and*
    // Camera.Position.x/y (228/232). A dump without the key then corrupts the
    // camera instead of aiming (view freezes, mouse look dies). 0 = unresolved
    // and every rotation writer refuses to run.
    int cameraRotation = 0;
    // Camera.CameraSubject: Instance* the default camera module looks at every
    // RenderStep. Spectate writes another Humanoid here (see ovl_spectate.h).
    int cameraSubject = 0;
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
    // ---- Stunt tab (visible choreography on the root assembly) ----
    // All optional: a dump without these keys parses them as 0 and the Stunt
    // tab self-disables per feature. Never add them to validateOffsets' required
    // list - an older cache would then be rejected for no good reason.
    int primitiveRotation = 0;         // 176  9 float, column-major right/up/back
    int primitiveAngularVelocity = 0;  // 236  3 float, rad/s
    int primitiveFlags = 0;    // bitfield: Anchored/CanCollide/...
    int primitiveCanCollideBit = 8; // mask inside Flags (theo PrimitiveFlags)
    int primitiveAnchoredBit = 2;    // mask inside Flags (theo PrimitiveFlags)
    // ---- Đợt 2 (World tab): Lighting / Atmosphere / effects / Terrain / camera.
    // All client-local render fields. A key missing from the dump parses as 0
    // and the feature that needs it self-disables (no write, no crash).
    int lightingClockTime = 0;             // 176  float (hours 0..24)
    int lightingBrightness = 0;            // 256  float
    int lightingAmbient = 0;               // 184  Color3 (3 floats)
    int lightingOutdoorAmbient = 0;        // 232  Color3
    int lightingExposureCompensation = 0;  // 268  float
    int lightingFogStart = 0;              // 280  float
    int lightingFogEnd = 0;                // 276  float
    int lightingGlobalShadows = 0;         // 300  bool (1 byte)
    int atmosphereDensity = 0;             // 184  float
    int atmosphereHaze = 0;                // 192  float
    int atmosphereGlare = 0;               // 188  float
    int effectEnabled = 0;                 // Bloom/Blur/ColorCorrectionEffect: 152 bool
    int terrainGrassLength = 0;            // 472  float
    int terrainWaterReflectance = 0;       // 480  float
    int terrainWaterTransparency = 0;      // 484  float
    int terrainWaterWaveSize = 0;          // 488  float
    int playerMinZoomDistance = 0;         // 868  float
    int playerMaxZoomDistance = 0;         // 864  float
    // ---- Đợt 4 (Server tab): session identity + real ping. Read-only.
    int dataModelJobId = 0;        // 264  TString: 36-char UUID (join id)
    int dataModelServerIP = 0;     // 1456 TString: "ip", "ip|port" or "ip:port"
    int dataModelPlaceVersion = 0; // 412  int
    int statsItemValue = 0;        // StatsItem.Value (Stats -> "Data Ping")
    int worldGravity = 0;          // World.Gravity (544) float, read-only
};

// One HTTPS-GET implementation shared by OffsetManager and the Server tab
// (a second WinHTTP copy would drift). timeoutMs 0 = WinHTTP defaults; *status
// receives the HTTP status (-1 when no response was received). Returns the body
// ("" on any failure). Bodies above 4 MB are truncated.
std::string httpGetEx(const std::wstring& host, const std::wstring& path,
                      int timeoutMs, const wchar_t* userAgent, int* status);

// Sanity gate for a freshly parsed dump. Returns false when a *required* key is
// 0/out of range/misaligned - the signature of a renamed or restructured key.
// On success `why` may still hold non-fatal notes (optional keys that are 0 or
// bogus, e.g. StatsItem.Value). Never run the overlay on data that fails this.
bool validateOffsets(const Offsets& o, std::string& why);

// Offline parser self-tests (no game, no network):
//   -offdump <file.json>  print every parsed field as "Name=value"
//   -offtest <dir>        parse every *.json, compare with <name>.expect
int runOffDump(const std::string& jsonPath);
int runOffTest(const std::string& dir, std::string& log);

class OffsetManager {
public:
    // Returns true when offsets are ready (fresh fetch or valid cache) AND pass
    // validateOffsets().
    // runningVersion: e.g. "version-cec3ad5889b447cf" of the local Roblox build.
    // goodPath: optional "last known good" body; written on success, read as a
    // last resort before the plain cache. Empty = disabled.
    static bool ensure(const std::string& runningVersion,
                       const std::string& cachePath,
                       Offsets& out,
                       std::string& log,
                       const std::string& goodPath = std::string());
};
