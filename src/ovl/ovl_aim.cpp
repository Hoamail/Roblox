// Aim tick / trigger / autoclick, moved verbatim from overlay.cpp (S3.3a).
#include "ovl_aim.h"
#include "ovl_view.h"
#include "ovl_core.h"  // qpcSeconds / qpcNs
#include "ovl_cfg.h"   // cfgAim* / cfgTrig* / cfgAC*
#include "ovl_state.h" // cfg* tab state
#include "imgui.h"
#include <cmath>
#include <cstdio>

AimCtx::AimCtx(Memory& m_, SDK& sdk_, const Offsets& o_, std::vector<Ent>& ents_,
               bool& haveWrite_, uint64_t& camAddr_, Vec3& cam_,
               int& vpx_, int& vpy_, int& vpw_, int& vph_, int& sw_, int& sh_,
               bool& aimOn_, bool& trigOn_, bool& acOn_, uint64_t& stickPlayer_)
  : m(m_), sdk(sdk_), o(o_), ents(ents_), haveWrite(haveWrite_),
    camAddr(camAddr_), cam(cam_), vpx(vpx_), vpy(vpy_), vpw(vpw_), vph(vph_),
    sw(sw_), sh(sh_), aimOn(aimOn_), trigOn(trigOn_), acOn(acOn_),
    stickPlayer(stickPlayer_) {}

