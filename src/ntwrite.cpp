// Shared NtWriteVirtualMemory syscall TU.
// memory.h used to define these per-TU as static. After splitting overlay.cpp
// into modules, a write from another TU would see g_ntWriteId = 0 and silently
// fall back to WriteProcessMemory. One definition here keeps every TU on the
// same syscall path (one Memory instance -> one resolved id).
#include <cstddef>
#include <cstdint>

extern "C" {
__attribute__((used)) uint32_t g_ntWriteId = 0;

// mov eax, [g_ntWriteId] ; mov r10, rcx ; syscall ; ret
// (rdx/r8/r9 + the 5th arg at [rsp+0x28] are already where the kernel wants them)
__attribute__((naked)) long ntWriteSyscall(void* handle, void* addr,
                                           const void* buf, size_t size,
                                           size_t* written) {
    __asm__(
        "movl g_ntWriteId(%rip), %eax\n"
        "movq %rcx, %r10\n"
        "syscall\n"
        "ret\n");
}
}
