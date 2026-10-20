#include "ovl_tabs.h"
#include "ovl_ui.h"
#include "ovl_state.h"
#include "imgui.h"

void drawTabCombat(MenuCtx& c) {
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
                // ---------------- Combat ----------------
                if (chkInt("Aim", &cfgAimEnabled))
                    upsertCfgKey("aim.enabled", cfgAimEnabled ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                sliderIntCfg("##aimfov", &cfgAimFov, 10, 800, "fov %d", "aim.fov");
                hitMark();
                ImGui::SameLine();
                keyStateLabel(cfgAimKey, aimOn && cfgAimEnabled);

                // velext smoothing: tau = 8 ms * smoothing (<=1 snaps)
                ImGui::TextUnformatted("Smoothing");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(130);
                ImGui::SliderFloat("##aimsmooth", &cfgAimSmoothing, 1.0f, 20.0f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    char sb[32];
                    snprintf(sb, sizeof sb, "%.2f", cfgAimSmoothing);
                    upsertCfgKey("aim.smoothing", sb);
                }
                hitMark();
                ImGui::SameLine();
                {
                    float a60 = velextAlphaFromDt(cfgAimSmoothing, 1.0f / 60.0f);
                    if (a60 >= 1.0f)
                        ImGui::TextDisabled("velext: snap");
                    else
                        ImGui::TextDisabled("velext tau=%.0fms a60=%.3f",
                                            8.0f * cfgAimSmoothing, a60);
                }

                if (chkInt("Trigger", &cfgTrigEnabled))
                    upsertCfgKey("trig.enabled", cfgTrigEnabled ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                keyStateLabel(cfgTrigKey, trigOn && cfgTrigEnabled);
                ImGui::SameLine();
                ImGui::TextDisabled("%s", cfgTrigKeyMode ? "toggle" : "hold");

                if (chkInt("AutoClick", &cfgACEnabled))
                    upsertCfgKey("autoclick.enabled", cfgACEnabled ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                keyStateLabel(cfgACKey, acOn && cfgACEnabled);

                if (chkInt("Hitbox", &cfgHitboxEnabled))
                    upsertCfgKey("hitbox.enabled", cfgHitboxEnabled ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                if (haveWrite)
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "mem ON");
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "mem OFF");
}