void AimCtx::tick(ImDrawList* dl, const float* vm, bool haveVm,
                  DWORD fgPid, bool typingNow) {
    int vw = vpw ? vpw : sw, vh = vph ? vph : sh;
    int ccx = (vpw ? vpx : 0) + vw / 2;
    int ccy = (vph ? vpy : 0) + vh / 2;

    // Draw FOV at the true crosshair (not Windows cursor)
    dl->AddCircle(ImVec2((float)ccx, (float)ccy), (float)cfgAimFov,
                  IM_COL32(255, 255, 255, 90), 48);
    bool wantAim = aimOn && cfgAimEnabled && fgPid == m.pid && !typingNow;
    bool wantTrig = trigOn && cfgTrigEnabled && fgPid == m.pid && !typingNow;
    shiftedNow = false;
    if (!wantAim) {
        emaInit = false;
        emaPrim = 0;
        mouseCtl = MouseCtlState{};
        accX = 0.0f;
        accY = 0.0f;
        bpInFlight = false;
        bpStampNs = 0;
        bpCursorHold = false;
        bpStaleCount = 0;
        bpStaleHold = false;
        bpProbeNs = 0;
        mouseSvc = 0;      // re-resolve on next enable (<=1/s)
        mouseSvcRetry = 0;
    }
    if ((wantAim || wantTrig) && haveVm) {
        dbgEnts = (int)ents.size();
        DWORD nowTick = GetTickCount();
        double nowQ = qpcSeconds();
        float dt = lastAimQpc > 0.0 ? (float)(nowQ - lastAimQpc) : 0.016f;
        if (dt < 0) dt = 0;
        if (dt > 0.1f) dt = 0.1f;
        lastAimQpc = nowQ;

        uint64_t localTeam = 0;
        for (auto& e : ents) {
            if (e.self) {
                localTeam = e.team;
                break;
            }
        }

        // Best-target record (v3: score-based, sticky by Player ID).
        struct AimTarget {
            int sx = 0, sy = 0;
            Vec3 world{}, root{}, vel{};
            uint64_t prim = 0, player = 0;
            float distPx = 0, radiusPx = 0, score = 0;
        };

        AimTarget bestTarget{};
        bool haveTarget = false;

        // evaluateEntity: filters + bone + prediction + project + score.
        auto evaluateEntity = [&](Ent& e, AimTarget& outT) -> bool {
            if (!e.prim) return false;
            if (e.player && o.playerModelInstance) {
                uint64_t currentModel = 0;
                if (!m.read(e.player + (uint64_t)o.playerModelInstance,
                            currentModel) ||
                    !SDK::plausiblePtr(currentModel) ||
                    currentModel != e.model)
                    return false;
            }
            // Health is a validity check, not just an optional filter:
            // retaining a dead Humanoid's last primitive causes stale
            // locks until the slow entity refresh removes it.
            if (e.humanoid && o.humanoidHealth) {
                float hp = 0.0f;
                if (!m.read(e.humanoid + (uint64_t)o.humanoidHealth, hp) ||
                    !std::isfinite(hp) || hp <= 0.0f ||
                    hp > 1000000.0f)
                    return false;
            }
            Vec3 root{};
            if (!m.read(e.prim + (uint64_t)o.primitivePosition, root))
                return false;
            if (!std::isfinite(root.x) || !std::isfinite(root.y) ||
                !std::isfinite(root.z))
                return false;
            if (cfgAimIgnoreTeam && e.team && localTeam && e.team == localTeam)
                return false;
            if (cfgAimMaxDist > 0) {
                float ddx = root.x - cam.x, ddy = root.y - cam.y,
                      ddz = root.z - cam.z;
                if (sqrtf(ddx * ddx + ddy * ddy + ddz * ddz) > cfgAimMaxDist)
                    return false;
            }
            Vec3 base = root;
            bool haveBase = false;
            uint64_t anchorPrim = 0;
            if (cfgAimBone == 0) {
                anchorPrim = e.headPrim;
            } else if (cfgAimBone >= 2 && cfgAimBone <= 5) {
                anchorPrim = e.limbPrim[cfgAimBone - 2];
            }
            if (anchorPrim) {
                Vec3 hp{};
                if (m.read(anchorPrim + (uint64_t)o.primitivePosition, hp) &&
                    std::isfinite(hp.x) && std::isfinite(hp.y) &&
                    std::isfinite(hp.z)) {
                    // A bone primitive 30+ studs from the root is not part of
                    // this character any more (destroyed part still readable in
                    // freed memory). Aiming at it pins the camera on a point
                    // that never moves again, so ignore the bone instead.
                    float bx = hp.x - root.x, by = hp.y - root.y,
                          bz = hp.z - root.z;
                    float bd = sqrtf(bx * bx + by * by + bz * bz);
                    if (bd <= 30.0f) {
                        base = hp;
                        haveBase = true;
                    } else {
                        diagRate("aim-bone-stale", 1000,
                                 "bone prim=%p is %.1f studs from root "
                                 "(player=%p) - ignored",
                                 (void*)anchorPrim, bd, (void*)e.player);
                    }
                }
            }
            if (!haveBase)
                base = Vec3{ root.x, root.y + cfgAimHeight, root.z };
            Vec3 head = base;
            if (cfgAimPredict && o.primitiveVelocity) {
                Vec3 vel{};
                if (m.read(e.prim + (uint64_t)o.primitiveVelocity, vel) &&
                    std::isfinite(vel.x) && std::isfinite(vel.y) &&
                    std::isfinite(vel.z)) {
                    head.x += vel.x * dt * cfgAimPScale;
                    outT.vel = vel;
                    head.y += vel.y * dt * cfgAimPScale;
                    head.z += vel.z * dt * cfgAimPScale;
                }
            }
            Vec3 feet{ root.x, root.y - 2.5f, root.z };
            int sx, sy, fx, fy;
            if (!w2s(head, vm, sx, sy) || !w2s(feet, vm, fx, fy))
                return false;
            outT.sx = sx;
            outT.sy = sy;
            outT.world = head;
            outT.root = root;
            outT.prim = e.prim;
            outT.player = e.player;
            float bodyPx = sqrtf((float)((sx - fx) * (sx - fx) +
                                        (sy - fy) * (sy - fy)));
            if (!std::isfinite(bodyPx) || bodyPx < 1.0f) bodyPx = 1.0f;
            float k = (cfgAimBone == 0) ? cfgAimDynK : 0.7f;
            float rawRadius = k * bodyPx + 2.0f;
            // Expand the gate for fast targets so prediction has room
            // to acquire them before they cross the center.
            float velocityGate = 0.0f;
            if (cfgAimPredict && o.primitiveVelocity) {
                Vec3 vel{};
                if (m.read(e.prim + (uint64_t)o.primitiveVelocity, vel) &&
                    std::isfinite(vel.x) && std::isfinite(vel.y) &&
                    std::isfinite(vel.z)) {
                    float speed = sqrtf(vel.x * vel.x +
                                        vel.y * vel.y +
                                        vel.z * vel.z);
                    float screenSpeedPxPerS = speed * 20.0f;
                    if (std::isfinite(screenSpeedPxPerS))
                        velocityGate = screenSpeedPxPerS * 0.100f;
                }
            }
            const float baseGate = 50.0f;
            float minGate = baseGate + velocityGate;
            float acquireRadius =
                (std::isfinite(rawRadius) && rawRadius > minGate)
                    ? rawRadius
                    : minGate;
            if (acquireRadius > (float)cfgAimFov)
                acquireRadius = (float)cfgAimFov;
            float dx = (float)(sx - ccx), dy = (float)(sy - ccy);
            float distPx = sqrtf(dx * dx + dy * dy);
            outT.distPx = distPx;
            outT.radiusPx = acquireRadius;
            // FIX 2: score prefers near-center AND big-on-screen.
            outT.score = distPx / (bodyPx * 0.5f > 8.0f ? bodyPx * 0.5f : 8.0f);
            return true;
        };

        // (entAim folded into evaluateEntity above.)


        // 1. Sticky by Player ID (players) or prim (NPCs).
        // Player objects persist across respawns; prims don't.
        if (cfgAimSticky && stickPlayer != 0) {
            for (auto& e : ents) {
                bool match = e.self ? false
                             : (e.player ? (e.player == stickPlayer)
                                         : (e.prim == stickPlayer));
                if (!match) continue;
                AimTarget t;
                if (evaluateEntity(e, t)) {
                    // FIX 3 (TrackGain v4): widen gate x2.5 while locked.
                    float trackRadius = t.radiusPx * 2.5f;
                    if (trackRadius > (float)cfgAimFov * 1.5f)
                        trackRadius = (float)cfgAimFov * 1.5f;
                    if (t.distPx <= trackRadius) {
                        bestTarget = t;
                        haveTarget = true;
                        bool drop = false;
                        if (lockTick != 0) {
                            float ox = t.world.x - lockPos.x;
                            float oy = t.world.y - lockPos.y;
                            float oz = t.world.z - lockPos.z;
                            if (sqrtf(ox * ox + oy * oy + oz * oz) > 40.0f)
                                drop = true;
                            if (stickStartTick == 0)
                                stickStartTick = nowTick;
                            if (!drop && cfgAimStickTime > 0 &&
                                nowTick - stickStartTick >
                                    (DWORD)(cfgAimStickTime * 1000.0f))
                                drop = true;
                        }
                        if (drop) {
                            haveTarget = false;
                            stickPlayer = 0;
                            stickStartTick = 0;
                            lockTick = 0;
                        } else {
                            lockPos = t.world;
                            lockTick = nowTick;
                        }
                    } else {
                        stickPlayer = 0;
                        stickStartTick = 0;
                        lockTick = 0;
                    }
                } else {
                    stickPlayer = 0;
                    stickStartTick = 0;
                    lockTick = 0;
                }
                break;
            }
        }
        // 2. Acquire: score-based by default (distance/size), nearest
        // when cfgAimNearest=1. Ghidra first-match dropped: it locked
        // far targets while ignoring near-center ones.
        if (!haveTarget) {
            float bestScore = 1e9f;
            for (auto& e : ents) {
                if (e.self) continue;
                AimTarget t;
                if (!evaluateEntity(e, t)) continue;
                if (t.distPx > t.radiusPx || t.distPx > (float)cfgAimFov)
                    continue;
                if (cfgAimNearest) {
                    if (t.distPx < bestTarget.distPx || !haveTarget) {
                        bestTarget = t;
                        haveTarget = true;
                    }
                } else {
                    if (t.score < bestScore) {
                        bestScore = t.score;
                        bestTarget = t;
                        haveTarget = true;
                    }
                }
            }
            if (haveTarget && cfgAimSticky) {
                stickPlayer = bestTarget.player ? bestTarget.player
                                                : bestTarget.prim;
                lockPos = bestTarget.world;
                lockTick = nowTick;
                stickStartTick = nowTick;
            } else if (haveTarget) {
            }
        }
        // Branch B overextend: aim past the head away from the root.
        if (haveTarget && cfgAimOverextend > 0) {
            Vec3 ext{ bestTarget.world.x + cfgAimOverextend * (bestTarget.world.x - bestTarget.root.x),
                      bestTarget.world.y + cfgAimOverextend * (bestTarget.world.y - bestTarget.root.y),
                      bestTarget.world.z + cfgAimOverextend * (bestTarget.world.z - bestTarget.root.z) };
            int ex, ey;
            if (w2s(ext, vm, ex, ey)) {
                bestTarget.sx = ex;
                bestTarget.sy = ey;
                bestTarget.world = ext;
                float dx = (float)(ex - ccx);
                float dy = (float)(ey - ccy);
                bestTarget.distPx = sqrtf(dx * dx + dy * dy);
                if (bestTarget.distPx > (float)cfgAimFov)
                    haveTarget = false;
            } else {
                haveTarget = false;
            }
        }
        if (wantAim && !haveTarget) {
            emaInit = false;
            emaPrim = 0;
            mouseCtl = MouseCtlState{};
            accX = 0.0f;
            accY = 0.0f;
            invalidateAimSnapshot();
        }
        bool acted = (wantAim && haveTarget && bestTarget.distPx <= (float)cfgAimFov);
        if (!acted || (cfgAimMode == 1 &&
                       bestTarget.distPx < cfgAimDeadzone)) {
            // Do not let the high-rate writer keep pursuing a target
            // that is no longer an actionable, current selection.
            invalidateAimSnapshot();
        }
        // One line per second with the whole decision state: enough to tell a
        // lost target (tgt=no) from a frozen one (world stops changing) and
        // from an actuation that cannot run (no-actuator line below).
        diagRate("aim-tick", 1000,
                 "aim tick on=%d mode=%d write=%d cam=%p tgt=%s prim=%p "
                 "world=%.2f,%.2f,%.2f screen=%d,%d distPx=%.1f acted=%d",
                 wantAim ? 1 : 0, cfgAimMode, haveWrite ? 1 : 0, (void*)camAddr,
                 haveTarget ? "yes" : "no", (void*)bestTarget.prim,
                 bestTarget.world.x, bestTarget.world.y, bestTarget.world.z,
                 bestTarget.sx, bestTarget.sy, bestTarget.distPx,
                 acted ? 1 : 0);
        if (acted) {
            float gdx = (float)(bestTarget.sx - ccx), gdy = (float)(bestTarget.sy - ccy);
            float gdist = sqrtf(gdx * gdx + gdy * gdy);
            if (gdist >= cfgAimDeadzone) {
                bool paced = cfgAimInterval <= 0 ||
                             (nowTick - lastActTick >= (DWORD)cfgAimInterval);
                if (paced) lastActTick = nowTick;
                if (paced && cfgAimMode == 1 &&
                    (!haveWrite || !camAddr || !o.cameraRotation))
                    diagRate("aim-no-actuator", 1000,
                             "aim mode=1 cannot actuate: write=%d cam=%p rotOff=%d",
                             haveWrite ? 1 : 0, (void*)camAddr, o.cameraRotation);
                if (paced && cfgAimMode == 2 && haveWrite && camAddr &&
                    o.cameraViewport && o.visualEngineDimensions) {
                    // Viewport-shift silent aim (Foulz): move the
                    // render viewport so the target lands on center.
                    // Neither cursor nor camera rotation is touched.
                    float resW = 0, resH = 0;
                    uint64_t ve2 = 0;
                    if (m.read(sdk.base + o.visualEnginePtr, ve2) && ve2 &&
                        m.read(ve2 + (uint64_t)o.visualEngineDimensions, resW) &&
                        m.read(ve2 + (uint64_t)o.visualEngineDimensions + 4, resH) &&
                        resW > 0 && resH > 0 && resW < 10000 && resH < 10000) {
                        int rx = vpw ? bestTarget.sx - vpx : bestTarget.sx;
                        int ry = vph ? bestTarget.sy - vpy : bestTarget.sy;
                        VP2 vp{ (short)((resW - rx) * 2),
                                (short)((resH - ry) * 2) };
                        if (m.write(camAddr + (uint64_t)o.cameraViewport, vp)) {
                            vpShifted = true;
                            shiftedNow = true;
                        }
                    }
                } else                         if (paced && cfgAimMode == 1 && haveWrite && camAddr) {
                    // Publish the target for the dedicated writer thread.
                    // Keeping one rotation writer avoids read/modify/write
                    // races with the render loop and the game's camera tick.
                    { std::lock_guard<std::mutex> lk(g_snapMu);
                      g_snap.ready = true;
                      g_snap.epoch = g_snapEpoch.load(std::memory_order_relaxed);
                      g_snap.seq = g_snapSeq.fetch_add(1, std::memory_order_relaxed) + 1;
                      g_snap.tgt = bestTarget.world;
                      g_snap.vel = bestTarget.vel;
                      g_snap.cm = cam;
                      g_snap.camAddr = camAddr;
                      g_snap.qpc = qpcSeconds(); }
                    diagRate("snapshot-publish", 100,
                             "snapshot publish epoch=%llu cam=%p player=%p prim=%p target=%.2f,%.2f,%.2f",
                             (unsigned long long)g_snapEpoch.load(),
                             (void*)camAddr, (void*)bestTarget.player,
                             (void*)bestTarget.prim,
                             bestTarget.world.x, bestTarget.world.y,
                             bestTarget.world.z);
                } else if (paced && cfgAimMode == 0) {
                    uint64_t tgtKey = bestTarget.player
                                          ? bestTarget.player
                                          : bestTarget.prim;
                    float tsmooth = cfgAimTSmooth;
                    if (tsmooth < 0.0f) tsmooth = 0.0f;
                    if (tsmooth > 0.99f) tsmooth = 0.99f;
                    float gamesens = cfgAimGameSens > 0.05f
                                         ? cfgAimGameSens
                                         : 0.05f;
                    float outGain = cfgAimSens / gamesens;
                    if (outGain < 0.1f) outGain = 0.1f;
                    if (outGain > 4.0f) outGain = 4.0f;
                    if (cfgAimPid == 0) {
                        // ---- velext mouse model (aim.pid=1 keeps the
                        // legacy filters): velext alpha, camera
                        // back-pressure, real angle->counts mapping
                        // (Phase C), per-step clamp, 1.5 px deadzone.
                        // No smoothed state is kept. ----
                        uint64_t nowNsM = qpcNs();
                        uint64_t lastNsM = 0;
                        float dtV = 0.0f, alphaV = 1.0f;
                        // [I] MouseService (f at +0x21C): resolve via
                        // DataModel children like velsample does;
                        // retried at most once per second.
                        if (!mouseSvc && nowTick - mouseSvcRetry >= 1000) {
                            mouseSvcRetry = nowTick;
                            uint64_t dmS = 0;
                            if (sdk.dataModel(dmS)) {
                                sdk.walkChildren(dmS, [&](uint64_t c) {
                                    if (sdk.readName(c) == "MouseService") {
                                        mouseSvc = c;
                                        return false;
                                    }
                                    return true;
                                });
                            }
                            diagRate("aim-mouse-svc", 2000,
                                     "MouseService resolve: %s (%p)",
                                     mouseSvc ? "ok" : "not found",
                                     (void*)mouseSvc);
                        }
                        if (velextActGate(nowNsM, &lastNsM, &dtV, &alphaV,
                                          cfgAimSmoothing)) {
                            float eX = (float)bestTarget.sx - (float)ccx;
                            float eY = (float)bestTarget.sy - (float)ccy;
                            float distPx = sqrtf(eX * eX + eY * eY);
                            float yawDeg = 0, pitchDeg = 0;
                            bool haveAng = false;
                            float cre[9]{};
                            if (camAddr &&
                                m.read(camAddr + (uint64_t)o.cameraRotation, cre))
                                haveAng = camYawPitchDeg(cre, &yawDeg, &pitchDeg);
                            bool bpBlock = false;
                            float bpDY = 0, bpDP = 0, bpAgeMs = 0;
                            if (bpInFlight && haveAng) {
                                // [P] velext back-pressure: hold the next
                                // step until the camera moved >0.02 deg
                                // on either axis, or 60 ms passed.
                                bpDY = fabsf(wrap180f(yawDeg - bpYawDeg));
                                bpDP = fabsf(wrap180f(pitchDeg - bpPitchDeg));
                                bpAgeMs = (float)((nowNsM - bpStampNs) * 1e-6);
                                if (bpDY <= kVelextBpMoveDeg &&
                                    bpDP <= kVelextBpMoveDeg &&
                                    (nowNsM - bpStampNs) < kVelextBpTimeoutNs)
                                    bpBlock = true;
                            }
                            // [I] Phase C mouse-lock guard (primary):
                            // the game hides the cursor while it holds
                            // mouse lock; a visible cursor means
                            // third-person free-look - skip inject.
                            bpCursorHold = !mouseCursorHidden();
                            if (bpCursorHold) {
                                bpStaleCount = 0;
                                bpStaleHold = false;
                            }
                            // [I] Phase C mouse-lock guard (secondary):
                            // a back-pressure release with no camera
                            // movement means the counts did nothing.
                            // N in a row => injects are held; a single
                            // probe every 750 ms checks for relock.
                            if (bpInFlight && haveAng && !bpBlock &&
                                !bpCursorHold) {
                                if (bpDY > kVelextBpMoveDeg ||
                                    bpDP > kVelextBpMoveDeg) {
                                    bpStaleCount = 0;
                                    if (bpStaleHold) {
                                        bpStaleHold = false;
                                        diagRate("aim-lock-regained", 1000,
                                                 "counts move the camera again "
                                                 "(dYaw=%.3f dPitch=%.3f) - "
                                                 "resuming injects", bpDY, bpDP);
                                    }
                                } else if (!bpStaleHold) {
                                    if (bpStaleCount < 1000000)
                                        ++bpStaleCount;
                                    if (bpStaleCount >= kVelextBpStaleMax) {
                                        bpStaleHold = true;
                                        bpProbeNs = nowNsM;
                                        diagRate("aim-stale-hold", 1000,
                                                 "no mouse lock: %d releases "
                                                 "without camera move "
                                                 "(dYaw=%.3f dPitch=%.3f)",
                                                 bpStaleCount, bpDY, bpDP);
                                    }
                                }
                            }
                            bool probeDue = bpStaleHold &&
                                (nowNsM - bpProbeNs >= kVelextBpProbeNs);
                            if (bpCursorHold) {
                                bpInFlight = false; // nothing in flight
                                diagRate("aim-cursor-hold", 2000,
                                         "mouse not locked (cursor visible) - "
                                         "inject skipped, err=%.1f px", distPx);
                            } else if (bpStaleHold && !probeDue) {
                                bpInFlight = false;
                                diagRate("aim-stale-hold", 2000,
                                         "no mouse lock (no camera move) - "
                                         "inject held, probe in %.0f ms",
                                         (float)((kVelextBpProbeNs -
                                                  (nowNsM - bpProbeNs)) * 1e-6));
                            } else if (bpBlock) {
                                diagRate("aim-bp", 500,
                                         "mouse back-pressure hold: dYaw=%.3f dPitch=%.3f age=%.1fms",
                                         bpDY, bpDP, bpAgeMs);
                            } else if (alphaV <= kVelextAlphaEps) {
                                // [P] 0x1402B7FAC: too small to matter
                            } else if (distPx <= kVelextMouseDzPx) {
                                // [P] 0x1402B8144: 1.5 px deadzone
                            } else {
                                if (probeDue) bpProbeNs = nowNsM;
                                bpInFlight = false; // step released
                                float stepX = 0.0f, stepY = 0.0f;
                                bool mapped = false;
                                // [P] Phase C: real velext mapping.
                                // sens read is the fn_14010d580
                                // chain; focal needs a sane FOV and
                                // the fresh camera read (for p).
                                VelextSens vs = readVelextSens(m, sdk.base, mouseSvc);
                                float focal = 0.0f;
                                if (vs.ok && camAddr && o.cameraFOV) {
                                    float fovV = 0.0f;
                                    // FieldOfView is radians on recent builds
                                    // (70 deg reads 1.22173); without the
                                    // conversion this used to fall through to
                                    // the legacy linear gain every frame.
                                    float fovDeg = 0.0f;
                                    if (m.read(camAddr + (uint64_t)o.cameraFOV, fovV) &&
                                        std::isfinite(fovV))
                                        fovDeg = camFovDegrees(fovV);
                                    if (fovDeg > 10.0f && fovDeg < 150.0f) {
                                        float tanHalf = tanf(fovDeg * 0.5f * kDeg2Rad);
                                        if (tanHalf > 1e-6f)
                                            focal = ((float)vh * 0.5f) / tanHalf;
                                    }
                                }
                                if (vs.ok && focal > 0.0f && haveAng) {
                                    // p = pitch component (cre[5]);
                                    // its sign cancels in 1-p*p.
                                    velextMouseCounts(eX, eY, focal, cre[5],
                                                      vs.out1, vs.out2,
                                                      alphaV, dtV,
                                                      &stepX, &stepY);
                                    mapped = true;
                                }
                                if (!mapped) {
                                    // [I] fallback: legacy linearized
                                    // aim.sens/aim.gamesens gain.
                                    diagRate("aim-map-fallback", 3000,
                                             "velext mapping unusable (sens=%d "
                                             "focal=%.1f cre=%d) - legacy gain %.3f",
                                             vs.ok ? 1 : 0, focal,
                                             haveAng ? 1 : 0, outGain);
                                    float lim = kVelextStepCapPxS * alphaV;
                                    if (lim < kVelextStepCapLo) lim = kVelextStepCapLo;
                                    if (lim > kVelextStepCapHi) lim = kVelextStepCapHi;
                                    stepX = eX * alphaV * outGain;
                                    stepY = eY * alphaV * outGain;
                                    if (stepX > lim) stepX = lim;
                                    if (stepX < -lim) stepX = -lim;
                                    if (stepY > lim) stepY = lim;
                                    if (stepY < -lim) stepY = -lim;
                                }
                                accX += stepX;
                                accY += stepY;
                                int mx = (int)accX;
                                int my = (int)accY;
                                accX -= (float)mx;
                                accY -= (float)my;
                                if (mx || my) {
                                    sendRelMouse(mx, my);
                                    if (haveAng) {
                                        bpInFlight = true;
                                        bpStampNs = nowNsM;
                                        bpYawDeg = yawDeg;
                                        bpPitchDeg = pitchDeg;
                                    }
                                }
                            }
                        }
                    } else {
                    bool resetState = (!emaInit || emaPrim != tgtKey ||
                                        !mouseCtl.active);
                    if (resetState) {
                        emaTargetX = (float)bestTarget.sx;
                        emaTargetY = (float)bestTarget.sy;
                        emaPrim = tgtKey;
                        emaInit = true;
                        mouseCtl = MouseCtlState{};
                        mouseCtl.active = true;
                        mouseCtl.lastEmaX = emaTargetX;
                        mouseCtl.lastEmaY = emaTargetY;
                        mouseCtl.lastEmaInit = true;
                        accX = 0.0f;
                        accY = 0.0f;
                    }
                        if (tsmooth < 0.0f) tsmooth = 0.0f;
                        if (tsmooth > 0.99f) tsmooth = 0.99f;
                        float tauPos = 0.025f + 0.080f * tsmooth;
                        float smoothDiv =
                            (float)(cfgAimSmooth < 1 ? 1 : cfgAimSmooth);
                        float kp = 6.0f / sqrtf(smoothDiv);
                        float ki = 0.0f;
                        const float iMax = 120.0f;
                        const float vMax = 4500.0f;
                        float outGain = cfgAimSens /
                            (cfgAimGameSens > 0.05f
                                 ? cfgAimGameSens
                                 : 0.05f);
                        if (outGain < 0.1f) outGain = 0.1f;
                        if (outGain > 4.0f) outGain = 4.0f;

                        float a = dt / (tauPos + dt);
                        float b = 0.8f * a * a;
                        float predX = emaTargetX + mouseCtl.abVX * dt;
                        float predY = emaTargetY + mouseCtl.abVY * dt;
                        float inX = (float)bestTarget.sx - predX;
                        float inY = (float)bestTarget.sy - predY;
                        emaTargetX = predX + a * inX;
                        emaTargetY = predY + a * inY;
                        if (dt > 1e-5f) {
                            mouseCtl.abVX += (b / dt) * inX;
                            mouseCtl.abVY += (b / dt) * inY;
                        }

                        // A/B branch: same viewport-center error as
                        // 1-pole (crosshair == viewport center, NOT
                        // OS cursor). Keeps EMA/alpha-beta filter so
                        // aim.pid=0 vs 1 compares filter only.
                        float eX = emaTargetX - (float)ccx;
                        float eY = emaTargetY - (float)ccy;
                        float errMag = sqrtf(eX * eX + eY * eY);
                        float velMag = sqrtf(mouseCtl.abVX * mouseCtl.abVX +
                                             mouseCtl.abVY * mouseCtl.abVY);

                        float targetMoved = 0.0f;
                        if (mouseCtl.lastEmaInit) {
                            float targetMovedX = emaTargetX -
                                                 mouseCtl.lastEmaX;
                            float targetMovedY = emaTargetY -
                                                 mouseCtl.lastEmaY;
                            targetMoved = sqrtf(targetMovedX * targetMovedX +
                                                targetMovedY * targetMovedY);
                        }
                        mouseCtl.lastEmaX = emaTargetX;
                        mouseCtl.lastEmaY = emaTargetY;
                        mouseCtl.lastEmaInit = true;

                        if (!mouseCtl.settled) {
                            if (errMag < 0.45f && velMag < 5.0f &&
                                fabsf(inX) < 0.8f &&
                                fabsf(inY) < 0.8f &&
                                targetMoved < 1.0f) {
                                mouseCtl.idleT += dt;
                                if (mouseCtl.idleT > 0.05f)
                                    mouseCtl.settled = true;
                            } else {
                                mouseCtl.idleT = 0.0f;
                            }
                        } else if (errMag > 1.10f || velMag > 14.0f ||
                                   fabsf(inX) > 1.6f ||
                                   fabsf(inY) > 1.6f ||
                                   targetMoved > 2.0f) {
                            mouseCtl.settled = false;
                            mouseCtl.idleT = 0.0f;
                        }

                        if (!mouseCtl.settled) {
                            float nextIX = mouseCtl.iX + eX * dt;
                            float nextIY = mouseCtl.iY + eY * dt;
                            mouseCtl.iX = (nextIX > iMax)
                                              ? iMax
                                              : ((nextIX < -iMax)
                                                     ? -iMax
                                                     : nextIX);
                            mouseCtl.iY = (nextIY > iMax)
                                              ? iMax
                                              : ((nextIY < -iMax)
                                                     ? -iMax
                                                     : nextIY);
                            float uX = kp * eX + ki * mouseCtl.iX;
                            float uY = kp * eY + ki * mouseCtl.iY;
                            float mag = sqrtf(uX * uX + uY * uY);
                            if (mag > vMax && mag > 1e-4f) {
                                float scale = vMax / mag;
                                uX *= scale;
                                uY *= scale;
                                mouseCtl.iX *= 0.98f;
                                mouseCtl.iY *= 0.98f;
                            }

                            accX += uX * dt * outGain;
                            accY += uY * dt * outGain;
                            int mx = (int)accX;
                            int my = (int)accY;
                            accX -= (float)mx;
                            accY -= (float)my;
                            sendRelMouse(mx, my);
                        } else {
                            accX = 0.0f;
                            accY = 0.0f;
                            mouseCtl.iX = 0.0f;
                            mouseCtl.iY = 0.0f;
                        }
                    }
                }
            dl->AddLine(ImVec2((float)ccx, (float)ccy),
                        ImVec2((float)bestTarget.sx, (float)bestTarget.sy),
                        IM_COL32(0, 255, 0, 200), 2.0f);
            dl->AddCircle(ImVec2((float)bestTarget.sx, (float)bestTarget.sy), 8.0f,
                          IM_COL32(0, 255, 0, 255), 24);
            if (bestTarget.radiusPx > 0)
                dl->AddCircle(ImVec2((float)bestTarget.sx, (float)bestTarget.sy),
                              bestTarget.radiusPx,
                              IM_COL32(0, 255, 0, 120), 32);
        }
        // ---- Triggerbot (velext recipe): auto-click when the selected
        // target sits within pad px of center. Non-blocking phases.
        if (!wantTrig && trigPhase) {
            releaseTriggerIfDown();
        }
        if (wantTrig) {
            DWORD now2 = GetTickCount();
            bool centered = haveTarget && bestTarget.distPx <= (float)cfgTrigPad;
            if (trigPhase == 0) {
                if (centered &&
                    (now2 - lastFireTick >= (DWORD)cfgTrigCooldown)) {
                    trigPhase = 1;
                    trigT0 = now2;
                }
            } else if (trigPhase == 1) {
                if (!centered) {
                    trigPhase = 0;
                } else if (now2 - trigT0 >= (DWORD)cfgTrigDelay) {
                    INPUT dn{};
                    dn.type = INPUT_MOUSE;
                    dn.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                    SendInput(1, &dn, sizeof(dn));
                    trigPhase = 2;
                    trigT0 = now2;
                }
            } else if (trigPhase == 2) {
                if (now2 - trigT0 >= (DWORD)cfgTrigPress) {
                    INPUT up{};
                    up.type = INPUT_MOUSE;
                    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                    SendInput(1, &up, sizeof(up));
                    trigPhase = 0;
                    lastFireTick = now2;
                }
            }
            if (trigPhase) {
                dl->AddCircle(ImVec2((float)ccx, (float)ccy), 10.0f,
                              IM_COL32(255, 220, 0, 255), 16);
            }
        }
    }
    }
}

