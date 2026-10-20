#include "ovl_tabs.h"
#include "ovl_ui.h"
#include "ovl_state.h"
#include "imgui.h"

void drawTabMisc(MenuCtx& c) {
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
                // ---------------- Misc ----------------
                ImGui::BeginChild("##misc", ImVec2(0.0f, 400.0f), true);
                markChildArea();
                if (chkInt("watermark HUD", &cfgHudOn))
                    upsertCfgKey("hud.on", cfgHudOn ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                if (chkInt("feature list", &cfgHudFeats))
                    upsertCfgKey("hud.feats", cfgHudFeats ? "1" : "0");
                hitMark();
                if (chkInt("stream proof", &cfgWda))
                    upsertCfgKey("misc.wda", cfgWda ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                ImGui::TextDisabled("(hidden from capture)");
                if (chkInt("unlock mouse", &cfgMenuUnlockMouse))
                    upsertCfgKey("menu.unlockmouse",
                                 cfgMenuUnlockMouse ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                ImGui::TextDisabled("(game pins it while playing)");

                ImGui::Separator();
                ImGui::Text("Keys  (click, press a key; Esc = unbind)");
                keyBindWidget("panel", &g_toggleVk, "toggle");
                hitMark();
                ImGui::SameLine();
                keyBindWidget("panic", &cfgPanicKey, "panic.key");
                hitMark();
                keyBindWidget("aim", &cfgAimKey, "aim.key");
                hitMark();
                ImGui::SameLine();
                keyBindWidget("trigger", &cfgTrigKey, "trig.key");
                hitMark();
                keyBindWidget("autoclick", &cfgACKey, "autoclick.key");
                hitMark();
                ImGui::SameLine();
                keyBindWidget("fly", &cfgFlyKey, "fly.key");
                hitMark();
                keyBindWidget("speed", &cfgPlySpeedKey, "ply.speedkey");
                hitMark();
                ImGui::SameLine();
                keyBindWidget("jump", &cfgPlyJumpKey, "ply.jumpkey");
                hitMark();

                ImGui::Separator();
                ImGui::Text("Profiles  (whole cfg per slot)");
                static int profSel = 1;
                for (int i = 1; i <= 5; ++i) {
                    if (i > 1) ImGui::SameLine();
                    char nm[8];
                    snprintf(nm, sizeof nm, "p%d", i);
                    bool sel = (profSel == i);
                    if (sel)
                        ImGui::PushStyleColor(ImGuiCol_Button,
                                              ImVec4(0.26f, 0.59f, 0.98f, 1.0f));
                    if (ImGui::Button(nm, ImVec2(34.0f, 0.0f))) profSel = i;
                    if (sel) ImGui::PopStyleColor();
                    hitMark();
                }
                char slotName[8];
                snprintf(slotName, sizeof slotName, "p%d", profSel);
                bool haveSlot = profileExists(slotName);
                if (ImGui::Button("save", ImVec2(60.0f, 0.0f))) {
                    if (saveProfileSlot(slotName)) toast("saved %s", slotName);
                    else toast("save failed");
                }
                hitMark();
                ImGui::SameLine();
                if (!haveSlot) ImGui::BeginDisabled();
                if (ImGui::Button("load", ImVec2(60.0f, 0.0f))) {
                    if (loadProfileSlot(slotName)) toast("loaded %s", slotName);
                }
                hitMark();
                ImGui::SameLine();
                if (ImGui::Button("bind place", ImVec2(88.0f, 0.0f))) {
                    setPlaceProfile(place, slotName);
                    toast("%s <- place %llu", slotName, (unsigned long long)place);
                }
                hitMark();
                if (!haveSlot) ImGui::EndDisabled();
                if (chkInt("auto load by place", &cfgAutoProfile))
                    upsertCfgKey("profile.auto", cfgAutoProfile ? "1" : "0");
                hitMark();

                ImGui::Separator();
                ImGui::Text("build  %s", o.robloxVersion.c_str());
                ImGui::Text("place  %llu", (unsigned long long)place);
                ImGui::Text("fps    %.0f   loop %.1fms", ImGui::GetIO().Framerate,
                            loopMs);
                ImGui::Text("ents   %d   %s", dbgEnts,
                            haveWrite ? "mem ON" : "mem OFF");
                if (ImGui::Button("reset panel position", ImVec2(180.0f, 0.0f))) {
                    cfgMenuX = 60.0f;
                    cfgMenuY = 120.0f;
                    g_menuPosForce = true;
                    upsertCfgKey("menu.pos", "60,120");
                }
                hitMark();
                ImGui::EndChild();
}
