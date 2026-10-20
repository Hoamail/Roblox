#include "ovl_tabs.h"
#include "ovl_ui.h"
#include "ovl_state.h"
#include "imgui.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

void drawTabTeleport(MenuCtx& c) {
    Memory& m = c.m;
    const Offsets& o = c.o;
    std::vector<Ent>& ents = c.ents;
    uint64_t place = c.place;
    bool haveWrite = c.haveWrite;
    bool aimOn = c.aimOn, trigOn = c.trigOn, acOn = c.acOn;
    bool flyApply = c.flyApply, speedApply = c.speedApply, jumpApply = c.jumpApply;
    int dbgEnts = c.dbgEnts;
    float loopMs = c.loopMs;
    DWORD nowLoop = c.nowTick;
                // ---------------- Teleport: player list ----------------
                Vec3 selfPos{};
                bool selfOk = false;
                for (auto& e : ents)
                    if (e.self && e.prim &&
                        m.read(e.prim + (uint64_t)o.primitivePosition, selfPos) &&
                        std::isfinite(selfPos.x) && std::isfinite(selfPos.y) &&
                        std::isfinite(selfPos.z)) {
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
                        printf("tpdiag selfOk=%d self=%.2f,%.2f,%.2f rows=%d\n",
                               (int)selfOk, selfPos.x, selfPos.y, selfPos.z,
                               (int)rows.size());
                        for (size_t k = 0; k < rows.size() && k < 4; ++k) {
                            Ent& e2 = ents[rows[k].idx];
                            Vec3 pp{};
                            bool ok = m.read(e2.prim + (uint64_t)o.primitivePosition, pp);
                            printf("  tprow %-16s prim=%llx ok=%d pos=%.2f,%.2f,%.2f "
                                   "d=%.2f\n",
                                   e2.tag.c_str(), (unsigned long long)e2.prim,
                                   (int)ok, pp.x, pp.y, pp.z, rows[k].d);
                        }
                        fflush(stdout);
                    }
                }

                if (chkInt("NPCs", &cfgTpNpc))
                    upsertCfgKey("tp.npc", cfgTpNpc ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                ImGui::TextDisabled("%d listed  -  click TP", (int)rows.size());

                ImGui::BeginChild("##tplist", ImVec2(324.0f, 200.0f), true);
                {
                    // The whole list area must take the mouse: otherwise the
                    // wheel sits on an unmarked pixel, falls through to the
                    // game and zooms the camera instead of scrolling the list.
                    ImVec2 cp = ImGui::GetWindowPos();
                    ImVec2 cs = ImGui::GetWindowSize();
                    g_menuHits.push_back({ cp.x, cp.y, cp.x + cs.x, cp.y + cs.y });
                }
                if (rows.empty())
                    ImGui::TextDisabled("(no players in this server yet)");
                for (auto& r : rows) {
                    Ent& e = ents[r.idx];
                    ImGui::PushID((const void*)(uintptr_t)e.prim);
                    bool canTp = !e.self && haveWrite;
                    if (!canTp) ImGui::BeginDisabled();
                    if (ImGui::Button(e.self ? "you" : "TP", ImVec2(36.0f, 0.0f)) &&
                        canTp) {
                        g_tpTargetPrim = e.prim; // the loop does the write
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
                // ---- waypoints (Đợt 1-C): save here / TP / delete -----------
                ImGui::Separator();
                if (ImGui::Button("save waypoint", ImVec2(120.0f, 0.0f))) {
                    Vec3 sp{};
                    bool ok = false;
                    for (auto& e : ents) {
                        if (!e.self || !e.prim) continue;
                        if (m.read(e.prim + (uint64_t)o.primitivePosition, sp) &&
                            std::isfinite(sp.x) && std::isfinite(sp.y) &&
                            std::isfinite(sp.z))
                            ok = true;
                        break;
                    }
                    if (!ok)
                        toast("wp: self not found");
                    else if (g_ways.size() >= 64)
                        toast("wp: list full (64)");
                    else {
                        Waypoint w{};
                        char nm[24];
                        snprintf(nm, sizeof nm, "wp%02d", (int)g_ways.size() + 1);
                        w.name = nm;
                        w.p = sp;
                        g_ways.push_back(w);
                        saveWaypoints();
                        toast("saved %s", nm);
                    }
                }
                hitMark();
                ImGui::SameLine();
                if (ImGui::Button("clear all", ImVec2(90.0f, 0.0f))) {
                    g_ways.clear();
                    saveWaypoints();
                    toast("waypoints cleared");
                }
                hitMark();
                ImGui::SameLine();
                ImGui::TextDisabled("%d saved", (int)g_ways.size());
                ImGui::BeginChild("##wplist", ImVec2(324.0f, 120.0f), true);
                {
                    ImVec2 cp = ImGui::GetWindowPos();
                    ImVec2 cs = ImGui::GetWindowSize();
                    g_menuHits.push_back({ cp.x, cp.y, cp.x + cs.x, cp.y + cs.y });
                }
                if (g_ways.empty())
                    ImGui::TextDisabled("(none yet - stand somewhere");
                if (g_ways.empty()) ImGui::TextDisabled(" and hit save waypoint)");
                {
                    int wpDel = -1;
                    for (size_t i = 0; i < g_ways.size(); ++i) {
                        ImGui::PushID(1000 + (int)i);
                        bool canTp = haveWrite;
                        if (!canTp) ImGui::BeginDisabled();
                        if (ImGui::Button("TP", ImVec2(36.0f, 0.0f)) && canTp) {
                            g_tpTargetPos = g_ways[i].p; // the loop writes
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
                        ImGui::Text("%s  %.0f,%.0f,%.0f",
                                    g_ways[i].name.c_str(), g_ways[i].p.x,
                                    g_ways[i].p.y, g_ways[i].p.z);
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