void AimCtx::autoclickTick(DWORD fgPid, bool typingNow) {
    bool wantAC = acOn && cfgACEnabled && fgPid == m.pid && !typingNow;
    DWORD nowAC = GetTickCount();
    if (!wantAC && acPhase) {
        if (acPhase == 2) {
            INPUT up{};
            up.type = INPUT_MOUSE;
            up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(1, &up, sizeof(up));
        }
        acPhase = 0;
    }
    if (wantAC) {
        if (cfgACDuration > 0 && nowAC - acT0 >= (DWORD)cfgACDuration) {
            acOn = false; // burst done (also ends toggle)
            if (acPhase == 2) {
                INPUT up{};
                up.type = INPUT_MOUSE;
                up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                SendInput(1, &up, sizeof(up));
            }
            acPhase = 0;
        } else if (acPhase == 2) {
            if (nowAC - acPressT0 >= (DWORD)cfgACPress) {
                INPUT up{};
                up.type = INPUT_MOUSE;
                up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                SendInput(1, &up, sizeof(up));
                acPhase = 1;
            }
        } else if (nowAC >= acNext) {
            INPUT dn{};
            dn.type = INPUT_MOUSE;
            dn.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            SendInput(1, &dn, sizeof(dn));
            acPhase = 2;
            acPressT0 = nowAC;
            acNext = nowAC + (DWORD)cfgACInterval;
        } else {
            acPhase = 1;
        }
    }
}

