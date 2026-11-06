#include "ovl_tools.h"
#include "ovl_cfg.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Threading contract
//   main thread : toolsTick(), toolsState(), toolsRequest*(), toolsClear()
//   dump thread : dumpWorker() only
// g_toolsMu guards EVERY field of g_tools and the pending-request state.
// toolsTick() used to write g_tools.nodes/status unlocked while dumpWorker()
// wrote g_tools.status locked -> torn std::string/std::vector on the reader.
// Long memory walks now build into a local vector and only splice under the
// lock, so the UI never waits on ReadProcessMemory.
// ---------------------------------------------------------------------------
static ToolState g_tools;
static std::mutex g_toolsMu;
static std::thread g_dumpThread;
static std::atomic<bool> g_dumpRun{false};
static std::atomic<bool> g_dumpCancel{false};
static uint64_t g_lastDm = 0;
static uint64_t g_lastPlace = 0;
static std::string g_lastJob;
static DWORD g_lastTick = 0;
static std::vector<uint64_t> g_pendingParents; // expand queue, one tick drains N
static bool g_rootRefreshPending = false;
static std::vector<MacroEvent> g_macro;
static bool g_macroKeys[256]{};
static DWORD g_macroStart = 0;
static size_t g_macroPlayIndex = 0;
static DWORD g_macroPlayStart = 0;

ToolState toolsState() {
    std::lock_guard<std::mutex> lk(g_toolsMu);
    return g_tools;
}

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof b, "\\u%04x", c);
                out += b;
            } else {
                out += (char)c;
            }
        }
    }
    return out;
}

static void dumpWorker(Memory* m, const Offsets* o, uint64_t place,
                       std::string job, uint64_t root, int depthLimit,
                       int nodeLimit, std::string path) {
    std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
        // One lock, not three: the previous version nested three lock_guards on
        // the same non-recursive mutex and self-deadlocked here.
        std::lock_guard<std::mutex> lk(g_toolsMu);
        g_tools.status = "dump open failed";
        g_tools.dumpRunning = false;
        g_dumpRun.store(false);
        return;
    }
    f << "{\n  \"placeId\": " << (unsigned long long)place
      << ",\n  \"jobId\": \"" << jsonEscape(job)
      << "\",\n  \"root\": [\n";
    int nodes = 0;
    std::function<void(uint64_t, int, bool&)> walk =
        [&](uint64_t obj, int depth, bool& first) {
            if (g_dumpCancel.load() || nodes >= nodeLimit || depth > depthLimit)
                return;
            SDK sdk(*m, *o);
            std::string name = sdk.readName(obj);
            std::string cls = sdk.readClass(obj);
            if (!first) f << ",\n";
            first = false;
            f << "    {\"address\":\"0x" << std::hex
              << (unsigned long long)obj << std::dec
              << "\",\"name\":\"" << jsonEscape(name)
              << "\",\"class\":\"" << jsonEscape(cls)
              << "\",\"depth\":" << depth << "}";
            ++nodes;
            {
                std::lock_guard<std::mutex> lk(g_toolsMu);
                g_tools.dumpNodes = nodes;
            }
            if (depth < depthLimit)
                sdk.walkChildren(obj, [&](uint64_t child) {
                    walk(child, depth + 1, first);
                    return !g_dumpCancel.load() && nodes < nodeLimit;
                });
        };
    bool first = true;
    walk(root, 0, first);
    bool cancelled = g_dumpCancel.load();
    if (!cancelled && nodes < nodeLimit) {
        f << "\n  ]\n}\n";
        f.flush();
    }
    f.close();

    bool published = false;
    const char* status = nullptr;
    if (cancelled) {
        DeleteFileA(tmp.c_str());
        status = "dump cancelled";
    } else if (nodes >= nodeLimit) {
        DeleteFileA(tmp.c_str());
        status = "dump node limit reached";
    } else if (MoveFileExA(tmp.c_str(), path.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        published = true;
        status = "dump complete";
    } else {
        DeleteFileA(tmp.c_str());
        status = "dump rename failed";
    }
    {
        std::lock_guard<std::mutex> lk(g_toolsMu);
        if (published) g_tools.dumpPath = path;
        g_tools.status = status;
        g_tools.dumpRunning = false;
    }
    g_dumpRun.store(false);
}

void toolsClear() {
    toolsCancelDump();
    std::lock_guard<std::mutex> lk(g_toolsMu);
    g_tools.nodes.clear();
    g_tools.selectedInstance = 0;
    g_tools.status.clear();
    g_tools.refreshPending = false;
    g_pendingParents.clear();
    g_rootRefreshPending = false;
    g_lastDm = 0;
}

