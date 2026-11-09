// 3-tier entity cache, moved verbatim from overlay.cpp (S3.1).
#include "ovl_cache.h"
#include "ovl_cfg.h"   // cfgAimNPCs
#include "ovl_core.h"  // diagRate
#include "ovl_state.h" // loadHitboxProfile / hitboxRestoreAll
#include "ovl_world.h" // worldRestoreAll
#include "ovl_aim.h"   // invalidateAimSnapshot
#include <cmath>
#include <string>

std::string EntityCtx::classOf(uint64_t obj) {
    auto it = classCache.find(obj);
    if (it != classCache.end()) return it->second;
    std::string c = sdk.readClass(obj);
    if (!c.empty()) {
        classCache[obj] = c;
        if (classCache.size() > 20000) classCache.clear();
    }
    return c;
}
// Full resolve (strings) — only for strangers and respawns.
void EntityCtx::resolveParts(Ent& e, uint64_t model) {
    e.model = model;
    e.prim = 0;
    e.headPrim = 0;
    for (uint64_t& limb : e.limbPrim) limb = 0;
    e.rootPart = 0;
    e.headPart = 0;
    for (uint64_t& lp : e.limbPart) lp = 0;
    e.humanoid = 0;
    sdk.walkChildren(model, [&](uint64_t h) {
        if (classOf(h) != "Humanoid") return true;
        e.humanoid = h;
        sdk.walkChildren(model, [&](uint64_t part) {
            if (sdk.readName(part) != "HumanoidRootPart") return true;
            uint64_t prim = 0;
            if (m.read(part + (uint64_t)o.basePartPrimitive, prim) &&
                SDK::plausiblePtr(prim)) {
                e.prim = prim;
                e.rootPart = part;
            }
            return false;
        });
        return false;
    });
    sdk.walkChildren(model, [&](uint64_t part) {
        std::string name = sdk.readName(part);
        uint64_t* dst = nullptr;
        uint64_t* pst = nullptr;
        if (name == "Head") {
            dst = &e.headPrim;
            pst = &e.headPart;
        } else if (name == "LeftHand" || name == "LeftUpperArm" ||
                   name == "Left Arm") {
            dst = &e.limbPrim[0];
            pst = &e.limbPart[0];
        } else if (name == "RightHand" || name == "RightUpperArm" ||
                   name == "Right Arm") {
            dst = &e.limbPrim[1];
            pst = &e.limbPart[1];
        } else if (name == "LeftFoot" || name == "LeftLowerLeg" ||
                   name == "Left Leg") {
            dst = &e.limbPrim[2];
            pst = &e.limbPart[2];
        } else if (name == "RightFoot" || name == "RightLowerLeg" ||
                   name == "Right Leg") {
            dst = &e.limbPrim[3];
            pst = &e.limbPart[3];
        }
        if (dst && !*dst)
        {
            uint64_t prim = 0;
            if (m.read(part + (uint64_t)o.basePartPrimitive, prim) &&
                SDK::plausiblePtr(prim)) {
                *dst = prim;
                if (pst) *pst = part;
            }
        }
        return true;
    });
}
void EntityCtx::clearResolvedParts(Ent& e) {
    e.model = 0;
    e.prim = 0;
    e.headPrim = 0;
    e.humanoid = 0;
    e.rootPart = 0;
    e.headPart = 0;
    for (uint64_t& limb : e.limbPrim) limb = 0;
    for (uint64_t& lp : e.limbPart) lp = 0;
}
void EntityCtx::refreshEnts() {
    uint64_t dm = 0, players = 0, local = 0;
    if (!sdk.dataModel(dm)) {
        diagRate("cache-stop", 2000, "cache stop: no DataModel");
        ents.clear();
        invalidateAimSnapshot();
        return;
    }
    if (o.dataModelGameLoaded) {
        int loaded = 0;
        if (!m.read(dm + (uint64_t)o.dataModelGameLoaded, loaded) || !loaded) {
            diagRate("cache-stop", 2000,
                     "cache stop: GameLoaded=%d off=%d (place not ready?)",
                     loaded, o.dataModelGameLoaded);
            stickPlayer = 0;
            ents.clear();
            invalidateAimSnapshot();
            return;
        }
    }
    uint64_t placeNow = 0;
    sdk.placeId(dm, placeNow);
    if (placeNow && placeNow != lastPlaceSeen) {
        lastPlaceSeen = placeNow; // map hop: instant full rebuild below
        loadHitboxProfile(placeNow);
        hitboxRestoreAll(m, o);
        worldRestoreAll(m, o); // Đợt 2: the old place's instances are gone
        cDm = 0;
        cPlayers = 0;
        uisAddr = 0;
        ecache.clear();
        classCache.clear();
        npcScanSkip = 0;
    }
    if (!cDm || cDm != dm) {
        cDm = dm;
        cPlayers = 0;
        uisAddr = 0;
        ecache.clear();
        classCache.clear();
        npcScanSkip = 0;
        worldRestoreAll(m, o); // Đợt 2: new DataModel => all-new instances
    }
    if (!cPlayers) {
        if (!sdk.findChildByClass(dm, "Players", players)) {
            diagRate("cache-stop", 2000, "cache stop: Players not found dm=%p",
                     (void*)dm);
            ents.clear();
            invalidateAimSnapshot();
            return;
        }
        cPlayers = players;
    } else {
        players = cPlayers; // validate cheaply: parent must still be dm
        uint64_t par = 0;
        if (!o.instanceParent || !m.read(players + (uint64_t)o.instanceParent, par) ||
            par != dm) {
            cPlayers = 0;
            if (!sdk.findChildByClass(dm, "Players", players)) {
                ents.clear();
                invalidateAimSnapshot();
                return;
            }
            cPlayers = players;
        }
    }
    if (!m.read(players + (uint64_t)o.playersLocalPlayer, local) ||
        !SDK::plausiblePtr(local)) {
        diagRate("cache-stop", 2000,
                 "cache stop: LocalPlayer unreadable players=%p off=%d local=%p",
                 (void*)players, o.playersLocalPlayer, (void*)local);
        ents.clear();
        invalidateAimSnapshot();
        return;
    }
    if (!uisAddr && o.userInputWS)
        sdk.findChildByClass(dm, "UserInputService", uisAddr);
    std::map<uint64_t, bool> seen;
    sdk.walkChildren(players, [&](uint64_t p) {
        if (classOf(p) != "Player") return true;
        seen[p] = true;
        auto it = ecache.find(p);
        if (it == ecache.end()) {
            Ent e;
            e.self = (p == local);
            e.player = p;
            m.read(p + (uint64_t)o.playerTeam, e.team);
            e.tag = sdk.readTStringDirect(p + (uint64_t)o.playerDisplayName);
            if (e.tag.empty()) e.tag = sdk.readName(p);
            uint64_t model = 0;
            if (m.read(p + (uint64_t)o.playerModelInstance, model) &&
                SDK::plausiblePtr(model))
                resolveParts(e, model);
            if (e.prim) ecache[p] = e;
        } else {
            Ent& e = it->second;
            e.self = (p == local);
            m.read(p + (uint64_t)o.playerTeam, e.team);
            uint64_t model = 0;
            if (!m.read(p + (uint64_t)o.playerModelInstance, model) ||
                !SDK::plausiblePtr(model)) {
                // ModelInstance is swapped while the character is re-rigged, so
                // a single unreadable sample must not cost the entity (dropping
                // it made ESP flicker 7->5->7). Keep the last proven-good parts
                // - but still prove they are alive: a destroyed part keeps a
                // readable Primitive whose Position is frozen, and a ghost box
                // stuck at a dead position is worse than a one-refresh gap.
                if (e.prim) {
                    uint64_t par = 0;
                    bool rootGone =
                        e.rootPart && o.instanceParent &&
                        m.read(e.rootPart + (uint64_t)o.instanceParent, par) &&
                        par != e.model;
                    Vec3 probe{};
                    bool unreadable =
                        o.primitivePosition &&
                        (!m.read(e.prim + (uint64_t)o.primitivePosition, probe) ||
                         !std::isfinite(probe.x) || !std::isfinite(probe.y) ||
                         !std::isfinite(probe.z));
                    if (rootGone || unreadable) clearResolvedParts(e);
                }
                return true;
            }
            // A part that is no longer a child of the character still has a
            // readable Primitive whose Position is frozen at the moment it was
            // destroyed: the aim would keep writing that direction forever
            // (camera stops following, view feels stuck). Instance.Parent is
            // the cheapest liveness proof we have.
            auto partGone = [&](uint64_t part) -> bool {
                if (!part || !o.instanceParent) return false;
                uint64_t par = 0;
                return m.read(part + (uint64_t)o.instanceParent, par) &&
                       par != model;
            };
            bool staleParts = !e.prim;
            if (!staleParts && o.primitivePosition) {
                Vec3 probe{};
                staleParts =
                    !m.read(e.prim + (uint64_t)o.primitivePosition, probe) ||
                    !std::isfinite(probe.x) || !std::isfinite(probe.y) ||
                    !std::isfinite(probe.z);
            }
            bool boneDropped = false;
            if (!staleParts && partGone(e.rootPart))
                staleParts = true;
            if (!staleParts && partGone(e.headPart))
                boneDropped = true;
            for (int bi = 0; bi < 4 && !staleParts; ++bi)
                if (partGone(e.limbPart[bi])) boneDropped = true;
            // A corpse keeps ModelInstance pointing at the husk: once its root
            // part is gone there is nothing left to resolve, so retrying every
            // 250 ms until the respawn only burns walks. Wait for the model
            // change instead (health <= 0 proves it is not a streaming rig).
            bool husk = false;
            if (!e.prim && e.humanoid && o.humanoidHealth) {
                float hpC = 0.0f;
                husk = m.read(e.humanoid + (uint64_t)o.humanoidHealth, hpC) &&
                       (!std::isfinite(hpC) || hpC <= 0.0f);
            }
            if (model != e.model || (!husk && (staleParts || boneDropped)))
            {
                const char* reason = model != e.model ? "model-change"
                                   : staleParts ? "part-gone"
                                                : "bone-gone";
                diagRate("entity-reresolve", 250,
                         "entity re-resolve player=%p oldModel=%p newModel=%p oldPrim=%p reason=%s",
                         (void*)e.player, (void*)e.model, (void*)model,
                         (void*)e.prim, reason);
                resolveParts(e, model); // respawned, streamed in or re-rigged
            }
        }
        return true;
    });
    for (auto it = ecache.begin(); it != ecache.end();) {
        if (!seen.count(it->first) && !it->second.isNPC)
            it = ecache.erase(it);
        else
            ++it;
    }
    // ---- NPCs/mobs: Model instances under Workspace with a Humanoid ----
    // (excludes the local player's own character). Budgeted so a crowded
    // map can't stall the render thread; the scan cursor advances between
    // refreshes without caching incomplete streamed-in models as props.
    bool npcSweepComplete = false;
    // The Teleport tab lists NPCs too, so the sweep must run when either the
    // aim NPC filter or the Teleport "NPCs" checkbox asks for it. Gating this
    // on cfgAimNPCs alone made the Teleport NPC toggle a no-op (no NPC ever
    // entered the cache, so the list stayed empty).
    if (cfgAimNPCs || cfgTpNpc) {
        uint64_t ws = 0, selfModel = 0;
        if (o.dataModelWorkspace && m.read(dm + (uint64_t)o.dataModelWorkspace, ws) && ws) {
            for (auto& kv : ecache) {
                if (kv.second.self) {
                    selfModel = kv.second.model;
                    break;
                }
            }
            if (!selfModel && local) {
                uint64_t lm = 0;
                if (m.read(local + (uint64_t)o.playerModelInstance, lm)) selfModel = lm;
            }
            // Player character Models are ALSO Model+Humanoid under
            // Workspace -> skip ALL of them, not just self, or every
            // enemy gets a duplicate orange NPC box over the red/green one.
            std::set<uint64_t> playerModels;
            for (auto& kv : ecache)
                if (!kv.second.isNPC && kv.second.model)
                    playerModels.insert(kv.second.model);
            if (selfModel) playerModels.insert(selfModel);
            for (auto it = ecache.begin(); it != ecache.end();) {
                if (it->second.isNPC &&
                    (playerModels.count(it->first) ||
                     playerModels.count(it->second.model)))
                    it = ecache.erase(it);
                else
                    ++it;
            }
            int budget = 12;
            size_t candidateIndex = 0;
            size_t nextScanSkip = npcScanSkip;
            bool stoppedByBudget = false;
            sdk.walkChildren(ws, [&](uint64_t ch) {
                if (classOf(ch) != "Model" || ch == selfModel ||
                    playerModels.count(ch))
                    return true;
                size_t currentIndex = candidateIndex++;
                if (currentIndex < npcScanSkip) {
                    if (ecache.count(ch))
                        seen[ch] = true;
                    return true;
                }
                if (budget <= 0) {
                    stoppedByBudget = true;
                    return false;
                }
                nextScanSkip = currentIndex + 1;
                if (ecache.count(ch)) {
                    seen[ch] = true;
                    Ent& ne = ecache[ch];
                    // Same ghost rule as players: an NPC whose root part was
                    // re-parented/destroyed must be re-resolved, not aimed at.
                    if (o.instanceParent && ne.rootPart) {
                        uint64_t par = 0;
                        if (m.read(ne.rootPart + (uint64_t)o.instanceParent, par) &&
                            par != ch) {
                            diagRate("npc-reresolve", 1000,
                                     "npc re-resolve model=%p oldPrim=%p reason=part-gone",
                                     (void*)ch, (void*)ne.prim);
                            resolveParts(ne, ch);
                        }
                    }
                    return true;
                }
                --budget;
                bool hasHum = false;
                sdk.walkChildren(ch, [&](uint64_t h) {
                    if (classOf(h) == "Humanoid") {
                        hasHum = true;
                        return false;
                    }
                    return true;
                });
                if (!hasHum) return true;
                Ent e;
                e.isNPC = true;
                e.player = 0;
                e.self = false;
                e.tag = sdk.readName(ch);
                if (e.tag.empty()) return true;
                resolveParts(e, ch);
                if (e.prim) {
                    ecache[ch] = e;
                    seen[ch] = true;
                }
                return true;
            });
            if (stoppedByBudget) {
                npcScanSkip = nextScanSkip;
            } else {
                npcScanSkip = 0;
                npcSweepComplete = true;
            }
        }
    } else {
        npcScanSkip = 0;
        npcSweepComplete = true;
    }
    for (auto it = ecache.begin(); it != ecache.end();) {
        if (!seen.count(it->first) &&
            (!it->second.isNPC || npcSweepComplete))
            it = ecache.erase(it);
        else
            ++it;
    }
    ents.clear();
    for (auto& kv : ecache)
        if (kv.second.prim) ents.push_back(kv.second);
    diagRate("entity-refresh", 1000,
             "entity refresh dm=%p players=%p local=%p cache=%zu ents=%zu",
             (void*)dm, (void*)players, (void*)local,
             ecache.size(), ents.size());
}
