// ESP / radar, moved verbatim from overlay.cpp (S3.2).
#include "ovl_esp.h"
#include "ovl_view.h"
#include "ovl_state.h" // cfgEsp* / cfgRadar* / cfgHudOn
#include "imgui.h"
#include <cmath>
#include <cstdio>

void EspCtx::draw(ImDrawList* dl, const float* vm) {
    uint64_t espLocalTeam = 0;
    Vec3 espSelf{};
    bool espSelfOk = false;
    for (auto& e : ents) {
        if (!e.self) continue;
        espLocalTeam = e.team;
        if (m.read(e.prim + (uint64_t)o.primitivePosition, espSelf) &&
            std::isfinite(espSelf.x) && std::isfinite(espSelf.y) &&
            std::isfinite(espSelf.z))
            espSelfOk = true;
        break;
    }
    auto rgb = [](int c) {
        return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
    };
    // A primitive's position, projected (skeleton + head dot).
    auto w2sPrim = [&](uint64_t prim, int& px, int& py) {
        if (!prim) return false;
        Vec3 p{};
        if (!m.read(prim + (uint64_t)o.primitivePosition, p)) return false;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
            !std::isfinite(p.z))
            return false;
        return w2s(p, vm, px, py);
    };
    // Projection that stays usable when the point is off screen or behind
    // the camera: the off-screen arrows need a direction, not a pixel.
    auto w2sRay = [&](const Vec3& wp, float& nx, float& ny) {
        float x = vm[0] * wp.x + vm[1] * wp.y + vm[2] * wp.z + vm[3];
        float y = vm[4] * wp.x + vm[5] * wp.y + vm[6] * wp.z + vm[7];
        float ww = vm[12] * wp.x + vm[13] * wp.y + vm[14] * wp.z + vm[15];
        bool front = (ww > 0.1f);
        float wc = front ? ww : 0.1f;
        nx = x / wc;
        ny = y / wc;
        if (!front) {
            nx = -nx;
            ny = -ny;
        }
    };
    if (cfgEspOn)
    for (auto& e : ents) {
        // Skip self: camera hugs the local character, tiny view depth
        // amplifies noise into jitter (velext defaults showSelf=0 too).
        if (e.self) continue;
        if (e.isNPC && !cfgEspNpc) continue;
        Vec3 root{};
        if (!m.read(e.prim + (uint64_t)o.primitivePosition, root))
            continue;
        if (!std::isfinite(root.x) || !std::isfinite(root.y) ||
            !std::isfinite(root.z))
            continue;
        float dist = -1.0f;
        if (espSelfOk) {
            float dx = root.x - espSelf.x, dy = root.y - espSelf.y,
                  dz = root.z - espSelf.z;
            dist = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        if (cfgEspMaxDist > 0 && dist > (float)cfgEspMaxDist) continue;
        Vec3 head{ root.x, root.y + 2.8f, root.z };
        Vec3 feet{ root.x, root.y - 2.5f, root.z };
        int hx = 0, hy = 0, fx = 0, fy = 0;
        bool onScreen = w2s(head, vm, hx, hy) && w2s(feet, vm, fx, fy);
        int h = fy - hy;
        if (onScreen && (h < 4 || h > 2000)) onScreen = false;
        ImU32 col = e.isNPC ? rgb(cfgEspColNpc)
                  : (e.team && espLocalTeam && e.team == espLocalTeam)
                        ? rgb(cfgEspColTeam)
                        : rgb(cfgEspColEnemy);
        ImU32 edge = IM_COL32(0, 0, 0, 220);
        if (!onScreen) {
            if (cfgEspOof) {
                float nx = 0, ny = 0;
                w2sRay(head, nx, ny);
                float m2 = fmaxf(fabsf(nx), fabsf(ny));
                if (m2 > 1.0f) {
                    float ax = nx / m2, ay = ny / m2;
                    float bw = (float)sw * 0.42f, bh = (float)sh * 0.42f;
                    float px = (float)sw * 0.5f + ax * bw;
                    float py = (float)sh * 0.5f + ay * bh;
                    float p2x = -ay, p2y = ax;
                    ImVec2 tip(px + ax * 10.0f, py + ay * 10.0f);
                    dl->AddTriangleFilled(
                        tip, ImVec2(px + p2x * 7.0f, py + p2y * 7.0f),
                        ImVec2(px - p2x * 7.0f, py - p2y * 7.0f), col);
                    if (dist >= 0.0f) {
                        char db[24];
                        snprintf(db, sizeof db, "%.0fm", dist);
                        dl->AddText(ImVec2(tip.x - 10.0f, tip.y - 16.0f),
                                    edge, db);
                        dl->AddText(ImVec2(tip.x - 11.0f, tip.y - 17.0f),
                                    col, db);
                    }
                }
            }
            continue;
        }
        int w = h / 2;
        int cx = (hx + fx) / 2;
        float bx0 = (float)(cx - w / 2), bx1 = (float)(cx + w / 2);
        float by0 = (float)hy, by1 = (float)fy;
        if (cfgEspFill)
            dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1),
                              (col & 0x00FFFFFFu) | 0x38000000u);
        if (cfgEspBox) {
            // bold box: black underlay then colour on top
            dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), edge, 0.0f, 0,
                        4.0f);
            dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), col, 0.0f, 0,
                        2.0f);
        }
        if (cfgEspHp && e.humanoid && o.humanoidHealth &&
            o.humanoidMaxHealth) {
            float hp = 0.0f, mhp = 0.0f;
            if (m.read(e.humanoid + (uint64_t)o.humanoidHealth, hp) &&
                m.read(e.humanoid + (uint64_t)o.humanoidMaxHealth, mhp) &&
                std::isfinite(hp) && mhp > 0.0f) {
                float f = hp / mhp;
                if (f < 0.0f) f = 0.0f;
                if (f > 1.0f) f = 1.0f;
                float hbx0 = bx0 - 6.0f, hbx1 = bx0 - 3.0f;
                dl->AddRectFilled(ImVec2(hbx0, by0), ImVec2(hbx1, by1),
                                  IM_COL32(0, 0, 0, 190));
                ImU32 hcol = f > 0.5f
                                 ? IM_COL32(80, 255, 80, 255)
                                 : (f > 0.25f ? IM_COL32(255, 220, 60, 255)
                                              : IM_COL32(255, 70, 70, 255));
                dl->AddRectFilled(ImVec2(hbx0, by1 - (by1 - by0) * f),
                                  ImVec2(hbx1, by1), hcol);
            }
        }
        if (cfgEspName || cfgEspDist) {
            char nb[112];
            const char* label = e.tag.c_str();
            if (cfgEspDist && dist >= 0.0f) {
                snprintf(nb, sizeof nb, "%s [%.0fm]", e.tag.c_str(), dist);
                label = nb;
            }
            ImVec2 ts = ImGui::CalcTextSize(label);
            ImVec2 tp((float)cx - ts.x * 0.5f, by0 - ts.y - 4.0f);
            dl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), edge, label);
            dl->AddText(tp, col, label);
        }
        if (cfgEspHead && e.headPrim) {
            int px = 0, py = 0;
            if (w2sPrim(e.headPrim, px, py))
                dl->AddCircleFilled(ImVec2((float)px, (float)py), 3.5f, col);
        }
        if (cfgEspLine)
            dl->AddLine(ImVec2((float)sw * 0.5f, (float)sh),
                        ImVec2((float)cx, by1),
                        (col & 0x00FFFFFFu) | 0x90000000u, 1.5f);
        if (cfgEspSkel && e.headPrim) {
            int rpx = 0, rpy = 0, hpx = 0, hpy = 0;
            if (w2sPrim(e.prim, rpx, rpy) &&
                w2sPrim(e.headPrim, hpx, hpy)) {
                dl->AddLine(ImVec2((float)hpx, (float)hpy),
                            ImVec2((float)rpx, (float)rpy), col, 1.5f);
                for (int i = 0; i < 4; ++i) {
                    int lx = 0, ly = 0;
                    if (w2sPrim(e.limbPrim[i], lx, ly))
                        dl->AddLine(ImVec2((float)rpx, (float)rpy),
                                    ImVec2((float)lx, (float)ly), col,
                                    1.5f);
                }
            }
        }
    }
    // ---- radar: top-right square, self at the centre, camera-up = up ----
    if (cfgRadarOn) {
        float r9[9]{};
        bool haveRot =
            camAddr && m.read(camAddr + (uint64_t)o.cameraRotation, r9);
        int ew = vpw ? vpw : sw;
        int eox = vpw ? vpx : 0, eoy = vph ? vpy : 0;
        float sz = (float)cfgRadarSize;
        float rx = (float)(eox + ew) - sz - 14.0f;
        float ry = (float)eoy + 14.0f + (cfgHudOn ? 34.0f : 0.0f);
        ImVec2 c0(rx + sz * 0.5f, ry + sz * 0.5f);
        // NB: a black translucent fill would blend down to pure black,
        // which is the window colour key -> the panel would vanish.
        dl->AddRectFilled(ImVec2(rx, ry), ImVec2(rx + sz, ry + sz),
                          IM_COL32(10, 12, 16, 150), 6.0f);
        dl->AddRect(ImVec2(rx, ry), ImVec2(rx + sz, ry + sz),
                    IM_COL32(200, 200, 200, 90), 6.0f);
        dl->AddLine(ImVec2(c0.x - 5.0f, c0.y), ImVec2(c0.x + 5.0f, c0.y),
                    IM_COL32(255, 255, 255, 150));
        dl->AddLine(ImVec2(c0.x, c0.y - 5.0f), ImVec2(c0.x, c0.y + 5.0f),
                    IM_COL32(255, 255, 255, 150));
        float range = (float)(cfgRadarRange > 10 ? cfgRadarRange : 10);
        float scale = (sz * 0.5f - 8.0f) / range;
        // Camera CFrame layout (see lookYaw): column 0 = Right,
        // column 2 = -Look, so look = -(m[2],m[5],m[8]).
        float fwdX = -r9[2], fwdZ = -r9[8];
        float rgtX = r9[0], rgtZ = r9[6];
        float fl = sqrtf(fwdX * fwdX + fwdZ * fwdZ);
        float rl = sqrtf(rgtX * rgtX + rgtZ * rgtZ);
        if (haveRot && fl > 1e-4f && rl > 1e-4f && espSelfOk) {
            fwdX /= fl;
            fwdZ /= fl;
            rgtX /= rl;
            rgtZ /= rl;
            for (auto& e : ents) {
                if (e.self) continue;
                if (e.isNPC && !cfgEspNpc) continue;
                Vec3 p{};
                if (!m.read(e.prim + (uint64_t)o.primitivePosition, p))
                    continue;
                if (!std::isfinite(p.x) || !std::isfinite(p.z)) continue;
                float dx = p.x - espSelf.x, dz = p.z - espSelf.z;
                float px = c0.x + (dx * rgtX + dz * rgtZ) * scale;
                float py = c0.y - (dx * fwdX + dz * fwdZ) * scale;
                if (px < rx + 3.0f || px > rx + sz - 3.0f ||
                    py < ry + 3.0f || py > ry + sz - 3.0f)
                    continue;
                ImU32 c = e.isNPC ? rgb(cfgEspColNpc)
                        : (e.team && espLocalTeam &&
                           e.team == espLocalTeam)
                              ? rgb(cfgEspColTeam)
                              : rgb(cfgEspColEnemy);
                dl->AddCircleFilled(ImVec2(px, py), 3.0f, c);
            }
        }
    }
    // ---- saved-waypoint highlighter (Teleport tab) -----------------------
    // Independent of cfgEspOn: markers are a navigation aid, not an ESP
    // category. Drawn from the same projection so they land exactly on the
    // saved world position; a beam + diamond marks the spot, names/distance sit
    // above it, and off-screen waypoints get an edge arrow like the OOF boxes.
    if (cfgWpHl && !g_ways.empty() && vm) {
        ImU32 wcol = rgb(cfgWpHlColor);
        ImU32 wedge = IM_COL32(0, 0, 0, 220);
        for (size_t wi = 0; wi < g_ways.size(); ++wi) {
            const Waypoint& w = g_ways[wi];
            if (!std::isfinite(w.p.x) || !std::isfinite(w.p.y) ||
                !std::isfinite(w.p.z))
                continue;
            float dist = -1.0f;
            if (espSelfOk) {
                float dx = w.p.x - espSelf.x, dy = w.p.y - espSelf.y,
                      dz = w.p.z - espSelf.z;
                dist = sqrtf(dx * dx + dy * dy + dz * dz);
            }
            if (cfgWpHlMax > 0 && dist >= 0.0f && dist > (float)cfgWpHlMax)
                continue;
            Vec3 mark{ w.p.x, w.p.y + 1.0f, w.p.z };
            int px = 0, py = 0;
            bool onScreen = w2s(mark, vm, px, py);
            if (!onScreen) {
                // Behind the camera or outside the client rect: point at it.
                float nx = 0.0f, ny = 0.0f;
                w2sRay(mark, nx, ny);
                float m2 = fmaxf(fabsf(nx), fabsf(ny));
                if (m2 <= 1.0f) continue;
                float ax = nx / m2, ay = ny / m2;
                float bw = (float)sw * 0.42f, bh = (float)sh * 0.42f;
                float ex = (float)sw * 0.5f + ax * bw;
                float ey = (float)sh * 0.5f + ay * bh;
                float p2x = -ay, p2y = ax;
                ImVec2 tip(ex + ax * 10.0f, ey + ay * 10.0f);
                dl->AddTriangleFilled(
                    tip, ImVec2(ex + p2x * 7.0f, ey + p2y * 7.0f),
                    ImVec2(ex - p2x * 7.0f, ey - p2y * 7.0f), wcol);
                ImVec2 ts = ImGui::CalcTextSize(w.name.c_str());
                float lx = tip.x - ts.x * 0.5f, ly = tip.y - ts.y - 6.0f;
                dl->AddText(ImVec2(lx + 1.0f, ly + 1.0f), wedge,
                            w.name.c_str());
                dl->AddText(ImVec2(lx, ly), wcol, w.name.c_str());
                continue;
            }
            // Vertical beam so the marker is readable from a distance even when
            // the diamond sits on unrelated geometry.
            Vec3 top{ w.p.x, w.p.y + 6.0f, w.p.z };
            int tx = 0, ty = 0;
            bool topOk = w2s(top, vm, tx, ty);
            if (topOk)
                dl->AddLine(ImVec2((float)px, (float)py),
                            ImVec2((float)tx, (float)ty), wcol, 2.0f);
            float s = 7.0f;
            ImVec2 c0((float)px, (float)py);
            ImVec2 pts[4] = { ImVec2(c0.x, c0.y - s), ImVec2(c0.x + s, c0.y),
                              ImVec2(c0.x, c0.y + s), ImVec2(c0.x - s, c0.y) };
            dl->AddConvexPolyFilled(pts, 4,
                                    (wcol & 0x00FFFFFFu) | 0x70000000u);
            dl->AddPolyline(pts, 4, wcol, ImDrawFlags_Closed, 2.0f);
            char lb[112];
            if (dist >= 0.0f)
                snprintf(lb, sizeof lb, "%s  %.0fm", w.name.c_str(), dist);
            else
                snprintf(lb, sizeof lb, "%s", w.name.c_str());
            ImVec2 ts = ImGui::CalcTextSize(lb);
            float lx = (float)px - ts.x * 0.5f;
            float ly = (topOk ? (float)ty : (float)py) - ts.y - 6.0f;
            dl->AddText(ImVec2(lx + 1.0f, ly + 1.0f), wedge, lb);
            dl->AddText(ImVec2(lx, ly), wcol, lb);
        }
    }
}

bool EspCtx::w2s(const Vec3& w, const float* mt, int& sx, int& sy) {
    return screenW2S(w, mt, sx, sy, vpx, vpy, vpw, vph, sw, sh);
}
