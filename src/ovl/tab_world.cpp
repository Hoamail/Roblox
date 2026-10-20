#include "ovl_tabs.h"
#include "ovl_ui.h"
#include "ovl_state.h"
#include "imgui.h"

void drawTabWorld(MenuCtx& c) {
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
                // ---------------- World: lighting / atmosphere / terrain -------
                // Client-local render state only: nothing here replicates, and
                // unticking writes the saved originals back (worldTick restores).
                ImGui::BeginChild("##world", ImVec2(0.0f, 330.0f), true);
                markChildArea();
                if (chkInt("Fullbright", &cfgWorldFull))
                    upsertCfgKey("world.full", cfgWorldFull ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                sliderFloatCfg("##wbright", &cfgWorldBright, 1.0f, 3.0f,
                               "bright %.2f", "world.bright");
                hitMark();
                ImGui::SameLine();
                ImGui::TextDisabled("+ white ambient");

                if (chkInt("Anti-fog", &cfgWorldFog))
                    upsertCfgKey("world.fog", cfgWorldFog ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                ImGui::TextDisabled("fogStart/end = 1e5");

                if (chkInt("No shadows", &cfgWorldShadows))
                    upsertCfgKey("world.shadows", cfgWorldShadows ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                ImGui::TextDisabled("GlobalShadows = 0");

                if (chkInt("Exposure", &cfgWorldExpo))
                    upsertCfgKey("world.expo", cfgWorldExpo ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                sliderFloatCfg("##wexpo", &cfgWorldExpoVal, -2.0f, 2.0f, "%.2f",
                               "world.expoval");
                hitMark();

                if (chkInt("Time", &cfgWorldClock))
                    upsertCfgKey("world.clock", cfgWorldClock ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                sliderFloatCfg("##wclock", &cfgWorldClockVal, 0.0f, 24.0f,
                               "%.1f h", "world.clockval");
                hitMark();

                if (chkInt("Bloom/blur/CC off", &cfgWorldFx))
                    upsertCfgKey("world.fx", cfgWorldFx ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                ImGui::TextDisabled("effects under Lighting/Camera");

                if (chkInt("Atmosphere off", &cfgWorldAtmo))
                    upsertCfgKey("world.atmo", cfgWorldAtmo ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                ImGui::TextDisabled("density/haze/glare = 0");

                if (chkInt("Water clear/flat", &cfgWorldWater))
                    upsertCfgKey("world.water", cfgWorldWater ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                ImGui::TextDisabled("transp 1, waves 0, refl 0");

                if (chkInt("No grass", &cfgWorldGrass))
                    upsertCfgKey("world.grass", cfgWorldGrass ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                ImGui::TextDisabled("GrassLength = 0");

                if (chkInt("FOV", &cfgWorldFov))
                    upsertCfgKey("world.fovon", cfgWorldFov ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                sliderFloatCfg("##wfov", &cfgWorldFovVal, 70.0f, 120.0f, "%.0f deg",
                               "world.fov");
                hitMark();

                if (chkInt("Far zoom", &cfgWorldZoom))
                    upsertCfgKey("world.zoom", cfgWorldZoom ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                ImGui::TextDisabled("zoom 0.5 .. 2000");

                ImGui::Separator();
                if (wLighting)
                    ImGui::TextDisabled("Lighting 0x%llx  Terrain 0x%llx",
                                        (unsigned long long)wLighting,
                                        (unsigned long long)wTerrain);
                else
                    ImGui::TextDisabled("Lighting: not found (game running?)");
                ImGui::TextDisabled("atmo %d  bloom %d  blur %d  cc %d  player %d",
                                    wAtmosphere ? 1 : 0, wBloom ? 1 : 0,
                                    wBlur ? 1 : 0, wCC ? 1 : 0, wLocalPlr ? 1 : 0);
                ImGui::EndChild();
}
