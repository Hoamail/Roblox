// World tab module, moved verbatim from overlay.cpp (S2.4).
#include "ovl_world.h"
#include "ovl_state.h"
#include "ovl_core.h"
#include <cmath>
#include <cstdio>
#include <cstring>

// ---- World (Đợt 2): lighting / atmosphere / effects / terrain / camera -------
// Every field below is client-local render state (Lighting, Atmosphere, the
// post effects, Terrain, Camera + Player zoom): a write changes this machine's
// picture only, nothing replicates. PLAN §0.2 discipline still applies: write
// only while the feature is ticked, remember the original before the first
// write, restore on untick / panic / exit, and after the one-shot apply re-read
// at most 4x/s and re-write only on drift. FOV is the deliberate exception:
// the game rebuilds Camera.FieldOfView every render, so it is written every
// loop while ticked, which is the only way the override sticks.
// (The cfg flags themselves are declared near loadAimCfg, which parses them.)

// Instance cache. Reset (with the saved originals) on place change, DataModel
// rebuild and camera/local-player swap; while something is missing the
// resolve pass retries at most once a second.
uint64_t wLighting = 0, wTerrain = 0, wAtmosphere = 0;
uint64_t wBloom = 0, wBlur = 0, wCC = 0, wLocalPlr = 0;
static DWORD wResolveTick = 0;  // last resolve attempt (<= 1/s)
static DWORD wVerifyTick = 0;   // last class re-validation (every 2 s)
static DWORD wDiffTick = 0;     // last drift read-back (<= 4/s)

// One float field / one bool (1 byte) field / one Color3 (3 floats) field.
// "have" = the original was captured before our first write; "wrote" = the
// wanted value has been written at least once.
struct WorldF { bool have = false; uint64_t addr = 0; float orig = 0.0f; float last = 0.0f; bool wrote = false; };
struct WorldB { bool have = false; uint64_t addr = 0; unsigned char orig = 0, last = 0; bool wrote = false; };
struct WorldV3 { bool have = false; uint64_t addr = 0; float orig[3]{}; float last[3]{}; bool wrote = false; };
// Every field of the tab, so "forget everything" is one assignment.
struct WorldState {
    WorldF bright, expo, clock, fogS, fogE, zoomMin, zoomMax, fov, fov2;
    WorldF dens, haze, glare, watTr, watWave, watRefl, grass;
    WorldB shadow, bloomB, blurB, ccB;
    WorldV3 amb, outAmb;
};
static WorldState worldSt;

// FOV raw values are radians on some builds (11f155f3: 70 deg == 1.22173) and
// degrees on others; anything at or below this is read as radians.
static constexpr float kFovRadMax = 3.2f;
// One place that formats a raw FOV for the diag lines: "70.0deg(raw 1.2217)"
// when the slot holds radians, plain "90.0" when it holds degrees.
static void fovFmt(char* b, size_t n, float raw) {
    if (raw > 0.0f && raw <= kFovRadMax)
        snprintf(b, n, "%.1fdeg(raw %.4f)", raw * 57.29578f, raw);
    else
        snprintf(b, n, "%.1f", raw);
}

