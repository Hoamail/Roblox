#include "ovl_tabs.h"
#include "ovl_ui.h"
#include "ovl_state.h"
#include "imgui.h"

void drawTabVisual(MenuCtx& c) {
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
                // ---------------- Visual: ESP v2 + radar ----------------
                ImGui::BeginChild("##visual", ImVec2(0.0f, 400.0f), true);
                markChildArea();
                g_colHover = nullptr;
                auto espChk = [&](const char* lbl, int* v, const char* key) {
                    if (chkInt(lbl, v)) upsertCfgKey(key, *v ? "1" : "0");
                    hitMark();
                };
                espChk("ESP on", &cfgEspOn, "esp.on");
                ImGui::SameLine();
                ImGui::TextDisabled("(master)");
                espChk("box", &cfgEspBox, "esp.box");
                ImGui::SameLine();
                espChk("fill", &cfgEspFill, "esp.fill");
                ImGui::SameLine();
                espChk("name", &cfgEspName, "esp.name");
                espChk("distance", &cfgEspDist, "esp.dist");
                ImGui::SameLine();
                espChk("hp bar", &cfgEspHp, "esp.hp");
                ImGui::SameLine();
                espChk("head dot", &cfgEspHead, "esp.head");
                espChk("skeleton", &cfgEspSkel, "esp.skel");
                ImGui::SameLine();
                espChk("snapline", &cfgEspLine, "esp.line");
                ImGui::SameLine();
                espChk("offscr", &cfgEspOof, "esp.oof");
                espChk("NPCs", &cfgEspNpc, "esp.npc");
                ImGui::SameLine();
                sliderIntCfg("max dist", &cfgEspMaxDist, 0, 3000, "%d m",
                             "esp.maxdist");
                hitMark();
                ImGui::Separator();
                ImGui::Text("colours  (click a bar to cycle)");
                // The ImGui colour picker is a popup and popups cannot be
                // dismissed here (no keyboard focus, and clicks outside the panel
                // fall through to the game), so the colours cycle through a fixed
                // palette instead. The swatch IS the button.
                auto colBtn = [&](const char* lbl, int* v, const char* key) {
                    const int n = (int)(sizeof(kColorPal) / sizeof(kColorPal[0]));
                    int idx = -1;
                    for (int i = 0; i < n; ++i)
                        if (kColorPal[i] == *v) {
                            idx = i;
                            break;
                        }
                    float rr = (float)((*v >> 16) & 0xFF) / 255.0f;
                    float gg = (float)((*v >> 8) & 0xFF) / 255.0f;
                    float bb = (float)(*v & 0xFF) / 255.0f;
                    float lum = 0.299f * rr + 0.587f * gg + 0.114f * bb;
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(rr, gg, bb, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          ImVec4(rr, gg, bb, 0.72f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                          ImVec4(rr, gg, bb, 0.55f));
                    // The overlay is colour-keyed on PURE BLACK (SetLayeredWindowAttributes
                    // RGB(0,0,0) + LWA_COLORKEY): every pixel that ends up exactly
                    // (0,0,0) is not only invisible but also click-through at the OS
                    // level - the click never even reaches our window. So text drawn
                    // over a bright bar must be a near-black grey, never real black.
                    ImGui::PushStyleColor(ImGuiCol_Text, lum > 0.55f
                                                             ? ImVec4(0.06f, 0.06f, 0.06f, 1)
                                                             : ImVec4(1, 1, 1, 1));
                    bool pr = ImGui::Button(lbl, ImVec2(150.0f, 19.0f));
                    if (ImGui::IsItemHovered()) g_colHover = lbl;
                    ImGui::PopStyleColor(4);
                    hitMark();
                    if (pr) {
                        *v = kColorPal[(idx + 1) % n];
                        char b[16];
                        snprintf(b, sizeof b, "%d", *v);
                        upsertCfgKey(key, b);
                        if (g_diagState) {
                            printf("colclick %s idx=%d -> %d\n", lbl, idx, *v);
                            fflush(stdout);
                        }
                    }
                };
                colBtn("enemy", &cfgEspColEnemy, "esp.colenemy");
                colBtn("team", &cfgEspColTeam, "esp.colteam");
                colBtn("npc", &cfgEspColNpc, "esp.colnpc");
                ImGui::Separator();
                if (chkInt("radar", &cfgRadarOn))
                    upsertCfgKey("radar.on", cfgRadarOn ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                sliderIntCfg("range", &cfgRadarRange, 50, 2000, "%dm", "radar.range");
                hitMark();
                ImGui::SameLine();
                sliderIntCfg("size", &cfgRadarSize, 80, 260, "%dpx", "radar.size");
                hitMark();
                ImGui::EndChild();
}
