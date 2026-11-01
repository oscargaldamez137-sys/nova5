#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

// Synthetic process only. No emulator, game, Vulkan, or on-disk guest code.
[[gnu::naked, gnu::noreturn]] static void BadStack(void*) {
    asm volatile("movq %rcx, %rsp\n\tcallq 1f\n\t1: ud2\n\t");
}
static LONG WINAPI Handled(PEXCEPTION_POINTERS e) {
    return e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
int main(int argc, char** argv) {
    if (argc != 5 || std::strcmp(argv[2], "space argument") ||
        std::strcmp(argv[3], "trailing\\") || std::strcmp(argv[4], "embedded\"quote")) return 3;
    SetThreadDescription(GetCurrentThread(), L"crash-capture-host-fixture");
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    std::printf("[MODULE_MAP] load base=0x%llx size=0x%lx name=old-fixture\n", reinterpret_cast<uint64_t>(base), nt->OptionalHeader.SizeOfImage);
    std::printf("[MODULE_MAP] unload base=0x%llx name=old-fixture\n", reinterpret_cast<uint64_t>(base));
    std::printf("[MODULE_MAP] load base=0x%llx size=0x%lx name=fixture.bin\n", reinterpret_cast<uint64_t>(base), nt->OptionalHeader.SizeOfImage);
    std::fflush(stdout);
    if (!std::strcmp(argv[1], "clean")) return 0;
    if (!std::strcmp(argv[1], "handled")) {
        auto* handler = AddVectoredExceptionHandler(1, Handled);
        if (!handler) return 4;
        ULONG_PTR info[2] {1, 0};
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 2, info);
        RemoveVectoredExceptionHandler(handler);
        return 0;
    }
    if (!std::strcmp(argv[1], "bad-stack")) {
        auto* memory = static_cast<char*>(VirtualAlloc(nullptr, 0x40000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!memory) return 4;
        DWORD old = 0;
        if (!VirtualProtect(memory, 0x20000, PAGE_NOACCESS, &old)) return 4;
        BadStack(memory + 0x18000); // unprobed jump into inaccessible pages, then CALL
    }
    *reinterpret_cast<volatile unsigned char*>(0) = 14;
    return 5;
}