void toolsCancelDump() {
    if (g_dumpRun.load()) g_dumpCancel.store(true);
    // Join unconditionally: a finished worker is still joinable, and assigning
    // a new std::thread over it (or letting it reach static destruction) calls
    // std::terminate.
    if (g_dumpThread.joinable()) g_dumpThread.join();
    std::lock_guard<std::mutex> lk(g_toolsMu);
    g_tools.dumpRunning = false;
    g_dumpRun.store(false);
}

void toolsShutdown() { toolsCancelDump(); }

static bool robloxForeground(DWORD pid) {
    HWND w = GetForegroundWindow();
    if (!w) return false;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(w, &fgPid);
    return fgPid == pid;
}

void toolsMacroStart() {
    std::lock_guard<std::mutex> lk(g_toolsMu);
    g_macro.clear();
    std::fill(g_macroKeys, g_macroKeys + 256, false);
    g_macroStart = GetTickCount();
    g_tools.macroRecording = true;
    g_tools.macroPlaying = false;
    g_tools.macroEvents = 0;
    g_tools.macroElapsed = 0;
    g_tools.status = "macro recording";
}

void toolsMacroStop() {
    std::lock_guard<std::mutex> lk(g_toolsMu);
    g_tools.macroRecording = false;
    g_tools.macroElapsed = GetTickCount() - g_macroStart;
    g_tools.status = "macro recorded";
}

void toolsMacroCancel() {
    WORD release[256];
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_toolsMu);
        g_tools.macroRecording = false;
        g_tools.macroPlaying = false;
        for (int vk = 1; vk < 256; ++vk)
            if (g_macroKeys[vk]) {
                release[n++] = (WORD)vk;
                g_macroKeys[vk] = false;
            }
        g_tools.status = "macro cancelled";
    }
    // SendInput outside the lock: it can block on the input queue and the UI
    // must not stall behind it.
    for (int i = 0; i < n; ++i) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = release[i];
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(in));
    }
}

void toolsMacroPlay() {
    std::lock_guard<std::mutex> lk(g_toolsMu);
    if (g_tools.macroRecording) return;
    if (g_macro.empty()) {
        g_tools.status = "macro empty";
        return;
    }
    g_macroPlayIndex = 0;
    g_macroPlayStart = GetTickCount();
    g_tools.macroPlaying = true;
    g_tools.macroElapsed = 0;
    g_tools.status = "macro playing";
}

void toolsMacroTick(DWORD now, DWORD pid) {
    std::vector<INPUT> batch;
    {
        std::lock_guard<std::mutex> lk(g_toolsMu);
        if (g_tools.macroRecording) {
            g_tools.macroElapsed = now - g_macroStart;
            if (!robloxForeground(pid) || g_tools.macroElapsed >= 30000 ||
                g_macro.size() >= 2000) {
                g_tools.macroRecording = false;
                g_tools.status = !robloxForeground(pid)
                                     ? "macro stopped (focus)"
                                     : (g_tools.macroElapsed >= 30000
                                            ? "macro stopped (30s)"
                                            : "macro event limit");
                return;
            }
            for (int vk = 1; vk < 256; ++vk) {
                bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (down == g_macroKeys[vk]) continue;
                g_macroKeys[vk] = down;
                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON)
                    continue;
                g_macro.push_back({g_tools.macroElapsed, (WORD)vk, down});
                g_tools.macroEvents = (int)g_macro.size();
            }
            return;
        }
        if (!g_tools.macroPlaying) return;
        if (!robloxForeground(pid) || now - g_macroPlayStart >= 30000) {
            g_tools.macroPlaying = false;
            g_tools.status = "macro stopped";
            return;
        }
        DWORD elapsed = now - g_macroPlayStart;
        g_tools.macroElapsed = elapsed;
        while (g_macroPlayIndex < g_macro.size() &&
               g_macro[g_macroPlayIndex].atMs <= elapsed && batch.size() < 64) {
            const MacroEvent& e = g_macro[g_macroPlayIndex++];
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = e.vk;
            in.ki.dwFlags = e.down ? 0 : KEYEVENTF_KEYUP;
            batch.push_back(in);
        }
        if (g_macroPlayIndex == g_macro.size()) {
            g_tools.macroPlaying = false;
            g_tools.status = "macro complete";
        }
    }
    if (!batch.empty())
        SendInput((UINT)batch.size(), batch.data(), sizeof(INPUT));
}

