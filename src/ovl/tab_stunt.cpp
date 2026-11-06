#include "ovl_tabs.h"
#include "ovl_ui.h"
#include "ovl_state.h"
#include "ovl_stunt.h"
#include "imgui.h"
#include <cstdio>

// Stunt tab (index 8). Read-only view of the state that ovl_stunt owns: the
// tab only sets cfg / g_stuntTargetPrim, the main loop hands them to the
// writer thread (same rule as Fly in tab_body.cpp).
void drawTabStunt(MenuCtx& c) {
    Memory& m = c.m;
    const Offsets& o = c.o;
    std::vector<Ent>& ents = c.ents;
    bool haveWrite = c.haveWrite;
    DWORD nowLoop = c.nowTick;

    const int n = Stunt::presetCount();
    if (cfgStuntPreset < 0 || cfgStuntPreset >= n) cfgStuntPreset = 0;

    ImGui::Text("Visible stunts");
    // Label must differ from the "Stunt" tab button: same label in the same
    // window = same ImGui ID ("2 visible items with conflicting ID").
    if (chkInt("Stunt##stunton", &cfgStuntEnabled)) {
        if (!cfgStuntEnabled) g_stuntActive = 0; // untick = off, key is dead
        upsertCfgKey("stunt.enabled", cfgStuntEnabled ? "1" : "0");
    }
    hitMark();
    ImGui::SameLine();
    keyStateLabel(cfgStuntKey, g_stuntActive != 0);
    ImGui::SameLine();
    if (Stunt::active()) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "RUN %.2fM w/s",
                           Stunt::writesPerSec() / 1e6f);
    } else {
        ImGui::TextDisabled("idle");
    }

    // ---- preset grid (4 per row, highlight on the active one) --------------
    for (int i = 0; i < n; ++i) {
        if (i % 4) ImGui::SameLine();
        ImGui::PushID(i);
        const bool sel = (cfgStuntPreset == i);
        if (sel)
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.26f, 0.59f, 0.98f, 1.0f));
        if (ImGui::Button(Stunt::presetName(i), ImVec2(76.0f, 0.0f))) {
            cfgStuntPreset = i;
            char b[8];
            snprintf(b, sizeof b, "%d", i);
            upsertCfgKey("stunt.preset", b);
        }
        if (sel) ImGui::PopStyleColor();
        hitMark();
        ImGui::PopID();
    }

    ImGui::Separator();
    sliderFloatCfg("speed##st", &cfgStuntSpeed, 0.25f, 2.0f, "%.2fx", "stunt.speed");
    hitMark();
    ImGui::SameLine();
    sliderFloatCfg("amp##st", &cfgStuntAmp, 0.1f, 3.0f, "%.2f", "stunt.amp");
    hitMark();

    if (chkInt("ragdoll##st", &cfgStuntRagdoll))
        upsertCfgKey("stunt.ragdoll", cfgStuntRagdoll ? "1" : "0");
    hitMark();
    ImGui::SameLine();
    if (chkInt("loop##st", &cfgStuntLoop))
        upsertCfgKey("stunt.loop", cfgStuntLoop ? "1" : "0");
    hitMark();
    ImGui::SameLine();
    if (chkInt("hover##st", &cfgStuntHover))
        upsertCfgKey("stunt.hover", cfgStuntHover ? "1" : "0");
    hitMark();

    // Runtime A/B switch (stunt.mode): dynamic = let the solver integrate the
    // angular velocity, kinematic = we integrate it and write the matrix.
    {
        char lbl[48];
        snprintf(lbl, sizeof lbl, "mode: %s##stmode",
                 cfgStuntMode ? "kinematic" : "dynamic");
        if (ImGui::Button(lbl, ImVec2(140.0f, 0.0f))) {
            cfgStuntMode = cfgStuntMode ? 0 : 1;
            char b[8];
            snprintf(b, sizeof b, "%d", cfgStuntMode);
            upsertCfgKey("stunt.mode", b);
        }
        hitMark();
        ImGui::SameLine();
        keyBindWidget("key", &cfgStuntKey, "stunt.key", 90.0f);
        hitMark();
    }

    // ---- target (piggyback / orbit / tornado) ------------------------------
    const bool needTarget =
        (cfgStuntPreset == 8 || cfgStuntPreset == 9 || cfgStuntPreset == 15);
    if (needTarget) {
        uint64_t cur = g_stuntTargetPrim;
        const char* curName = "(none)";
        for (auto& e : ents)
            if (e.prim && e.prim == cur) {
                curName = e.tag.c_str();
                break;
            }
        ImGui::TextDisabled("target: %s", curName);
        ImGui::BeginChild("##stunt_tgt", ImVec2(324.0f, 92.0f), true);
        {
            ImVec2 cp = ImGui::GetWindowPos();
            ImVec2 cs = ImGui::GetWindowSize();
            g_menuHits.push_back({ cp.x, cp.y, cp.x + cs.x, cp.y + cs.y });
        }
        int listed = 0;
        for (auto& e : ents) {
            if (!e.prim || !e.player || e.self) continue;
            ++listed;
            ImGui::PushID((const void*)(uintptr_t)e.prim);
            const bool sel = (g_stuntTargetPrim == e.prim);
            if (sel)
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.26f, 0.59f, 0.98f, 1.0f));
            if (ImGui::Button(e.tag.c_str(), ImVec2(150.0f, 0.0f)))
                g_stuntTargetPrim = e.prim;
            if (sel) ImGui::PopStyleColor();
            hitMark();
            ImGui::PopID();
        }
        if (!listed) ImGui::TextDisabled("(no other players yet)");
        ImGui::EndChild();
    }

    // ---- run control -------------------------------------------------------
    // key=0 (the default) means hold-to-run, so the tab is usable without
    // binding a key that could collide with a macro sweep.
    if (cfgStuntKey == 0) {
        ImGui::Button("Run (hold)##stunt", ImVec2(120.0f, 0.0f));
        g_stuntHold = ImGui::IsItemActive() ? 1 : 0;
        hitMark();
        ImGui::SameLine();
        ImGui::TextDisabled(g_stuntActive ? "held" : "hold to run");
    } else {
        ImGui::TextDisabled("press %s to toggle", keyName(cfgStuntKey).c_str());
    }

    // ---- status ------------------------------------------------------------
    const bool ready = haveWrite;
    int sub = Stunt::presetRuntime();
    if (Stunt::active() && sub >= 0 && sub < n)
        ImGui::TextDisabled("segment: %s", Stunt::presetName(sub));
    else if (!ready)
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "read-only: no write handle (version mismatch?)");
    else if (!o.primitiveRotation || !o.primitiveAngularVelocity ||
             !o.humanoidPlatformStand)
        ImGui::TextDisabled("note: rotation/angular offsets missing in this dump "
                            "- kinematic presets degraded");
    if (Stunt::active())
        ImGui::TextDisabled("teleport disabled while a stunt runs");
    (void)m;
    (void)nowLoop;
}
