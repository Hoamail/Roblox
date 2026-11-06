#pragma once
// Minimal RPM wrapper. P1 will extend with caching + WPM.
// Writes can go out as a raw NtWriteVirtualMemory syscall (the way external
// tools do it): ntdll's stub is patched by the anti-cheat, so calling through
// WriteProcessMemory is visible to it, while our own "mov eax,id; syscall" is
// not. Falls back to the Win32 path if the id cannot be resolved.
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <string>
#include <cstdint>

extern "C" {
// Defined once in src\ntwrite.cpp (linked into every write-capable tool).
// Shared so a write from any TU uses the id resolved by openWrite().
extern uint32_t g_ntWriteId;

// mov eax, [g_ntWriteId] ; mov r10, rcx ; syscall ; ret
// (rdx/r8/r9 + the 5th arg at [rsp+0x28] are already where the kernel wants them)
long ntWriteSyscall(void* handle, void* addr, const void* buf, size_t size,
                    size_t* written);
}

class Memory {
public:
    DWORD pid = 0;
    HANDLE h = nullptr;
    uint32_t ntWriteId = 0;     // copy of g_ntWriteId once resolved
    bool directWrite = false;   // false -> always use WriteProcessMemory

    bool findProcess(const std::wstring& name) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        bool ok = false;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (name == pe.szExeFile) { pid = pe.th32ProcessID; ok = true; break; }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return ok;
    }

    bool open() {
        if (h) CloseHandle(h);
        h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
        return h != nullptr;
    }

    // Drop the current handle (e.g. to re-open with write rights after the
    // offsets version was validated).
    void close() {
        if (h) CloseHandle(h);
        h = nullptr;
    }

    // Full rights for memory-write features (aim). May fail where
    // read-only succeeds — caller must handle gracefully.
    bool openWrite() {
        h = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                            PROCESS_QUERY_INFORMATION,
                        FALSE, pid);
        if (h) resolveWriteSyscall();
        return h != nullptr;
    }

    // Reads the syscall id straight out of ntdll's NtWriteVirtualMemory stub:
    //   4C 8B D1        mov r10, rcx
    //   B8 xx xx xx xx  mov eax, <id>
    //   0F 05           syscall
    //   C3              ret
    // Same Hell's/Halo's Gate resolution the reference external does.
    void resolveWriteSyscall() {
        directWrite = false;
        ntWriteId = 0;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return;
        const unsigned char* p = (const unsigned char*)GetProcAddress(ntdll, "NtWriteVirtualMemory");
        if (!p) return;
        for (int i = 0; i < 64; i++) {
            const unsigned char* q = p + i;
            if (q[0] == 0x4C && q[1] == 0x8B && q[2] == 0xD1 && q[3] == 0xB8) {
                ntWriteId = *(const uint32_t*)(q + 4);
                break;
            }
        }
        if (!ntWriteId) { // stub patched: look for any "mov eax,id; syscall"
            for (int i = 0; i < 64; i++) {
                const unsigned char* q = p + i;
                if (q[0] == 0xB8 && q[5] == 0x0F && q[6] == 0x05) {
                    ntWriteId = *(const uint32_t*)(q + 1);
                    break;
                }
            }
        }
        if (ntWriteId) {
            g_ntWriteId = ntWriteId;
            directWrite = true;
        }
    }

    bool writeBytes(uint64_t addr, const void* src, size_t n) {
        SIZE_T written = 0;
        if (directWrite && g_ntWriteId) {
            long st = ntWriteSyscall(h, (void*)addr, src, n, &written);
            if (st >= 0 && written == n) return true;
        }
        return WriteProcessMemory(h, (LPVOID)addr, src, n, &written) &&
               written == n;
    }

    template <typename T>
    bool write(uint64_t addr, const T& val) {
        return writeBytes(addr, (const void*)&val, sizeof(T));
    }

    // "version-<hex>" from the running image path. Empty if unreadable.
    std::string exeVersion() {
        wchar_t buf[MAX_PATH]{};
        if (!GetModuleFileNameExW(h, nullptr, buf, MAX_PATH)) return "";
        std::wstring w = buf;
        size_t p = w.find(L"version-");
        if (p == std::wstring::npos) return "";
        size_t e = w.find(L'\\', p);
        std::wstring v = w.substr(p, e == std::wstring::npos ? e : e - p);
        std::string s;
        for (wchar_t c : v) s += (char)c;
        return s;
    }

    template <typename T>
    bool read(uint64_t addr, T& out) {
        SIZE_T n = 0;
        return ReadProcessMemory(h, (LPCVOID)addr, &out, sizeof(T), &n) && n == sizeof(T);
    }

    ~Memory() { if (h) CloseHandle(h); }
};
