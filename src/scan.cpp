#include "memory.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

struct Hit {
    uint64_t address = 0;
    uint64_t value = 0;
};

static bool parseU64(const char* s, uint64_t& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    unsigned long long v = strtoull(s, &end, 0);
    if (!end || *end) return false;
    out = (uint64_t)v;
    return true;
}

static DWORD argPid(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--pid")) return (DWORD)strtoul(argv[i + 1], nullptr, 10);
    return 0;
}

static const char* argValue(int argc, char** argv, const char* name) {
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return nullptr;
}

static bool saveHits(const char* path, const std::vector<Hit>& hits) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    for (const Hit& h : hits)
        f << std::hex << h.address << " " << h.value << "\n";
    return (bool)f;
}

static bool loadHits(const char* path, std::vector<Hit>& hits) {
    std::ifstream f(path);
    if (!f) return false;
    uint64_t a = 0, v = 0;
    while (f >> std::hex >> a >> v) {
        if (hits.size() >= 1000000) return false;
        hits.push_back({a, v});
    }
    return !f.bad();
}

static void usage() {
    puts("ext_scan --pid PID --scan-u64 VALUE --out hits.txt [--max-bytes N]");
    puts("ext_scan --pid PID --filter-u64 VALUE --in hits.txt --out hits.txt");
    puts("read-only; no write/freeze support");
}

int main(int argc, char** argv) {
    const char* scan = argValue(argc, argv, "--scan-u64");
    const char* filter = argValue(argc, argv, "--filter-u64");
    const char* out = argValue(argc, argv, "--out");
    const char* in = argValue(argc, argv, "--in");
    if ((!scan && !filter) || !out) {
        usage();
        return 2;
    }
    DWORD pid = argPid(argc, argv);
    if (!pid) {
        usage();
        return 2;
    }
    Memory m;
    m.pid = pid;
    if (!m.open()) {
        puts("error: cannot open process read-only");
        return 1;
    }
    std::vector<Hit> hits;
    uint64_t want = 0;
    const char* value = scan ? scan : filter;
    if (!parseU64(value, want)) {
        puts("error: invalid u64 value");
        return 2;
    }
    if (filter) {
        if (!in || !loadHits(in, hits)) {
            puts("error: cannot load hit file");
            return 1;
        }
        std::vector<Hit> kept;
        for (const Hit& h : hits) {
            uint64_t got = 0;
            if (m.read(h.address, got) && got == want)
                kept.push_back({h.address, got});
        }
        hits.swap(kept);
    } else {
        SIZE_T maxBytes = 256ull * 1024 * 1024;
        if (const char* maxArg = argValue(argc, argv, "--max-bytes")) {
            uint64_t n = 0;
            if (!parseU64(maxArg, n) || n == 0) return 2;
            maxBytes = (SIZE_T)std::min<uint64_t>(n, 1024ull * 1024 * 1024);
        }
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        uint8_t* p = (uint8_t*)si.lpMinimumApplicationAddress;
        uint8_t* end = (uint8_t*)si.lpMaximumApplicationAddress;
        SIZE_T scanned = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        while (p < end && scanned < maxBytes && hits.size() < 1000000) {
            if (!VirtualQueryEx(m.h, p, &mbi, sizeof(mbi))) break;
            uint8_t* next = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT &&
                !(mbi.Protect & PAGE_GUARD) &&
                !(mbi.Protect & PAGE_NOACCESS)) {
                SIZE_T remaining = std::min<SIZE_T>(mbi.RegionSize, maxBytes - scanned);
                for (SIZE_T off = 0; off + sizeof(uint64_t) <= remaining; off += 8) {
                    uint64_t got = 0;
                    SIZE_T read = 0;
                    if (ReadProcessMemory(m.h, (uint8_t*)mbi.BaseAddress + off,
                                          &got, sizeof(got), &read) &&
                        read == sizeof(got) && got == want)
                        hits.push_back({(uint64_t)mbi.BaseAddress + off, got});
                }
                scanned += remaining;
            }
            if (next <= p) break;
            p = next;
        }
    }
    if (!saveHits(out, hits)) {
        puts("error: cannot save hit file");
        return 1;
    }
    printf("scan complete hits=%zu\n", hits.size());
    return 0;
}