static bool wfSeed(Memory& m, WorldF& f, uint64_t addr) {
    float cur = 0.0f;
    if (!addr || !m.read(addr, cur) || !std::isfinite(cur)) return false;
    f = WorldF{};
    f.have = true;
    f.addr = addr;
    f.orig = cur;
    return true;
}
// Seed on first use (or when the instance changed: the stale original is
// dropped, never written to a dead address), write on enable / on any value
// change. Drift repair is wfDiff at <= 4 Hz.
static void wfApply(Memory& m, WorldF& f, uint64_t addr, float want) {
    if (!addr) return;
    if ((!f.have || f.addr != addr) && !wfSeed(m, f, addr)) return;
    if (!f.wrote || f.last != want) {
        if (m.write(addr, want)) {
            f.last = want;
            f.wrote = true;
        }
    }
}
static void wfDiff(Memory& m, WorldF& f, float want) {
    if (!f.have || !f.addr) return;
    float cur = 0.0f;
    if (m.read(f.addr, cur) && cur != want && m.write(f.addr, want)) f.last = want;
}
static void wfRestore(Memory& m, WorldF& f) {
    if (f.have && f.addr) m.write(f.addr, f.orig);
    f = WorldF{};
}
static bool wbSeed(Memory& m, WorldB& f, uint64_t addr) {
    unsigned char cur = 0;
    if (!addr || !m.read(addr, cur)) return false;
    f = WorldB{};
    f.have = true;
    f.addr = addr;
    f.orig = cur;
    return true;
}
static void wbApply(Memory& m, WorldB& f, uint64_t addr, unsigned char want) {
    if (!addr) return;
    if ((!f.have || f.addr != addr) && !wbSeed(m, f, addr)) return;
    if (!f.wrote || f.last != want) {
        if (m.write(addr, want)) {
            f.last = want;
            f.wrote = true;
        }
    }
}
static void wbDiff(Memory& m, WorldB& f, unsigned char want) {
    if (!f.have || !f.addr) return;
    unsigned char cur = 0;
    if (m.read(f.addr, cur) && cur != want && m.write(f.addr, want)) f.last = want;
}
static void wbRestore(Memory& m, WorldB& f) {
    if (f.have && f.addr) m.write(f.addr, f.orig);
    f = WorldB{};
}
static bool wvSeed(Memory& m, WorldV3& f, uint64_t addr) {
    float cur[3]{};
    if (!addr || !m.read(addr, cur)) return false;
    f = WorldV3{};
    f.have = true;
    f.addr = addr;
    memcpy(f.orig, cur, sizeof cur);
    return true;
}
// Color3 writes go through writeBytes: the array would decay to a pointer in
// the generic write<T> template and send 8 bytes of pointer instead of the RGB.
static void wvApply(Memory& m, WorldV3& f, uint64_t addr, const float want[3]) {
    if (!addr) return;
    if ((!f.have || f.addr != addr) && !wvSeed(m, f, addr)) return;
    if (!f.wrote || memcmp(f.last, want, sizeof(float) * 3) != 0) {
        if (m.writeBytes(addr, want, sizeof(float) * 3)) {
            memcpy(f.last, want, sizeof(float) * 3);
            f.wrote = true;
        }
    }
}
static void wvDiff(Memory& m, WorldV3& f, const float want[3]) {
    if (!f.have || !f.addr) return;
    float cur[3]{};
    if (m.read(f.addr, cur) && memcmp(cur, want, sizeof cur) != 0 &&
        m.writeBytes(f.addr, want, sizeof(float) * 3))
        memcpy(f.last, want, sizeof(float) * 3);
}
static void wvRestore(Memory& m, WorldV3& f) {
    if (f.have && f.addr) m.writeBytes(f.addr, f.orig, sizeof(float) * 3);
    f = WorldV3{};
}

