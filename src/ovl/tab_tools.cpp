#include "ovl_tabs.h"
#include "ovl_ui.h"
#include "ovl_state.h"
#include "imgui.h"

void drawTabTools(MenuCtx& c) {
    (void)c;
                // ---------------- Tools: recon ---------------------------------
                ImGui::BeginChild("##tools", ImVec2(0.0f, 230.0f), true);
                markChildArea();
                ImGui::TextDisabled("Instance explorer, saveinstance-lite,");
                ImGui::TextDisabled("memory scanner, macro recorder: Đợt 6.");
                ImGui::EndChild();
}
