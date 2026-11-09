#include "ovl_tabs.h"
#include "ovl_ui.h"
#include "ovl_state.h"
#include "ovl_stunt.h"
#include "ovl_spectate.h"
#include "ovl_tp.h"
#include "imgui.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

// Teleport tab: player/NPC roster with one-click guided dashes, camera
// spectate, and the saved-waypoint list (save / TP / delete / highlight).
//
// The tab never writes memory. A click only publishes a request
// (g_tpTargetPrim / g_tpPosTick); the main loop validates the self/target
// primitives and hands the job to Tp::request (ovl_tp.cpp), which holds
// AssemblyLinearVelocity until the character arrives. Every disabled button
// here has a visible reason above it instead of silently dropping the click
// in the main loop.
void drawTabTeleport(MenuCtx& c) {
    Memory& m = c.m;
    const Offsets& o = c.o;
    std::vector<Ent>& ents = c.ents;
    bool haveWrite = c.haveWrite;
    bool flyApply = c.flyApply;
    DWORD nowLoop = c.nowTick;

    // ---- teleport availability -------------------------------------------
    // One reason, one line. A read-only session (Roblox version != offsets)
    // never starts the Tp writer, and fly/stunt own the same velocity field.
    const char* tpWhy = nullptr;
    ImVec4 tpWhyCol(1.0f, 0.72f, 0.35f, 1.0f);
    if (!haveWrite) {
        tpWhy = "read-only session (version mismatch) - teleport disabled";
        tpWhyCol = ImVec4(1.0f, 0.45f, 0.35f, 1.0f);
    } else if (!o.primitivePosition) {
        tpWhy = "missing Primitive.Position offset - teleport disabled";
        tpWhyCol = ImVec4(1.0f, 0.45f, 0.35f, 1.0f);
    } else if (Stunt::active()) {
        tpWhy = "stunt is running - stop it to teleport";
    } else if (flyApply) {
        tpWhy = "fly is armed - release it to teleport";
    }
    const bool tpReady = (tpWhy == nullptr);

    if (tpWhy) ImGui::TextColored(tpWhyCol, "%s", tpWhy);

    // ---- roster ----------------------------------------------------------
    Vec3 selfPos{};
    bool selfOk = false;
    for (auto& e : ents) {
        if (!e.self || !e.prim) continue;
        if (m.read(e.prim + (uint64_t)o.primitivePosition, selfPos) &&
            std::isfinite(selfPos.x) && std::isfinite(selfPos.y) &&
            std::isfinite(selfPos.z))
            selfOk = true;
        break;
    }

    struct Row {
        float d;
        int idx;
        bool hpOk;
        float hp;
    };
    std::vector<Row> rows;
    rows.reserve(ents.size());
    for (size_t i = 0; i < ents.size(); ++i) {
        Ent& e = ents[i];
        if (!e.prim) continue;
        if (e.isNPC ? !cfgTpNpc : !e.player) continue;
        Vec3 p{};
        if (!m.read(e.prim + (uint64_t)o.primitivePosition, p) ||
            !std::isfinite(p.x) || !std::isfinite(p.y) ||
            !std::isfinite(p.z))
            continue;
        Row r{};
        r.idx = (int)i;
        r.d = 1.0e9f;
        r.hpOk = false;
        r.hp = 0.0f;
        if (e.self)
            r.d = -1.0f; // self first
        else if (selfOk) {
            float dx = p.x - selfPos.x, dy = p.y - selfPos.y,
                  dz = p.z - selfPos.z;
            r.d = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        if (!e.isNPC && !e.self && e.humanoid && o.humanoidHealth) {
            float hp = 0.0f;
            r.hpOk = m.read(e.humanoid + (uint64_t)o.humanoidHealth, hp) &&
                     std::isfinite(hp);
            r.hp = hp;
        }
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.d < b.d; });

    if (g_diagState) {
        static DWORD lastTpDiag = 0;
        DWORD nd2 = GetTickCount();
        if (nd2 - lastTpDiag >= 2000) {
            lastTpDiag = nd2;
            printf("tpdiag selfOk=%d self=%.2f,%.2f,%.2f rows=%d ready=%d\n",
                   (int)selfOk, selfPos.x, selfPos.y, selfPos.z,
                   (int)rows.size(), (int)tpReady);
            for (size_t k = 0; k < rows.size() && k < 4; ++k) {
                Ent& e2 = ents[rows[k].idx];
                Vec3 pp{};
                bool ok = m.read(e2.prim + (uint64_t)o.primitivePosition, pp);
                printf("  tprow %-16s prim=%llx ok=%d pos=%.2f,%.2f,%.2f "
                       "d=%.2f\n",
                       e2.tag.c_str(), (unsigned long long)e2.prim, (int)ok,
                       pp.x, pp.y, pp.z, rows[k].d);
            }
            fflush(stdout);
        }
    }

    if (chkInt("NPCs", &cfgTpNpc))
        upsertCfgKey("tp.npc", cfgTpNpc ? "1" : "0");
    hitMark();
    ImGui::SameLine();
    ImGui::TextDisabled("%d listed  -  click TP", (int)rows.size());

    // One-click dash: the TP buttons no longer write Position (the solver
    // reverted that in 10-50 ms and needed spam clicks).
    ImGui::Text("TP");
    ImGui::SameLine();
    if (chkInt("instant", &cfgTpInstant))
        upsertCfgKey("tp.instant", cfgTpInstant ? "1" : "0");
    hitMark();
    ImGui::SameLine();
    if (cfgTpInstant) ImGui::BeginDisabled();
    sliderIntCfg("##tpspeed", &cfgTpSpeed, 60, 1200, "%d u/s", "tp.speed");
    hitMark();
    if (cfgTpInstant) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(cfgTpInstant ? "snap (piggyback)"
                                     : "travel speed");
    if (Tp::active()) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                           "teleporting %.2fM wr/s",
                           Tp::writesPerSec() / 1e6f);
        hitMark();
        ImGui::SameLine();
        if (ImGui::Button("stop##dash", ImVec2(48.0f, 0.0f)))
            Tp::cancel();
        hitMark();
    }
    if (g_specWant) {
        if (Spectate::active())
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                               "spectating %s", g_specName);
        else
            ImGui::TextDisabled("spectating %s (waiting)", g_specName);
        ImGui::SameLine();
        if (ImGui::Button("stop##spec", ImVec2(48.0f, 0.0f)))
            g_specWant = 0;
        hitMark();
    }

    ImGui::BeginChild("##tplist", ImVec2(324.0f, 200.0f), true);
    {
        // The whole list area must take the mouse: otherwise the wheel sits on
        // an unmarked pixel, falls through to the game and zooms the camera
        // instead of scrolling the list.
        ImVec2 cp = ImGui::GetWindowPos();
        ImVec2 cs = ImGui::GetWindowSize();
        g_menuHits.push_back({ cp.x, cp.y, cp.x + cs.x, cp.y + cs.y });
    }
    if (rows.empty())
        ImGui::TextDisabled(haveWrite ? "(no players in this server yet)"
                                      : "(read-only session: roster unavailable)");
    for (auto& r : rows) {
        Ent& e = ents[r.idx];
        ImGui::PushID((const void*)(uintptr_t)e.prim);
        bool isSpec = g_specWant &&
                      (g_specIsPlayer ? (e.player && e.player == g_specKey)
                                      : (e.isNPC && e.model == g_specKey));
        // Spectate: pin Camera.CameraSubject to the target Humanoid. The game's
        // own camera module then follows that character; mouse orbit and scroll
        // zoom keep working normally.
        bool canSpec = !e.self && haveWrite && !Stunt::active() &&
                       o.cameraSubject && e.humanoid;
        if (!canSpec) ImGui::BeginDisabled();
        if (ImGui::Button(isSpec ? "off" : "spec", ImVec2(40.0f, 0.0f)) &&
            canSpec) {
            if (isSpec) {
                g_specWant = 0;
            } else {
                g_specWant = 1;
                g_specIsPlayer = e.isNPC ? 0 : 1;
                g_specKey = e.isNPC ? e.model : e.player;
                snprintf(g_specName, sizeof g_specName, "%s", e.tag.c_str());
                toast("spectate %s", e.tag.c_str());
            }
        }
        if (!canSpec) ImGui::EndDisabled();
        hitMark();
        ImGui::SameLine();
        bool canTp = !e.self && tpReady;
        if (!canTp) ImGui::BeginDisabled();
        if (ImGui::Button(e.self ? "you" : "TP", ImVec2(36.0f, 0.0f)) &&
            canTp) {
            g_tpTargetPrim = e.prim; // the loop starts the dash
            g_tpRequestTick = nowLoop;
        }
        if (!canTp) ImGui::EndDisabled();
        hitMark();
        ImGui::SameLine();
        if (e.self)
            ImGui::Text("%s  (you)", e.tag.c_str());
        else if (r.hpOk && r.hp <= 0.0f)
            ImGui::Text("%s  %.0fm  dead", e.tag.c_str(), r.d);
        else if (r.hpOk)
            ImGui::Text("%s  %.0fm  hp %.0f", e.tag.c_str(), r.d, r.hp);
        else
            ImGui::Text("%s  %.0fm", e.tag.c_str(), r.d);
        ImGui::PopID();
    }
    ImGui::EndChild();

    // ---- waypoints --------------------------------------------------------
    ImGui::Separator();
    ImGui::Text("waypoints");

    // Save / TP always use the self position sampled above: the old loop broke
    // out on the first roster entry and could report "self not found" while the
    // local character was in fact resolvable.
    if (ImGui::Button("save waypoint", ImVec2(120.0f, 0.0f))) {
        if (!selfOk)
            toast("wp: self not found");
        else if (g_ways.size() >= 64)
            toast("wp: list full (64)");
        else {
            Waypoint w{};
            char nm[24];
            snprintf(nm, sizeof nm, "wp%02d", (int)g_ways.size() + 1);
            w.name = nm;
            w.p = selfPos;
            g_ways.push_back(w);
            saveWaypoints();
            toast("saved %s", nm);
        }
    }
    hitMark();
    ImGui::SameLine();
    {
        // Two-click clear: waypoints are persistent and a misclick next to
        // "save" must not wipe the list (there is no undo).
        static DWORD wpClearArm = 0;
        bool armed = wpClearArm && (nowLoop - wpClearArm) < 2500;
        if (ImGui::Button(armed ? "sure?##wpclear" : "clear all##wpclear",
                          ImVec2(90.0f, 0.0f))) {
            if (armed) {
                g_ways.clear();
                saveWaypoints();
                wpClearArm = 0;
                toast("waypoints cleared");
            } else {
                wpClearArm = nowLoop;
                toast("clear all: click again");
            }
        }
        hitMark();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d / 64 saved", (int)g_ways.size());

    // Highlighter controls: the marker itself is drawn by ovl_esp.cpp from the
    // shared view matrix (works even when the ESP master switch is off).
    if (chkInt("highlight", &cfgWpHl))
        upsertCfgKey("wp.hl", cfgWpHl ? "1" : "0");
    hitMark();
    if (cfgWpHl) {
        ImGui::SameLine();
        // Swatch button that cycles the same palette as the Visual tab. A real
        // colour picker is a popup and popups cannot be dismissed reliably here
        // (the panel never takes keyboard focus), so click-to-cycle it is.
        const int np = (int)(sizeof(kColorPal) / sizeof(kColorPal[0]));
        float rr = (float)((cfgWpHlColor >> 16) & 0xFF) / 255.0f;
        float gg = (float)((cfgWpHlColor >> 8) & 0xFF) / 255.0f;
        float bb = (float)(cfgWpHlColor & 0xFF) / 255.0f;
        float lum = 0.299f * rr + 0.587f * gg + 0.114f * bb;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(rr, gg, bb, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(rr, gg, bb, 0.72f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(rr, gg, bb, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_Text, lum > 0.55f
                                                 ? ImVec4(0.06f, 0.06f, 0.06f, 1)
                                                 : ImVec4(1, 1, 1, 1));
        if (ImGui::Button("col##wphl", ImVec2(46.0f, 19.0f))) {
            int idx = -1;
            for (int i = 0; i < np; ++i)
                if (kColorPal[i] == cfgWpHlColor) {
                    idx = i;
                    break;
                }
            cfgWpHlColor = kColorPal[(idx + 1) % np];
            char cb[24];
            snprintf(cb, sizeof cb, "%d", cfgWpHlColor);
            upsertCfgKey("wp.hlcolor", cb);
        }
        ImGui::PopStyleColor(4);
        hitMark();
        ImGui::SameLine();
        sliderIntCfg("##wpmax", &cfgWpHlMax, 0, 3000, "%d m", "wp.hlmax");
        hitMark();
        ImGui::SameLine();
        ImGui::TextDisabled("draw range (0 = all)");
    }

    ImGui::BeginChild("##wplist", ImVec2(324.0f, 120.0f), true);
    {
        ImVec2 cp = ImGui::GetWindowPos();
        ImVec2 cs = ImGui::GetWindowSize();
        g_menuHits.push_back({ cp.x, cp.y, cp.x + cs.x, cp.y + cs.y });
    }
    if (g_ways.empty()) {
        ImGui::TextDisabled("(none yet - stand somewhere");
        ImGui::TextDisabled(" and hit save waypoint)");
    }
    {
        int wpDel = -1;
        for (size_t i = 0; i < g_ways.size(); ++i) {
            ImGui::PushID(1000 + (int)i);
            bool canTp = tpReady;
            if (!canTp) ImGui::BeginDisabled();
            if (ImGui::Button("TP", ImVec2(36.0f, 0.0f)) && canTp) {
                g_tpTargetPos = g_ways[i].p; // the loop starts the dash
                g_tpPosTick = nowLoop;
                snprintf(g_tpPosName, sizeof g_tpPosName, "%s",
                         g_ways[i].name.c_str());
            }
            if (!canTp) ImGui::EndDisabled();
            hitMark();
            ImGui::SameLine();
            if (ImGui::Button("X", ImVec2(24.0f, 0.0f)))
                wpDel = (int)i;
            hitMark();
            ImGui::SameLine();
            float wd = -1.0f;
            if (selfOk) {
                float dx = g_ways[i].p.x - selfPos.x,
                      dy = g_ways[i].p.y - selfPos.y,
                      dz = g_ways[i].p.z - selfPos.z;
                wd = sqrtf(dx * dx + dy * dy + dz * dz);
            }
            char lb[160];
            if (wd >= 0.0f)
                snprintf(lb, sizeof lb, "%s  %.0fm  %.0f,%.0f,%.0f",
                         g_ways[i].name.c_str(), wd, g_ways[i].p.x,
                         g_ways[i].p.y, g_ways[i].p.z);
            else
                snprintf(lb, sizeof lb, "%s  %.0f,%.0f,%.0f",
                         g_ways[i].name.c_str(), g_ways[i].p.x,
                         g_ways[i].p.y, g_ways[i].p.z);
            ImGui::TextUnformatted(lb);
            ImGui::PopID();
        }
        if (wpDel >= 0) {
            g_ways.erase(g_ways.begin() + wpDel);
            saveWaypoints();
            toast("waypoint deleted");
        }
    }
    ImGui::EndChild();
}