void toolsRequestDump(Memory& m, const Offsets& o) {
    if (g_dumpRun.load() || !g_lastDm) return;
    if (g_dumpThread.joinable()) g_dumpThread.join(); // reap the finished worker

    std::string dir = g_exeDir.empty() ? std::string("dumps") : exePath("dumps");
    CreateDirectoryA(dir.c_str(), nullptr);
    uint64_t place = 0;
    std::string job;
    {
        std::lock_guard<std::mutex> lk(g_toolsMu);
        place = g_tools.place;
        job = g_tools.jobId;
        g_dumpCancel.store(false);
        g_dumpRun.store(true);
        g_tools.dumpRunning = true;
        g_tools.dumpNodes = 0;
        g_tools.status = "dump running";
    }
    char name[64];
    snprintf(name, sizeof name, "\\%llu_%lu.json", (unsigned long long)place,
             (unsigned long)GetTickCount());
    std::string path = dir + name;
    g_dumpThread = std::thread(dumpWorker, &m, &o, place, std::move(job),
                               g_lastDm, 5, 5000, std::move(path));
}

void toolsRequestRefresh() {
    std::lock_guard<std::mutex> lk(g_toolsMu);
    g_pendingParents.clear();
    g_rootRefreshPending = true;
    g_tools.refreshPending = true;
}

void toolsSelect(uint64_t address) {
    std::lock_guard<std::mutex> lk(g_toolsMu);
    g_tools.selectedInstance = address;
}

// Addresses of every node below `parent` (children, grandchildren, ...).
// Caller holds g_toolsMu. `parent` itself is not included.
static std::vector<uint64_t> collectDescendants(uint64_t parent) {
    std::vector<uint64_t> out{parent};
    for (;;) {
        size_t before = out.size();
        for (const ToolNode& n : g_tools.nodes) {
            if (n.address == parent) continue;
            if (std::find(out.begin(), out.end(), n.address) != out.end())
                continue;
            if (std::find(out.begin(), out.end(), n.parent) != out.end())
                out.push_back(n.address);
        }
        if (out.size() == before) break;
    }
    out.erase(out.begin());
    return out;
}

static void eraseNodes(const std::vector<uint64_t>& addrs) {
    if (addrs.empty()) return;
    g_tools.nodes.erase(
        std::remove_if(g_tools.nodes.begin(), g_tools.nodes.end(),
                       [&addrs](const ToolNode& n) {
                           return std::find(addrs.begin(), addrs.end(),
                                            n.address) != addrs.end();
                       }),
        g_tools.nodes.end());
}

void toolsRequestChildren(uint64_t parent) {
    if (!parent) return;
    std::lock_guard<std::mutex> lk(g_toolsMu);
    for (ToolNode& n : g_tools.nodes) {
        if (n.address != parent) continue;
        if (n.expanded) {
            n.expanded = false;
            n.childrenLoaded = false;
            g_pendingParents.erase(
                std::remove(g_pendingParents.begin(), g_pendingParents.end(),
                            parent),
                g_pendingParents.end());
            eraseNodes(collectDescendants(parent));
        } else {
            n.expanded = true;
            n.childrenLoaded = false;
            if (std::find(g_pendingParents.begin(), g_pendingParents.end(),
                          parent) == g_pendingParents.end())
                g_pendingParents.push_back(parent);
            g_tools.refreshPending = true;
        }
        return; // `n` is dangling past the erase above
    }
}

