// ext_dump: machine-readable DataModel tree dumps for game profiling.
// Read-only. No game effect.
//
//   ext_dump --status
//   ext_dump --tree [--root Workspace,...] [--class BasePart,...]
//            [--name-contains Hitbox] [--depth 6] [--budget 5000]
//            [--with-pos] [--with-prim] [--out file.json]
//
// Defaults are structure-only (class/name/path/child-count). --with-pos and
// --with-prim cost +2/+1 RPM per node: only use with narrow filters.
#include "offsets.h"
#include "sdk.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static std::string toLower(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = (char)tolower((unsigned char)c);
    return r;
}

static std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static void jsonEscape(FILE* f, const std::string& s) {
    fputc('"', f);
    for (unsigned char c : s) {
        if (c == '"') fputs("\\\"", f);
        else if (c == '\\') fputs("\\\\", f);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static bool isPartClass(const std::string& cls) {
    return cls == "Part" || cls == "MeshPart" || cls == "UnionOperation" ||
           cls == "WedgePart" || cls == "CornerWedgePart" ||
           cls == "TrussPart" || cls == "SpawnLocation" || cls == "Seat" ||
           cls == "VehicleSeat";
}
static bool hasArg(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], flag)) return true;
    return false;
}

static std::string getArg(int argc, char** argv, const char* flag,
                          const std::string& dflt) {
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], flag)) return argv[i + 1];
    return dflt;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    bool status = hasArg(argc, argv, "--status");
    bool tree = hasArg(argc, argv, "--tree");
    bool rawMode = hasArg(argc, argv, "--raw");
    if (!status && !tree && !rawMode) {
        printf("usage: ext_dump --status | ext_dump --tree [filters]\n");
        return 1;
    }
    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe") || !m.open()) {
        printf("{\"error\":\"game process not found\"}\n");
        return 1;
    }
    const char* rawAddr = nullptr;
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--raw")) rawAddr = argv[i + 1];
    if (rawAddr) {
        uint64_t addr = strtoull(rawAddr, nullptr, 0);
        int n = atoi(getArg(argc, argv, "--raw-n", "32").c_str());
        if (n < 1) n = 1;
        if (n > 256) n = 256;
        printf("{\"addr\":\"0x%llx\",\"cells\":[", (unsigned long long)addr);
        for (int i = 0; i < n; ++i) {
            uint64_t v = 0;
            bool ok = m.read(addr + (uint64_t)i * 8, v);
            float f[2]{};
            double d = 0;
            memcpy(&f, &v, 8);
            memcpy(&d, &v, 8);
            if (i) printf(",");
            printf("\n  ");
            printf("{\"off\":%d,\"ok\":%s,\"u64\":\"0x%llx\",\"f32\":[%.4g,%.4g],\"f64\":%.6g}",
                   i * 8, ok ? "true" : "false", (unsigned long long)v, f[0],
                   f[1], d);
        }
        printf("\n]}\n");
        return 0;
    }
    Offsets o;
    std::string log, cache = "offsets_cache.json";
    if (!OffsetManager::ensure(m.exeVersion(), cache, o, log)) {
        printf("{\"error\":\"offsets unavailable\"}\n");
        return 1;
    }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) {
        printf("{\"error\":\"module base not found\"}\n");
        return 1;
    }
    uint64_t dm = 0;
    if (!sdk.dataModel(dm)) {
        printf("{\"error\":\"datamodel not found\"}\n");
        return 1;
    }
    uint64_t place = 0, game = 0;
    sdk.placeId(dm, place);
    sdk.gameId(dm, game);

    if (status) {
        printf("{\"pid\":%lu,\"placeId\":%llu,\"gameId\":%llu,",
               (unsigned long)m.pid, (unsigned long long)place,
               (unsigned long long)game);
        printf("\"version\":");
        jsonEscape(stdout, m.exeVersion());
        printf("}\n");
        return 0;
    }

    // ---- --tree ----
    std::vector<std::string> roots = splitComma(getArg(argc, argv, "--root", "Workspace"));
    std::vector<std::string> classes = splitComma(getArg(argc, argv, "--class", ""));
    std::string wantName = toLower(getArg(argc, argv, "--name-contains", ""));
    int maxDepth = atoi(getArg(argc, argv, "--depth", "6").c_str());
    int budget = atoi(getArg(argc, argv, "--budget", "5000").c_str());
    bool withPos = hasArg(argc, argv, "--with-pos");
    bool withPrim = hasArg(argc, argv, "--with-prim");
    bool withAddr = hasArg(argc, argv, "--with-addr");
    if (maxDepth < 0) maxDepth = 0;
    if (maxDepth > 12) maxDepth = 12;
    if (budget < 1) budget = 1;
    if (budget > 50000) budget = 50000;
    std::string outPath = getArg(argc, argv, "--out", "");

    // Resolve root addresses by name among DataModel children.
    struct Frame {
        uint64_t addr;
        std::string parent;
        int depth;
    };
    std::vector<Frame> stack;
    {
        uint64_t vec = 0, b = 0, e = 0;
        if (m.read(dm + (uint64_t)o.instanceChildrenStart, vec) && vec &&
            m.read(vec, b) && m.read(vec + 8, e) && e > b &&
            e - b <= 0x20000) {
            long long n = (long long)((e - b) / 16);
            if (n > 2000) n = 2000;
            for (long long i = 0; i < n; ++i) {
                uint64_t child = 0;
                if (!m.read(b + (uint64_t)i * 16, child) || !child) continue;
                std::string nm = sdk.readName(child);
                for (auto& r : roots) {
                    if (nm == r) stack.push_back({ child, "", 0 });
                }
            }
        }
    }
    if (stack.empty()) {
        printf("{\"error\":\"roots not found\"}\n");
        return 1;
    }

    FILE* f = stdout;
    if (!outPath.empty()) {
        f = fopen(outPath.c_str(), "w");
        if (!f) {
            printf("{\"error\":\"cannot open output file\"}\n");
            return 1;
        }
    }
    fprintf(f, "{\"placeId\":%llu,\"gameId\":%llu,\"version\":",
            (unsigned long long)place, (unsigned long long)game);
    jsonEscape(f, m.exeVersion());
    fprintf(f, ",\"nodes\":[");

    bool truncated = false;
    int emitted = 0;
    bool first = true;
    // DFS with explicit stack (LIFO). Budget caps emitted nodes.
    while (!stack.empty() && !truncated) {
        Frame fr = stack.back();
        stack.pop_back();
        std::string nm = sdk.readName(fr.addr);
        std::string cls = sdk.readClass(fr.addr);
        std::string fpath = fr.parent.empty() ? nm : (fr.parent + "." + nm);
        // Enumerate children (needed for child-count + descent).
        std::vector<uint64_t> kids;
        {
            uint64_t vec = 0, b = 0, e = 0;
            if (m.read(fr.addr + (uint64_t)o.instanceChildrenStart, vec) && vec &&
                m.read(vec, b) && m.read(vec + 8, e) && e > b &&
                e - b <= 0x20000) {
                long long n = (long long)((e - b) / 16);
                if (n > 2000) n = 2000;
                for (long long i = 0; i < n; ++i) {
                    uint64_t child = 0;
                    if (!m.read(b + (uint64_t)i * 16, child) || !child) continue;
                    kids.push_back(child);
                }
            }
        }
        bool classOk = classes.empty();
        for (auto& c : classes)
            if (cls == c) {
                classOk = true;
                break;
            }
        bool nameOk = wantName.empty() ||
                      toLower(nm).find(wantName) != std::string::npos;
        bool isRoot = (fr.depth == 0);
        if ((isRoot || (classOk && nameOk)) && emitted < budget) {
            if (!first) fprintf(f, ",");
            first = false;
            fprintf(f, "{\"path\":");
            jsonEscape(f, fpath);
            fprintf(f, ",\"name\":");
            jsonEscape(f, nm);
            fprintf(f, ",\"class\":");
            jsonEscape(f, cls);
            fprintf(f, ",\"kids\":%d", (int)kids.size());
            if (withAddr)
                fprintf(f, ",\"addr\":\"0x%llx\"", (unsigned long long)fr.addr);
            if ((withPrim || withPos) && isPartClass(cls)) {
                uint64_t prim = 0;
                if (o.basePartPrimitive &&
                    m.read(fr.addr + (uint64_t)o.basePartPrimitive, prim) &&
                    SDK::plausiblePtr(prim)) {
                    fprintf(f, ",\"prim\":\"0x%llx\"", (unsigned long long)prim);
                    if (withPos && o.primitivePosition) {
                        Vec3 v{};
                        if (m.read(prim + (uint64_t)o.primitivePosition, v) &&
                            std::isfinite(v.x) && std::isfinite(v.y) &&
                            std::isfinite(v.z))
                            fprintf(f, ",\"pos\":[%.2f,%.2f,%.2f]", v.x, v.y, v.z);
                        if (o.primitiveSize) {
                            float sz[3]{};
                            if (m.read(prim + (uint64_t)o.primitiveSize, sz) &&
                                std::isfinite(sz[0]) && std::isfinite(sz[1]) &&
                                std::isfinite(sz[2]) && sz[0] > 0 && sz[0] < 500 &&
                                sz[1] > 0 && sz[1] < 500 && sz[2] > 0 && sz[2] < 500)
                                fprintf(f, ",\"size\":[%.2f,%.2f,%.2f]", sz[0],
                                        sz[1], sz[2]);
                        }
                        if (o.primitiveFlags) {
                            uint32_t fl = 0;
                            if (m.read(prim + (uint64_t)o.primitiveFlags, fl))
                                fprintf(f, ",\"flags\":\"0x%x\"", fl);
                        }
                    }
                }
            }
            fprintf(f, "}");
            ++emitted;
        }
        if (emitted >= budget) {
            truncated = true;
            break;
        }
        if (fr.depth < maxDepth) {
            // Push reversed to keep original order on pop.
            // Names resolve at pop time (one read per node).
            for (size_t i = kids.size(); i-- > 0;)
                stack.push_back({ kids[i], fpath, fr.depth + 1 });
        }
    }
    fprintf(f, "],\"emitted\":%d,\"truncated\":%s}\n", emitted,
            truncated ? "true" : "false");
    if (f != stdout) fclose(f);
    return 0;
}
