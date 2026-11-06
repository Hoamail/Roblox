#include "ovl_tabs.h"
#include "ovl_ui.h"
#include "ovl_state.h"
#include "imgui.h"

void drawTabServer(MenuCtx& c) {
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
                // ---------------- Server: session / hop / watchdog ------------
                // Info comes from srvTick (1 Hz RPM, no game writes); the list
                // is a snapshot of the worker's last fetch. Buttons only queue
                // requests or open a URI - nothing here blocks the loop.
                ImGui::BeginChild("##server", ImVec2(0.0f, 330.0f), true);
                markChildArea();
                {
                    uint64_t showPlace = srv.place ? srv.place : place;
                    if (srv.gameId || srv.placeVersion)
                        ImGui::Text("place %llu  game %llu  v%d",
                                    (unsigned long long)showPlace,
                                    (unsigned long long)srv.gameId,
                                    srv.placeVersion);
                    else
                        ImGui::Text("place %llu", (unsigned long long)showPlace);
                    ImGui::TextDisabled("job %s  ip %s",
                                        srv.jobId.empty()
                                            ? "--"
                                            : (srvShort(srv.jobId) + "..").c_str(),
                                        srv.serverIP.empty()
                                            ? "--"
                                            : srv.serverIP.c_str());
                    char up[24] = "--";
                    if (srv.jobSeen && nowLoop >= srv.jobSeen) {
                        unsigned long secs = (nowLoop - srv.jobSeen) / 1000;
                        if (secs < 3600)
                            snprintf(up, sizeof up, "%lum%02lus", secs / 60,
                                     secs % 60);
                        else
                            snprintf(up, sizeof up, "%luh%02lum", secs / 3600,
                                     (secs % 3600) / 60);
                    }
                    char ping[16];
                    if (srv.ping >= 0) snprintf(ping, sizeof ping, "%dms", srv.ping);
                    else snprintf(ping, sizeof ping, "--");
                    if (cfgSrvGeo) {
                        std::string reg = srv.region.empty() ? ".." : srv.region;
                        if (reg.size() > 14) reg = reg.substr(0, 12) + "..";
                        ImGui::TextDisabled("players %d  up %s  ping %s  %s",
                                            srv.players, up, ping, reg.c_str());
                    } else {
                        ImGui::TextDisabled("players %d  up %s  ping %s",
                                            srv.players, up, ping);
                    }
                }
                ImGui::Separator();

                // Snapshot + hop filter once per frame (the list is <=100 rows).
                std::vector<SrvRow> all, filt;
                int lstate = 0, lcode = 0;
                {
                    std::lock_guard<std::mutex> lk(g_srvWire.mu);
                    all = g_srvWire.rows;
                    lstate = g_srvWire.listState;
                    lcode = g_srvWire.listCode;
                }
                filt = srvFilterRows(all, srv.jobId);
                if (g_srvSel >= (int)filt.size()) g_srvSel = -1;

                if (ImGui::Button("Refresh"))
                    srvRequestList(srv.place ? srv.place : place);
                hitMark();
                ImGui::SameLine();
                if (ImGui::Button("Join")) {
                    if (g_srvSel >= 0 && g_srvSel < (int)filt.size())
                        srvOpenJoin(srv.place, filt[g_srvSel].id, "join");
                    else
                        toast("select a server row first");
                }
                hitMark();
                ImGui::SameLine();
                if (ImGui::Button("Hop")) {
                    if (!filt.empty())
                        srvOpenJoin(srv.place, filt[0].id, "hop");
                    else
                        toast("no server to hop to - Refresh");
                }
                hitMark();
                ImGui::SameLine();
                if (ImGui::Button("Rejoin")) {
                    if (!srv.jobId.empty())
                        srvOpenJoin(srv.place, srv.jobId, "rejoin");
                    else
                        toast("no jobId yet");
                }
                hitMark();
                if (ImGui::Button("Paste & Join")) {
                    std::string clip;
                    SrvJoin jr;
                    if (!srvClipGet(clip))
                        toast("clipboard empty");
                    else if (!srvParseJoin(clip, srv.place ? srv.place : place, jr))
                        toast("clipboard is not a job/link");
                    else
                        srvOpenJoin(jr.place, jr.job, "paste");
                }
                hitMark();
                ImGui::SameLine();
                if (ImGui::Button("Copy jobId")) {
                    if (srv.jobId.empty())
                        toast("no jobId yet");
                    else
                        toast("%s", srvClipSet(srv.jobId) ? "jobId copied"
                                                         : "clipboard busy");
                }
                hitMark();
                ImGui::SameLine();
                if (ImGui::Button("Copy link")) {
                    if (srv.jobId.empty() || !srv.place)
                        toast("no jobId yet");
                    else
                        toast("%s", srvClipSet(srvBuildLink(srv.place, srv.jobId))
                                      ? "link copied"
                                      : "clipboard busy");
                }
                hitMark();

                ImGui::Separator();
                ImGui::BeginChild("##srvlist", ImVec2(0.0f, 100.0f), true);
                if (lstate == 1)
                    ImGui::TextDisabled("loading...");
                else if (lstate == 0)
                    ImGui::TextDisabled("no list yet - Refresh");
                else if (lstate == 3) {
                    if (lcode > 0)
                        ImGui::TextDisabled(
                            "failed: HTTP %d (old list kept; wait before retry)",
                            lcode);
                    else
                        ImGui::TextDisabled("failed: no response (old list kept)");
                } else if (filt.empty())
                    ImGui::TextDisabled("no public server matches");
                for (int i = 0; i < (int)filt.size(); ++i) {
                    char row[80];
                    snprintf(row, sizeof row, "%s  %d/%d  %dms",
                             srvShort(filt[i].id).c_str(), filt[i].playing,
                             filt[i].maxPlayers, filt[i].ping);
                    if (ImGui::Selectable(row, g_srvSel == i)) g_srvSel = i;
                    hitMark(); // every row: a missing mark clicks into the game
                }
                ImGui::EndChild();

                ImGui::Separator();
                sliderIntCfg("hop min", &cfgSrvHopMin, 0, 50, "%d ply",
                             "srv.hopmin");
                hitMark();
                ImGui::SameLine();
                if (chkInt("fresh", &cfgSrvHopFresh))
                    upsertCfgKey("srv.hopfresh", cfgSrvHopFresh ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                if (chkInt("geo", &cfgSrvGeo))
                    upsertCfgKey("srv.geo", cfgSrvGeo ? "1" : "0");
                hitMark();
                if (chkInt("auto-rejoin", &cfgSrvAutoRejoin))
                    upsertCfgKey("srv.autorejoin", cfgSrvAutoRejoin ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                if (chkInt("anti-AFK", &cfgSrvAntiAfk))
                    upsertCfgKey("srv.antiafk", cfgSrvAntiAfk ? "1" : "0");
                hitMark();
                ImGui::SameLine();
                sliderIntCfg("##afkmin", &cfgSrvAfkMin, 5, 15, "%d min",
                             "srv.afkmin");
                hitMark();
                ImGui::EndChild();
}
