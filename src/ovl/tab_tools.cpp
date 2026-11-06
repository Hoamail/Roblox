#include "ovl_tabs.h"
#include "ovl_ui.h"
#include "ovl_state.h"
#include "ovl_tools.h"
#include "imgui.h"

void drawTabTools(MenuCtx& c) {
    ToolState s = toolsState();
    ImGui::BeginChild("##tools", ImVec2(0.0f, 330.0f), true);
    markChildArea();
    ImGui::Text("Instance Explorer (read-only)");
    if (ImGui::Button("Refresh root")) toolsRequestRefresh();
    hitMark();
    ImGui::SameLine();
    ImGui::TextDisabled("generation %llu", (unsigned long long)s.generation);
    ImGui::TextDisabled("%s", s.status.empty() ? "idle" : s.status.c_str());
    if (ImGui::Button(s.dumpRunning ? "Cancel dump" : "Save JSON")) {
        if (s.dumpRunning) toolsCancelDump();
        else toolsRequestDump(c.m, c.o);
    }
    hitMark();
    ImGui::SameLine();
    ImGui::TextDisabled("dump nodes %d", s.dumpNodes);
    if (ImGui::Button(s.macroRecording ? "Stop macro" : "Record macro")) {
        if (s.macroRecording) toolsMacroStop();
        else toolsMacroStart();
    }
    hitMark();
    ImGui::SameLine();
    if (ImGui::Button(s.macroPlaying ? "Cancel macro" : "Play macro")) {
        if (s.macroPlaying) toolsMacroCancel();
        else toolsMacroPlay();
    }
    hitMark();
    ImGui::SameLine();
    ImGui::TextDisabled("macro %d events, %lums", s.macroEvents,
                        (unsigned long)s.macroElapsed);
    ImGui::Separator();
    for (const ToolNode& n : s.nodes) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + n.depth * 12.0f);
        // "##<address>" is mandatory, not cosmetic: two instances can share the
        // exact same name/class/child-count (e.g. two unnamed parts), and an
        // identical label gives both rows the same ImGui ID - the second one
        // then fights the first for hover/selection and ImGui pops the
        // "2 visible items with conflicting ID" error box.
        char label[256];
        snprintf(label, sizeof label, "%s %s  [%d]##%llx", n.name.c_str(),
                 n.className.c_str(), n.childCount,
                 (unsigned long long)n.address);
        // A Selectable with no explicit width claims the whole row, including
        // the pixels the SmallButton is drawn on. Without AllowOverlap the
        // Selectable grabs HoveredId first and ImGui::ItemHoverable() then
        // rejects the button ("g.HoveredId != id && !HoveredIdAllowOverlap"),
        // so the button never hovers and never fires - the click looks dead.
        if (ImGui::Selectable(label, s.selectedInstance == n.address,
                              ImGuiSelectableFlags_AllowOverlap))
            toolsSelect(n.address);
        hitMark();
        if (n.childCount > 0) {
            ImGui::SameLine();
            char b[64];
            snprintf(b, sizeof b, "%s##%llx", n.expanded ? "collapse" : "expand",
                     (unsigned long long)n.address);
            if (ImGui::SmallButton(b)) toolsRequestChildren(n.address);
            hitMark();
        }
    }
    ImGui::EndChild();
}