// Put back every original we saved, then drop the instance cache. This is also
// the place-change / DataModel-rebuild reset: writes to already-destroyed
// instances simply fail, and that is the point - a stale "original" from the
// old place must never be re-seeded as if it were the new place's value.
void worldRestoreAll(Memory& m, const Offsets& o) {
    (void)o;
    bool any = worldSt.bright.have || worldSt.expo.have || worldSt.clock.have ||
               worldSt.fogS.have || worldSt.shadow.have || worldSt.bloomB.have ||
               worldSt.dens.have || worldSt.watTr.have || worldSt.grass.have ||
               worldSt.fov.have || worldSt.zoomMin.have;
    // Captured before the helpers clear the state: the diag line then shows
    // which real game values went back in.
    float rBright = worldSt.bright.orig, rFogS = worldSt.fogS.orig,
          rFov = worldSt.fov.orig;
    wfRestore(m, worldSt.bright);
    wvRestore(m, worldSt.amb);
    wvRestore(m, worldSt.outAmb);
    wfRestore(m, worldSt.fogS);
    wfRestore(m, worldSt.fogE);
    wbRestore(m, worldSt.shadow);
    wfRestore(m, worldSt.expo);
    wfRestore(m, worldSt.clock);
    wbRestore(m, worldSt.bloomB);
    wbRestore(m, worldSt.blurB);
    wbRestore(m, worldSt.ccB);
    wfRestore(m, worldSt.dens);
    wfRestore(m, worldSt.haze);
    wfRestore(m, worldSt.glare);
    wfRestore(m, worldSt.watTr);
    wfRestore(m, worldSt.watWave);
    wfRestore(m, worldSt.watRefl);
    wfRestore(m, worldSt.grass);
    wfRestore(m, worldSt.fov);
    wfRestore(m, worldSt.fov2);
    wfRestore(m, worldSt.zoomMin);
    wfRestore(m, worldSt.zoomMax);
    wLighting = wTerrain = wAtmosphere = 0;
    wBloom = wBlur = wCC = wLocalPlr = 0;
    wResolveTick = wVerifyTick = 0;
    if (any && g_diagState) {
        char fovTxt[48];
        fovFmt(fovTxt, sizeof fovTxt, rFov);
        printf("world restore all (orig bright=%.2f fogStart=%.0f fov=%s)\n",
               rBright, rFogS, fovTxt);
        fflush(stdout);
    }
}
// Lighting is a child of the DataModel, Terrain a child of Workspace.
// Atmosphere and the post effects are children of Lighting; some games parent
// them under the Camera instead, so both containers are scanned.
static void worldResolve(Memory& m, SDK& sdk, const Offsets& o, uint64_t dm,
                         uint64_t ws) {
    if (!wLighting) sdk.findChildByClass(dm, "Lighting", wLighting);
    if (wLighting) {
        uint64_t cam = 0;
        sdk.camera(ws, cam);
        auto scan = [&](uint64_t root) {
            if (!root) return;
            sdk.walkChildren(root, [&](uint64_t c) {
                std::string cls = sdk.readClass(c);
                if (cls == "Atmosphere") {
                    if (!wAtmosphere) wAtmosphere = c;
                } else if (cls == "BloomEffect") {
                    if (!wBloom) wBloom = c;
                } else if (cls == "BlurEffect") {
                    if (!wBlur) wBlur = c;
                } else if (cls == "ColorCorrectionEffect") {
                    if (!wCC) wCC = c;
                }
                return true;
            });
        };
        scan(wLighting);
        scan(cam);
    }
    if (!wTerrain && ws) sdk.findChildByClass(ws, "Terrain", wTerrain);
    if (!wLocalPlr) {
        uint64_t players = 0, local = 0;
        if (o.playersLocalPlayer && sdk.findChildByClass(dm, "Players", players) &&
            players && m.read(players + (uint64_t)o.playersLocalPlayer, local) &&
            SDK::plausiblePtr(local))
            wLocalPlr = local;
    }
}