// Walk one level and splice the result in. The walk itself runs unlocked; only
// the vector swap is under g_toolsMu.
static void loadChildren(Memory& m, SDK& sdk, const Offsets& o,
                         uint64_t parent) {
    int depth = 1;
    {
        std::lock_guard<std::mutex> lk(g_toolsMu);
        for (const ToolNode& n : g_tools.nodes)
            if (n.address == parent) {
                depth = n.depth + 1;
                break;
            }
    }

    std::vector<ToolNode> kids;
    sdk.walkChildren(parent, [&](uint64_t child) {
        if (kids.size() >= 200) return false;
        ToolNode n;
        n.address = child;
        n.parent = parent;
        n.depth = depth;
        n.name = sdk.readName(child);
        n.className = sdk.readClass(child);
        if (n.name.empty()) n.name = "<unnamed>";
        if (n.className.empty()) n.className = "<unknown>";
        uint64_t vec = 0, b = 0, e = 0;
        if (o.instanceChildrenStart &&
            m.read(child + (uint64_t)o.instanceChildrenStart, vec) && vec &&
            m.read(vec, b) && m.read(vec + (uint64_t)o.instanceChildrenEnd, e) &&
            e >= b && e - b <= 0x20000)
            n.childCount = (int)((e - b) / 16);
        kids.push_back(std::move(n));
        return true;
    });
    int added = (int)kids.size();

    std::lock_guard<std::mutex> lk(g_toolsMu);
    // The walk runs unlocked, so the row can be collapsed (or the root rebuilt)
    // while it is in flight. Splicing then would hang children under a closed
    // row, so only publish if the node is still there and still expanded.
    bool stillExpanded = false;
    for (const ToolNode& n : g_tools.nodes)
        if (n.address == parent) {
            stillExpanded = n.expanded;
            break;
        }
    if (!stillExpanded) return;
    // Re-expanding must not duplicate: drop whatever is still attached below.
    eraseNodes(collectDescendants(parent));
    // The tree is one flat vector drawn in order, so the children have to be
    // spliced in directly after their parent - push_back() dumped them at the
    // bottom of the whole list instead of under the row that was expanded.
    size_t at = g_tools.nodes.size();
    for (size_t i = 0; i < g_tools.nodes.size(); ++i)
        if (g_tools.nodes[i].address == parent) {
            at = i + 1;
            break;
        }
    g_tools.nodes.insert(g_tools.nodes.begin() + (ptrdiff_t)at,
                         std::make_move_iterator(kids.begin()),
                         std::make_move_iterator(kids.end()));
    for (ToolNode& n : g_tools.nodes)
        if (n.address == parent) n.childrenLoaded = true;
    g_tools.status = "loaded " + std::to_string(added) + " children";
}

static void rebuildRoot(Memory& m, SDK& sdk, const Offsets& o, uint64_t dm) {
    ToolNode root;
    root.address = dm;
    root.name = sdk.readName(dm);
    root.className = sdk.readClass(dm);
    if (root.name.empty()) root.name = "DataModel";
    if (root.className.empty()) root.className = "DataModel";
    root.expanded = true;
    {
        std::lock_guard<std::mutex> lk(g_toolsMu);
        g_tools.nodes.clear();
        g_tools.nodes.push_back(std::move(root));
    }
    loadChildren(m, sdk, o, dm);
}

void toolsTick(Memory& m, SDK& sdk, const Offsets& o, uint64_t place,
               const std::string& jobId, DWORD now) {
    toolsMacroTick(now, m.pid);
    uint64_t dm = 0;
    if (!sdk.dataModel(dm)) {
        bool had = false;
        {
            std::lock_guard<std::mutex> lk(g_toolsMu);
            had = g_lastDm != 0;
        }
        if (had) toolsClear();
        std::lock_guard<std::mutex> lk(g_toolsMu);
        g_tools.status = "DataModel unavailable";
        g_lastDm = 0;
        return;
    }
    bool changed;
    {
        std::lock_guard<std::mutex> lk(g_toolsMu);
        changed = place != g_lastPlace || jobId != g_lastJob || dm != g_lastDm;
    }
    if (changed && g_dumpRun.load()) toolsCancelDump();
    if (changed) {
        toolsClear(); // locks internally
        std::lock_guard<std::mutex> lk(g_toolsMu);
        ++g_tools.generation;
        g_lastPlace = place;
        g_lastJob = jobId;
        g_tools.place = place;
        g_tools.jobId = jobId;
        g_lastDm = dm;
        g_rootRefreshPending = true;
        g_tools.refreshPending = true;
    }

    bool root = false;
    std::vector<uint64_t> parents;
    {
        std::lock_guard<std::mutex> lk(g_toolsMu);
        if (!g_tools.refreshPending || now - g_lastTick < 100) return;
        g_lastTick = now;
        g_tools.refreshPending = false;
        root = g_rootRefreshPending;
        g_rootRefreshPending = false;
        parents.swap(g_pendingParents);
        if (parents.size() > 8) {
            g_pendingParents.assign(parents.begin() + 8, parents.end());
            parents.resize(8);
            g_tools.refreshPending = true;
        }
    }

    if (root) rebuildRoot(m, sdk, o, dm);
    for (uint64_t p : parents) {
        bool want;
        {
            std::lock_guard<std::mutex> lk(g_toolsMu);
            want = false;
            for (const ToolNode& n : g_tools.nodes)
                if (n.address == p) {
                    want = n.expanded && !n.childrenLoaded;
                    break;
                }
        }
        if (want) loadChildren(m, sdk, o, p);
    }
}
