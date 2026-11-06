#include "ovl_tabs.h"
#include "ovl_ui.h"
#include "ovl_state.h"
#include "ovl_fly.h"
#include "imgui.h"

void drawTabBody(MenuCtx& c) {
    Memory& m = c.m;
    const Offsets& o = c.o;
    const std::vector<Ent>& ents = c.ents;
    uint64_t place = c.place;
    bool haveWrite = c.haveWrite;
    bool aimOn = c.aimOn, trigOn = c.trigOn, acOn = c.acOn;
    bool flyApply = c.flyApply, speedApply = c.speedApply, jumpApply = c.jumpApply;
    int dbgEnts = c.dbgEnts;
    float loopMs = c.loopMs;
    DWORD nowLoop = c.nowTick;
                // ---------------- Body: movement ----------------
                ImGui::Text("Movement");
                if (chkInt("Fly", &cfgFlyEnabled)) {
                    if (!cfgFlyEnabled) g_flyActive = 0; // untick = off, F is dead
                    upsertCfgKey("fly.enabled", cfgFlyEnabled ? "1" : "0");
                }
                hitMark();
                ImGui::SameLine();
                sliderFloatCfg("##flyspeed", &cfgFlySpeed, 5.0f, 400.0f, "%.0f u/s",
                               "fly.speed");
                hitMark();
                ImGui::SameLine();
                keyStateLabel(cfgFlyKey, flyApply);
                // Writer telemetry: fly is held by a dedicated high-rate
                // thread (ovl_fly.cpp). If Fly+key are on and this stays
                // at 0, the writer never got the character primitive.
                if (Fly::active()) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                                       "%.2fM w/s",
                                       Fly::writesPerSec() / 1e6f);
                }

                if (chkInt("Speed", &cfgPlySpeed)) {
                    if (!cfgPlySpeed) g_speedActive = 0;
                    upsertCfgKey("ply.speed", cfgPlySpeed ? "1" : "0");
                }
                hitMark();
                ImGui::SameLine();
                sliderFloatCfg("##speedval", &cfgPlySpeedVal, 0.0f, 500.0f, "%.0f ws",
                               "ply.speedval");
                hitMark();
                ImGui::SameLine();
                keyStateLabel(cfgPlySpeedKey, speedApply);
                ImGui::Indent(12.0f);
                {
                    // click to cycle: a combo popup would draw outside the panel,
                    // and outside the panel every click belongs to the game
                    char lbl[48];
                    snprintf(lbl, sizeof lbl, "impl: %s##pm",
                             cfgPlyMode == 0 ? "property" : "velocity");
                    if (ImGui::Button(lbl, ImVec2(112.0f, 0.0f))) {
                        cfgPlyMode = cfgPlyMode ? 0 : 1;
                        char b[8];
                        snprintf(b, sizeof b, "%d", cfgPlyMode);
                        upsertCfgKey("ply.mode", b);
                    }
                    hitMark();
                    ImGui::SameLine();
                    if (speedApply)
                        ImGui::TextDisabled("actual %.2f", g_plyCurSpeed);
                    else if (cfgPlySpeed)
                        ImGui::TextDisabled("press %s",
                                            keyName(cfgPlySpeedKey).c_str());
                    else
                        ImGui::TextDisabled("unticked = %s inert",
                                            keyName(cfgPlySpeedKey).c_str());
                }
                ImGui::Unindent(12.0f);

                if (chkInt("Jump", &cfgPlyJump)) {
                    if (!cfgPlyJump) g_jumpActive = 0;
                    upsertCfgKey("ply.jump", cfgPlyJump ? "1" : "0");
                }
                hitMark();
                ImGui::SameLine();
                sliderFloatCfg("##jumpval", &cfgPlyJumpVal, 0.0f, 300.0f, "%.0f jp",
                               "ply.jumpval");
                hitMark();
                ImGui::SameLine();
                keyStateLabel(cfgPlyJumpKey, jumpApply);
                ImGui::Indent(12.0f);
                {
                    static const char* jmName[3] = { "auto", "power", "velocity" };
                    int jmi = (cfgPlyJumpMode >= 0 && cfgPlyJumpMode < 3)
                                  ? cfgPlyJumpMode
                                  : 0;
                    char lbl[48];
                    snprintf(lbl, sizeof lbl, "impl: %s##jm", jmName[jmi]);
                    if (ImGui::Button(lbl, ImVec2(112.0f, 0.0f))) {
                        cfgPlyJumpMode = (jmi + 1) % 3;
                        char b[8];
                        snprintf(b, sizeof b, "%d", cfgPlyJumpMode);
                        upsertCfgKey("ply.jumpmode", b);
                    }
                    hitMark();
                    ImGui::SameLine();
                    uint64_t selfHum = 0;
                    for (auto& e : ents)
                        if (e.self) { selfHum = e.humanoid; break; }
                    float jh = 0.0f, jp = 0.0f;
                    unsigned char ujp = 0;
                    bool ok = selfHum != 0;
                    if (ok && o.humanoidJumpHeight)
                        ok = m.read(selfHum + (uint64_t)o.humanoidJumpHeight, jh);
                    if (ok && o.humanoidJumpPower)
                        ok = m.read(selfHum + (uint64_t)o.humanoidJumpPower, jp);
                    if (ok && o.humanoidUseJumpPower)
                        ok = m.read(selfHum + (uint64_t)o.humanoidUseJumpPower, ujp);
                    if (ok)
                        ImGui::TextDisabled("power %.0f  height %.2f  UJP=%d", jp, jh,
                                            (int)ujp);
                    else
                        ImGui::TextDisabled("(no character yet)");
                }
                ImGui::Unindent(12.0f);
}