void worldTick(Memory& m, SDK& sdk, const Offsets& o, bool haveWrite,
                      DWORD nowLoop, uint64_t camAddr) {
    uint64_t dm = 0, ws = 0;
    if (!sdk.dataModel(dm)) {
        worldRestoreAll(m, o); // DataModel gone (leave/hop): nothing to keep
        return;
    }
    sdk.workspace(dm, ws);
    // Class re-validation: a same-place rejoin mints new instances without
    // tripping the place-change branch in refreshEnts.
    if (wLighting && nowLoop - wVerifyTick >= 2000) {
        wVerifyTick = nowLoop;
        if (sdk.readClass(wLighting) != "Lighting") {
            worldRestoreAll(m, o);
        } else if (wLocalPlr && sdk.readClass(wLocalPlr) != "Player") {
            wLocalPlr = 0;
            worldSt.zoomMin = WorldF{}; // stale player: drop, never write
            worldSt.zoomMax = WorldF{};
        }
    }
    if ((!wLighting || !wTerrain || !wAtmosphere || !wBloom || !wBlur || !wCC ||
         !wLocalPlr) &&
        nowLoop - wResolveTick >= 1000) {
        wResolveTick = nowLoop;
        worldResolve(m, sdk, o, dm, ws);
    }
    diagRate("world", 5000,
             "world lighting=%p terrain=%p atmo=%p fx=%d/%d/%d plr=%p",
             (void*)wLighting, (void*)wTerrain, (void*)wAtmosphere,
             wBloom ? 1 : 0, wBlur ? 1 : 0, wCC ? 1 : 0, (void*)wLocalPlr);
    if (g_diagState) {
        static DWORD lastWorldDiag = 0;
        if (nowLoop - lastWorldDiag >= 5000) {
            lastWorldDiag = nowLoop;
            // Read-only probe of the marquee slots: proves a write landed (or
            // that a restore did) without having to eyeball the picture.
            float vb = 0.0f, vfs = 0.0f, vfe = 0.0f, vfov = 0.0f, vclk = 0.0f;
            if (wLighting && o.lightingBrightness)
                m.read(wLighting + (uint64_t)o.lightingBrightness, vb);
            if (wLighting && o.lightingFogStart)
                m.read(wLighting + (uint64_t)o.lightingFogStart, vfs);
            if (wLighting && o.lightingFogEnd)
                m.read(wLighting + (uint64_t)o.lightingFogEnd, vfe);
            if (wLighting && o.lightingClockTime)
                m.read(wLighting + (uint64_t)o.lightingClockTime, vclk);
            if (camAddr && o.cameraFOV) m.read(camAddr + (uint64_t)o.cameraFOV, vfov);
            // The unit/format traps (radians slot, frozen-clock hacks) are
            // explained where the values come from; the diag just stays
            // readable, e.g. "fov=70.0deg(raw 1.2217) clock=-1.66e+15(=8.00 mod 24)".
            char fovTxt[48], clkTxt[48];
            fovFmt(fovTxt, sizeof fovTxt, vfov);
            if (std::isfinite(vclk) && fabsf(vclk) > 24.0f) {
                float m24 = fmodf(vclk, 24.0f);
                if (m24 < 0.0f) m24 += 24.0f;
                snprintf(clkTxt, sizeof clkTxt, "%.3g(=%.2f mod 24)", vclk, m24);
            } else {
                snprintf(clkTxt, sizeof clkTxt, "%.2f", vclk);
            }
            printf("world lighting=%p terrain=%p atmo=%p fx=%d/%d/%d plr=%p "
                   "vals bright=%.2f fogS=%.0f fogE=%.0f fov=%s clock=%s\n",
                   (void*)wLighting, (void*)wTerrain, (void*)wAtmosphere,
                   wBloom ? 1 : 0, wBlur ? 1 : 0, wCC ? 1 : 0, (void*)wLocalPlr,
                   vb, vfs, vfe, fovTxt, clkTxt);
            fflush(stdout);
        }
    }
    if (!haveWrite) return; // read-only handle: status only, no writes
    bool diffNow = (nowLoop - wDiffTick >= 250); // drift read-back <= 4/s
    if (diffNow) wDiffTick = nowLoop;
    const float white[3] = { 1.0f, 1.0f, 1.0f };

    // 1) Fullbright: Brightness slider + white Ambient/OutdoorAmbient.
    if (cfgWorldFull && wLighting && o.lightingBrightness && o.lightingAmbient &&
        o.lightingOutdoorAmbient) {
        wfApply(m, worldSt.bright, wLighting + (uint64_t)o.lightingBrightness,
                cfgWorldBright);
        wvApply(m, worldSt.amb, wLighting + (uint64_t)o.lightingAmbient, white);
        wvApply(m, worldSt.outAmb, wLighting + (uint64_t)o.lightingOutdoorAmbient,
                white);
        if (diffNow) {
            wfDiff(m, worldSt.bright, cfgWorldBright);
            wvDiff(m, worldSt.amb, white);
            wvDiff(m, worldSt.outAmb, white);
        }
    } else {
        wfRestore(m, worldSt.bright);
        wvRestore(m, worldSt.amb);
        wvRestore(m, worldSt.outAmb);
    }
    // 2) Anti-fog: push both planes out of sight (restore keeps both originals).
    if (cfgWorldFog && wLighting && o.lightingFogStart && o.lightingFogEnd) {
        wfApply(m, worldSt.fogS, wLighting + (uint64_t)o.lightingFogStart, 100000.0f);
        wfApply(m, worldSt.fogE, wLighting + (uint64_t)o.lightingFogEnd, 100000.0f);
        if (diffNow) {
            wfDiff(m, worldSt.fogS, 100000.0f);
            wfDiff(m, worldSt.fogE, 100000.0f);
        }
    } else {
        wfRestore(m, worldSt.fogS);
        wfRestore(m, worldSt.fogE);
    }
    // 3) No shadows (bool byte).
    if (cfgWorldShadows && wLighting && o.lightingGlobalShadows) {
        wbApply(m, worldSt.shadow, wLighting + (uint64_t)o.lightingGlobalShadows, 0);
        if (diffNow) wbDiff(m, worldSt.shadow, 0);
    } else {
        wbRestore(m, worldSt.shadow);
    }
    // 4) Exposure override.
    if (cfgWorldExpo && wLighting && o.lightingExposureCompensation) {
        wfApply(m, worldSt.expo,
                wLighting + (uint64_t)o.lightingExposureCompensation, cfgWorldExpoVal);
        if (diffNow) wfDiff(m, worldSt.expo, cfgWorldExpoVal);
    } else {
        wfRestore(m, worldSt.expo);
    }
    // 5) Time of day: written when the slider moves, kept frozen by the diff.
    if (cfgWorldClock && wLighting && o.lightingClockTime) {
        wfApply(m, worldSt.clock, wLighting + (uint64_t)o.lightingClockTime,
                cfgWorldClockVal);
        if (diffNow) wfDiff(m, worldSt.clock, cfgWorldClockVal);
    } else {
        wfRestore(m, worldSt.clock);
    }
    // 6) Bloom / Blur / ColorCorrection off (each may not exist in a game:
    // the missing ones are simply skipped).
    if (cfgWorldFx && o.effectEnabled) {
        if (wBloom)
            wbApply(m, worldSt.bloomB, wBloom + (uint64_t)o.effectEnabled, 0);
        else
            wbRestore(m, worldSt.bloomB);
        if (wBlur)
            wbApply(m, worldSt.blurB, wBlur + (uint64_t)o.effectEnabled, 0);
        else
            wbRestore(m, worldSt.blurB);
        if (wCC)
            wbApply(m, worldSt.ccB, wCC + (uint64_t)o.effectEnabled, 0);
        else
            wbRestore(m, worldSt.ccB);
        if (diffNow) {
            wbDiff(m, worldSt.bloomB, 0);
            wbDiff(m, worldSt.blurB, 0);
            wbDiff(m, worldSt.ccB, 0);
        }
    } else {
        wbRestore(m, worldSt.bloomB);
        wbRestore(m, worldSt.blurB);
        wbRestore(m, worldSt.ccB);
    }
    // 7) Atmosphere off.
    if (cfgWorldAtmo && wAtmosphere && o.atmosphereDensity && o.atmosphereHaze &&
        o.atmosphereGlare) {
        wfApply(m, worldSt.dens, wAtmosphere + (uint64_t)o.atmosphereDensity, 0.0f);
        wfApply(m, worldSt.haze, wAtmosphere + (uint64_t)o.atmosphereHaze, 0.0f);
        wfApply(m, worldSt.glare, wAtmosphere + (uint64_t)o.atmosphereGlare, 0.0f);
        if (diffNow) {
            wfDiff(m, worldSt.dens, 0.0f);
            wfDiff(m, worldSt.haze, 0.0f);
            wfDiff(m, worldSt.glare, 0.0f);
        }
    } else {
        wfRestore(m, worldSt.dens);
        wfRestore(m, worldSt.haze);
        wfRestore(m, worldSt.glare);
    }
    // 8) Clear + flat water (Terrain).
    if (cfgWorldWater && wTerrain && o.terrainWaterTransparency &&
        o.terrainWaterWaveSize && o.terrainWaterReflectance) {
        wfApply(m, worldSt.watTr,
                wTerrain + (uint64_t)o.terrainWaterTransparency, 1.0f);
        wfApply(m, worldSt.watWave,
                wTerrain + (uint64_t)o.terrainWaterWaveSize, 0.0f);
        wfApply(m, worldSt.watRefl,
                wTerrain + (uint64_t)o.terrainWaterReflectance, 0.0f);
        if (diffNow) {
            wfDiff(m, worldSt.watTr, 1.0f);
            wfDiff(m, worldSt.watWave, 0.0f);
            wfDiff(m, worldSt.watRefl, 0.0f);
        }
    } else {
        wfRestore(m, worldSt.watTr);
        wfRestore(m, worldSt.watWave);
        wfRestore(m, worldSt.watRefl);
    }
    // 9) No grass. [I] Terrain.WaterColor (464, Color3) is not touched this
    // round: it needs a colour UI and the panel cannot host popups (see the
    // colour-bar note in the Visual tab), so it waits for a palette button.
    if (cfgWorldGrass && wTerrain && o.terrainGrassLength) {
        wfApply(m, worldSt.grass, wTerrain + (uint64_t)o.terrainGrassLength, 0.0f);
        if (diffNow) wfDiff(m, worldSt.grass, 0.0f);
    } else {
        wfRestore(m, worldSt.grass);
    }
    // 10) FOV: rewritten every loop (the game re-derives FieldOfView each
    // render, so a one-shot or 4 Hz diff write would be visibly eaten).
    // Unit trap: on 11f155f3 the slot stores RADIANS (70 deg == 1.22173), so
    // the seed picks the unit once per camera (|orig| <= kFovRadMax ->
    // radians) and the slider stays in degrees. The same raw value also sits
    // in a second slot 12 bytes on (verified equal at seed on the live build,
    // independent of +304 afterwards); it is kept in sync while ticked so
    // whichever copy the renderer polls is covered - but only when it
    // started equal, which keeps the write off any unrelated field.
    if (cfgWorldFov && camAddr && o.cameraFOV) {
        uint64_t fa = camAddr + (uint64_t)o.cameraFOV;
        if (worldSt.fov.have && worldSt.fov.addr != fa) {
            worldSt.fov = WorldF{};  // camera swapped: stale originals dropped
            worldSt.fov2 = WorldF{};
        }
        if (!worldSt.fov.have) {
            if (wfSeed(m, worldSt.fov, fa)) {
                uint32_t a = 0, b = 0;
                if (worldSt.fov.orig > 0.0f && m.read(fa, a) &&
                    m.read(fa + 12, b) && a == b)
                    wfSeed(m, worldSt.fov2, fa + 12);
            }
        }
        if (worldSt.fov.have) {
            float want = (fabsf(worldSt.fov.orig) <= kFovRadMax)
                             ? cfgWorldFovVal * 0.0174532925f
                             : cfgWorldFovVal;
            if (m.write(fa, want)) {
                worldSt.fov.last = want;
                worldSt.fov.wrote = true;
            }
            if (worldSt.fov2.have && worldSt.fov2.addr == fa + 12 &&
                m.write(fa + 12, want)) {
                worldSt.fov2.last = want;
                worldSt.fov2.wrote = true;
            }
        }
    } else if (!cfgWorldFov) {
        wfRestore(m, worldSt.fov);
        wfRestore(m, worldSt.fov2);
    } else {
        worldSt.fov = WorldF{}; // no camera this tick: forget, never write stale
        worldSt.fov2 = WorldF{};
    }
    // 11) Far zoom: Player.MinZoomDistance / MaxZoomDistance.
    if (cfgWorldZoom && wLocalPlr && o.playerMinZoomDistance &&
        o.playerMaxZoomDistance) {
        wfApply(m, worldSt.zoomMin,
                wLocalPlr + (uint64_t)o.playerMinZoomDistance, 0.5f);
        wfApply(m, worldSt.zoomMax,
                wLocalPlr + (uint64_t)o.playerMaxZoomDistance, 2000.0f);
        if (diffNow) {
            wfDiff(m, worldSt.zoomMin, 0.5f);
            wfDiff(m, worldSt.zoomMax, 2000.0f);
        }
    } else {
        wfRestore(m, worldSt.zoomMin);
        wfRestore(m, worldSt.zoomMax);
    }
}
