// ext_projcheck: headless projection truth check.
//
// Prints, for the live camera, both candidate world->screen projections:
//   A) camera-derived (Camera.Rotation + Camera.Position + FieldOfView),
//      the velext v1.0.2 model used by the overlay (ovl_camview.h)
//   B) VisualEngine.ViewMatrix as read from memory
// together with the raw geometry (dotR / dotU / depth) of every player.
//
// The point: these numbers can be compared against the rendered frame by
// eye. In third person the local character must land near the client centre;
// a point to the right of the crosshair must project right of centre.
#include "offsets.h"
#include "ovl/ovl_camview.h"
#include "ovl/ovl_core.h"
#include "ovl/ovl_view.h"
#include "sdk.h"
#include <cstdio>
#include <string>

int main() {
    SetConsoleOutputCP(CP_UTF8);
    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe") || !m.open()) {
        printf("FAIL: game\n");
        return 1;
    }
    Offsets o;
    std::string log;
    if (!OffsetManager::ensure(m.exeVersion(), "offsets_cache.json", o, log)) {
        printf("FAIL offsets\n");
        return 1;
    }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) {
        printf("FAIL base\n");
        return 1;
    }
    uint64_t dm = 0, ws = 0, cam = 0, players = 0, local = 0;
    if (!sdk.dataModel(dm)) {
        printf("FAIL dm\n");
        return 1;
    }
    sdk.workspace(dm, ws);
    sdk.camera(ws, cam);
    sdk.findChildByClass(dm, "Players", players);
    if (players) m.read(players + (uint64_t)o.playersLocalPlayer, local);

    RECT gr{};
    int vw = 0, vh = 0, ox = 0, oy = 0;
    if (gameClientRect(m.pid, gr)) {
        ox = (int)gr.left;
        oy = (int)gr.top;
        vw = (int)(gr.right - gr.left);
        vh = (int)(gr.bottom - gr.top);
    }
    printf("client=%d,%d %dx%d\n", ox, oy, vw, vh);

    float rot[9]{};
    Vec3 eye{};
    float fov = 0.0f;
    bool haveRot = cam && o.cameraRotation &&
                   m.read(cam + (uint64_t)o.cameraRotation, rot);
    bool havePos = cam && sdk.cameraPos(cam, eye);
    bool haveFov = cam && o.cameraFOV &&
                   m.read(cam + (uint64_t)o.cameraFOV, fov);
    printf("camera=0x%llx rot=%d pos=%d fov=%d\n", (unsigned long long)cam,
           haveRot ? 1 : 0, havePos ? 1 : 0, haveFov ? 1 : 0);
    printf("camera pos=%.2f %.2f %.2f fovRaw=%.4f fovDeg=%.2f\n", eye.x, eye.y,
           eye.z, fov, camFovDegrees(fov));
    printf("rot9 = %.3f %.3f %.3f\n       %.3f %.3f %.3f\n       %.3f %.3f %.3f\n",
           rot[0], rot[1], rot[2], rot[3], rot[4], rot[5], rot[6], rot[7],
           rot[8]);

    float cvm[16]{};
    CamView cv;
    bool haveCv = false;
    if (haveRot && havePos && haveFov && vw > 0 && vh > 0) {
        if (camViewBuild(cv, rot, eye, camFovDegrees(fov))) {
            haveCv = camViewMatrix(cv, (float)vw / (float)vh, cvm);
            printf("basis right=%.3f %.3f %.3f up=%.3f %.3f %.3f "
                   "fwd=%.3f %.3f %.3f\n",
                   cv.right.x, cv.right.y, cv.right.z, cv.up.x, cv.up.y,
                   cv.up.z, cv.fwd.x, cv.fwd.y, cv.fwd.z);
        } else {
            printf("camViewBuild REJECTED\n");
        }
    }

    uint64_t ve = 0;
    float vmm[16]{};
    bool haveVe = false;
    if (m.read(sdk.base + o.visualEnginePtr, ve) && ve &&
        m.read(ve + (uint64_t)o.visualEngineViewMatrix, vmm))
        haveVe = true;
    printf("VE=0x%llx matrix=%d sane=%d\n", (unsigned long long)ve,
           haveVe ? 1 : 0,
           haveVe ? (camViewMatrixSane(vmm, eye, vw, vh) ? 1 : 0) : 0);
    if (haveVe) {
        for (int r = 0; r < 4; ++r)
            printf("  ve[%d] = %10.4f %10.4f %10.4f %10.4f\n", r,
                   vmm[r * 4 + 0], vmm[r * 4 + 1], vmm[r * 4 + 2],
                   vmm[r * 4 + 3]);
    }

    printf("players=%d\n", players ? 1 : 0);
    if (!players) return 0;
    sdk.walkChildren(players, [&](uint64_t p) {
        if (sdk.readClass(p) != "Player") return true;
        std::string nm = sdk.readName(p);
        uint64_t model = 0;
        m.read(p + (uint64_t)o.playerModelInstance, model);
        Vec3 pos{};
        bool havePos2 = false;
        if (model) {
            sdk.walkChildren(model, [&](uint64_t part) {
                if (sdk.readName(part) != "HumanoidRootPart") return true;
                uint64_t prim = 0;
                if (m.read(part + (uint64_t)o.basePartPrimitive, prim) && prim)
                    havePos2 = m.read(prim + (uint64_t)o.primitivePosition, pos);
                return false;
            });
        }
        printf("%s %-18s model=0x%llx", p == local ? "[SELF]" : "      ",
               nm.c_str(), (unsigned long long)model);
        if (!havePos2) {
            printf(" no-hrp\n");
            return true;
        }
        Vec3 d{ pos.x - eye.x, pos.y - eye.y, pos.z - eye.z };
        float dl = sqrtf(camViewDot(d, d));
        printf(" pos=%.1f,%.1f,%.1f dist=%.1f", pos.x, pos.y, pos.z, dl);
        if (haveCv) {
            printf(" dotR=%.2f dotU=%.2f depth=%.2f", camViewDot(d, cv.right),
                   camViewDot(d, cv.up), camViewDot(d, cv.fwd));
            int sx = 0, sy = 0;
            bool on = screenW2S(pos, cvm, sx, sy, ox, oy, vw, vh, vw, vh);
            printf(" cam=%d,%d/%d", sx, sy, on ? 1 : 0);
        }
        if (haveVe) {
            int sx = 0, sy = 0;
            bool on = screenW2S(pos, vmm, sx, sy, ox, oy, vw, vh, vw, vh);
            printf(" ve=%d,%d/%d", sx, sy, on ? 1 : 0);
        }
        printf("\n");
        return true;
    });
    printf("PROJCHECK DONE\n");
    return 0;
}