void AimCtx::releaseTriggerIfDown() {
    if (trigPhase == 2) {
        INPUT up{};
        up.type = INPUT_MOUSE;
        up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &up, sizeof(up));
    }
    trigPhase = 0;
}

void AimCtx::restoreViewportIfNeeded(bool force) {
    if (!(haveWrite && vpShifted && camAddr && o.cameraViewport &&
          o.visualEnginePtr && o.visualEngineDimensions &&
          (force || !shiftedNow)))
        return;

    uint64_t ve3 = 0;
    float rw = 0.0f, rh = 0.0f;
    if (m.read(sdk.base + o.visualEnginePtr, ve3) && ve3 &&
        m.read(ve3 + (uint64_t)o.visualEngineDimensions, rw) &&
        m.read(ve3 + (uint64_t)o.visualEngineDimensions + 4, rh) &&
        rw > 0.0f && rh > 0.0f && rw < 10000.0f && rh < 10000.0f) {
        VP2 rs{ (short)rw, (short)rh };
        if (m.write(camAddr + (uint64_t)o.cameraViewport, rs))
            vpShifted = false;
    }
}

void AimCtx::onAimDisabled() {
    stickStartTick = 0;
    lockTick = 0;
    lockPos = Vec3{};
    lastAimQpc = 0;
    lastActTick = 0;
    accX = 0.0f;
    accY = 0.0f;
    emaTargetX = 0.0f;
    emaTargetY = 0.0f;
    emaPrim = 0;
    emaInit = false;
    mouseCtl = MouseCtlState{};
}

void AimCtx::onAutoclickEnabled() {
    acT0 = GetTickCount();
    acNext = acT0;
}

bool AimCtx::w2s(const Vec3& w, const float* mt, int& sx, int& sy) {
    return screenW2S(w, mt, sx, sy, vpx, vpy, vpw, vph, sw, sh);
}
