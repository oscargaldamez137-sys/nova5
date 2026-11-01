// Optional out-of-process observer: Windows can reject exception delivery when
// guest RSP points at inaccessible memory. A VEH in that process cannot help.
// This observer never consumes guest exceptions or patches guest instructions.
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

struct Module { uint64_t base, size; std::string name; };
static std::vector<Module> modules;
static std::vector<Module> native_modules;
static FILE* report;
static HANDLE process;

static void ReadModules(const wchar_t* path) {
    modules = native_modules;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        unsigned long long base = 0, size = 0; int at = 0;
        if (std::sscanf(line.c_str(), "[MODULE_MAP] load base=0x%llx size=0x%llx name=%n", &base, &size, &at) == 2 && at) {
            modules.push_back({base, size, line.substr(at)});
        } else if (std::sscanf(line.c_str(), "[MODULE_MAP] unload base=0x%llx", &base) == 1) {
            std::erase_if(modules, [base](const auto& m) { return m.base == base; });
        }
    }
}
static std::string Address(uint64_t address) {
    char text[512];
    for (auto i = modules.rbegin(); i != modules.rend(); ++i) {
        if (address >= i->base && address - i->base < i->size) {
            std::snprintf(text, sizeof(text), "%s+0x%llx", i->name.c_str(), address - i->base);
            return text;
        }
    }
    std::snprintf(text, sizeof(text), "0x%llx", address);
    return text;
}
static bool Read(uint64_t address, void* dest, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(process, reinterpret_cast<void*>(address), dest, size, &got) && got == size;
}
static void NativeModule(void* base, HANDLE file) {
    IMAGE_DOS_HEADER dos {}; IMAGE_NT_HEADERS64 nt {};
    const auto address = reinterpret_cast<uint64_t>(base);
    if (!Read(address, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        !Read(address + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) return;
    char path[1024] {};
    if (!file || !GetFinalPathNameByHandleA(file, path, sizeof(path), 0)) std::snprintf(path, sizeof(path), "native-module");
    const char* name = path;
    for (const char* p=path; *p; ++p) if (*p=='\\' || *p=='/') name=p+1;
    native_modules.push_back({address, nt.OptionalHeader.SizeOfImage, name});
}
static void Region(const char* name, uint64_t address) {
    MEMORY_BASIC_INFORMATION m {};
    if (!VirtualQueryEx(process, reinterpret_cast<void*>(address), &m, sizeof(m))) return;
    std::fprintf(report, "%s=0x%llx region=%p+0x%zx allocation=%p state=0x%lx protect=0x%lx type=0x%lx\n",
        name, address, m.BaseAddress, m.RegionSize, m.AllocationBase, m.State, m.Protect, m.Type);
}
static void ThreadStack(HANDLE thread) {
    struct BasicInfo {
        LONG exit_status; void* teb; void* process_id; void* thread_id;
        ULONG_PTR affinity; LONG priority; LONG base_priority;
    };
    using QueryThread = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    const auto query = reinterpret_cast<QueryThread>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    BasicInfo info {}; NT_TIB tib {};
    if (!query || query(thread, 0, &info, sizeof(info), nullptr) < 0 ||
        !Read(reinterpret_cast<uint64_t>(info.teb), &tib, sizeof(tib))) return;
    // Same x64 TEB offset used by KYTY_GUEST_TEB_STACK_RANGE_ASM.
    uint64_t allocation = 0;
    Read(reinterpret_cast<uint64_t>(info.teb) + 0x1478, &allocation, sizeof(allocation));
    std::fprintf(report, "TEB=%p StackBase=%p StackLimit=%p DeallocationStack=0x%llx\n",
        info.teb, tib.StackBase, tib.StackLimit, allocation);
    Region("native stack allocation", allocation);
}
static void Words(const char* name, uint64_t address, size_t count) {
    std::fprintf(report, "%s at 0x%llx:\n", name, address);
    for (size_t i = 0; i < count; i += 4) {
        uint64_t words[4] {};
        if (!Read(address + i * 8, words, sizeof(words))) {
            std::fprintf(report, "  0x%llx unreadable\n", address + i * 8); break;
        }
        std::fprintf(report, "  0x%llx: %016llx %016llx %016llx %016llx\n", address + i * 8,
            words[0], words[1], words[2], words[3]);
    }
}
static void Capture(const DEBUG_EVENT& e, const wchar_t* log) {
    ReadModules(log);
    const auto& ex = e.u.Exception;
    const auto& r = ex.ExceptionRecord;
    std::fprintf(report, "[CRASH_CAPTURE] chance=%s pid=%lu tid=%lu code=0x%08lx flags=0x%lx pc=%s\n",
        ex.dwFirstChance ? "first (not necessarily fatal)" : "second", e.dwProcessId, e.dwThreadId,
        r.ExceptionCode, r.ExceptionFlags, Address(reinterpret_cast<uint64_t>(r.ExceptionAddress)).c_str());
    for (DWORD i = 0; i < r.NumberParameters; ++i)
        std::fprintf(report, "  exception.parameter[%lu]=0x%llx\n", i, r.ExceptionInformation[i]);
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, e.dwThreadId);
    CONTEXT c {}; c.ContextFlags = CONTEXT_ALL;
    if (thread && GetThreadContext(thread, &c)) {
        ThreadStack(thread);
        PWSTR name = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &name))) {
            std::fprintf(report, "thread name: %ls\n", name); LocalFree(name);
        }
        std::fprintf(report,
            "rip=%016llx rsp=%016llx rbp=%016llx eflags=%08lx mxcsr=%08lx\n"
            "rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n"
            "rsi=%016llx rdi=%016llx r8=%016llx r9=%016llx\n"
            "r10=%016llx r11=%016llx r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n",
            c.Rip,c.Rsp,c.Rbp,c.EFlags,c.MxCsr,c.Rax,c.Rbx,c.Rcx,c.Rdx,c.Rsi,c.Rdi,c.R8,c.R9,c.R10,c.R11,c.R12,c.R13,c.R14,c.R15);
        for (int i = 0; i < 16; ++i)
            std::fprintf(report, "xmm%d=%016llx%016llx\n", i, c.FltSave.XmmRegisters[i].High, c.FltSave.XmmRegisters[i].Low);
        Region("rip",c.Rip); Region("rsp",c.Rsp); Region("rsp-8",c.Rsp-8); Region("rbp",c.Rbp);
        if (r.NumberParameters >= 2) Region("fault address",r.ExceptionInformation[1]);
        std::fprintf(report, "instruction bytes at RIP:");
        for (unsigned i=0; i<32; ++i) { unsigned char b=0; if (!Read(c.Rip+i,&b,1)) break; std::fprintf(report," %02x",b); }
        std::fprintf(report,"\n");
        Words("stack",c.Rsp,128);
        uint64_t frame = c.Rbp;
        for (unsigned depth=0; depth<12; ++depth) {
            uint64_t pair[2] {};
            if (frame < c.Rsp || frame-c.Rsp>0x100000 || (frame&7) || !Read(frame,pair,sizeof(pair))) break;
            std::fprintf(report,"frame[%u]=0x%llx return=%s\n",depth,frame,Address(pair[1]).c_str());
            Words("frame locals/arguments",frame-0x100,48);
            if (pair[0]<=frame) break;
            frame=pair[0];
        }
        const uint64_t regs[] {c.Rax,c.Rbx,c.Rcx,c.Rdx,c.Rsi,c.Rdi,c.R8,c.R9,c.R10,c.R11,c.R12,c.R13,c.R14,c.R15};
        for (auto value:regs) if(value>=0x10000 && value<0x10000000000ull) Words("register target",value,16);
    } else std::fprintf(report,"GetThreadContext failed: %lu\n",GetLastError());
    if(thread) CloseHandle(thread);
    for(const auto& m:modules) std::fprintf(report,"[MODULE_MAP] base=0x%llx size=0x%llx name=%s\n",m.base,m.size,m.name.c_str());
    std::fflush(report);
}
static std::wstring Quote(const wchar_t* value) {
    std::wstring out=L"\""; size_t slashes=0;
    for (const wchar_t* p=value;;++p) {
        if (*p==L'\\') { ++slashes; continue; }
        if (*p==L'"' || !*p) out.append(slashes*2,L'\\'); else out.append(slashes,L'\\');
        slashes=0; if(!*p) break;
        if(*p==L'"') out+=L'\\'; out+=*p;
    }
    return out+L'"';
}
int wmain(int argc,wchar_t** argv) {
    if(argc<6) { std::fprintf(stderr,"usage: guest-crash-capture report.log module.out.log watch-rip-or-0 affinity-or-0 program [args...]\n"); return 2; }
    report=_wfopen(argv[1],L"w"); if(!report) return 2;
    const uint64_t watch=std::wcstoull(argv[3],nullptr,0);
    std::wstring command;
    for(int i=5;i<argc;++i) { if(i!=5) command+=L' '; command+=Quote(argv[i]); }
    STARTUPINFOW si {}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdInput=GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE); si.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi {};
    // Debug creation enables costly heap checks before any guest code runs.
    // Keep the normal heap by default; explicit _NO_DEBUG_HEAP=0 opts back in.
    wchar_t heap_mode[16] {};
    if (!GetEnvironmentVariableW(L"_NO_DEBUG_HEAP", heap_mode, 16)) {
        if (!SetEnvironmentVariableW(L"_NO_DEBUG_HEAP", L"1")) {
            std::fprintf(report,"SetEnvironmentVariable(_NO_DEBUG_HEAP) failed: %lu\n",GetLastError());
            std::fclose(report); return 2;
        }
        std::wcscpy(heap_mode,L"1");
    }
    std::fprintf(report,"[CRASH_CAPTURE] child _NO_DEBUG_HEAP=%ls (1 = normal heap)\n",heap_mode);
    if(!CreateProcessW(argv[5],command.data(),nullptr,nullptr,TRUE,DEBUG_ONLY_THIS_PROCESS,nullptr,nullptr,&si,&pi)) {
        std::fprintf(report,"CreateProcess failed: %lu\n",GetLastError()); std::fclose(report); return 2;
    }
    process=pi.hProcess;
    // Apply the requested mask before releasing the initial debug event, avoiding
    // a race with the launcher's assignment to the observer's ProcessorAffinity.
    DWORD_PTR affinity=0,system=0;
    const auto requested=static_cast<DWORD_PTR>(std::wcstoull(argv[4],nullptr,0));
    if(requested) affinity=requested;
    else GetProcessAffinityMask(GetCurrentProcess(),&affinity,&system);
    if(affinity && !SetProcessAffinityMask(process,affinity)) {
        std::fprintf(report,"SetProcessAffinityMask failed: %lu\n",GetLastError()); std::fclose(report); return 2;
    }
    std::fprintf(report,"observer pid=%lu child pid=%lu watch=0x%llx; exceptions passed through\n",GetCurrentProcessId(),pi.dwProcessId,watch);
    std::fflush(report);
    unsigned samples=0; bool initial_break=true;
    uint64_t first_chance=0, second_chance=0, access_violations=0, illegal_instructions=0, guard_pages=0;
    const auto started=GetTickCount64();
    auto reported=started;
    const auto summary=[&] {
        std::fprintf(report,"[CRASH_CAPTURE] elapsed_ms=%llu first=%llu second=%llu av=%llu illegal=%llu guard=%llu\n",
            GetTickCount64()-started,first_chance,second_chance,access_violations,illegal_instructions,guard_pages);
        std::fflush(report);
    };
    for(;;) {
        DEBUG_EVENT e {};
        const bool event=WaitForDebugEvent(&e,1000)!=FALSE;
        const auto error=event ? ERROR_SUCCESS : GetLastError();
        const auto now=GetTickCount64();
        if(now-reported>=10000) { summary(); reported=now; }
        if(!event) {
            if(error==ERROR_SEM_TIMEOUT) continue;
            std::fprintf(report,"WaitForDebugEvent failed: %lu\n",error); break;
        }
        DWORD disposition=DBG_CONTINUE;
        if(e.dwDebugEventCode==EXCEPTION_DEBUG_EVENT) {
            const auto& ex=e.u.Exception; const auto& r=ex.ExceptionRecord;
            if(ex.dwFirstChance) ++first_chance; else ++second_chance;
            if(r.ExceptionCode==EXCEPTION_ACCESS_VIOLATION) ++access_violations;
            if(r.ExceptionCode==EXCEPTION_ILLEGAL_INSTRUCTION) ++illegal_instructions;
            if(r.ExceptionCode==STATUS_GUARD_PAGE_VIOLATION) ++guard_pages;
            disposition=DBG_EXCEPTION_NOT_HANDLED;
            if(initial_break && ex.dwFirstChance && r.ExceptionCode==EXCEPTION_BREAKPOINT) { initial_break=false; disposition=DBG_CONTINUE; }
            else if(!ex.dwFirstChance || (samples<8 &&
                ((watch && reinterpret_cast<uint64_t>(r.ExceptionAddress)==watch) ||
                 (r.ExceptionCode==EXCEPTION_ACCESS_VIOLATION && r.NumberParameters>=2 && r.ExceptionInformation[1]==0)))) {
                Capture(e,argv[2]); ++samples;
            }
        } else if(e.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT) {
            NativeModule(e.u.LoadDll.lpBaseOfDll,e.u.LoadDll.hFile);
            if(e.u.LoadDll.hFile) CloseHandle(e.u.LoadDll.hFile);
        } else if(e.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT) {
            NativeModule(e.u.CreateProcessInfo.lpBaseOfImage,e.u.CreateProcessInfo.hFile);
            if(e.u.CreateProcessInfo.hFile) CloseHandle(e.u.CreateProcessInfo.hFile);
        } else if(e.dwDebugEventCode==UNLOAD_DLL_DEBUG_EVENT) {
            std::erase_if(native_modules,[&](const auto& m){ return m.base==reinterpret_cast<uint64_t>(e.u.UnloadDll.lpBaseOfDll); });
        }
        else if(e.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT) {
            const DWORD code=e.u.ExitProcess.dwExitCode;
            summary();
            std::fprintf(report,"[CRASH_CAPTURE] process-exit code=0x%08lx\n",code); std::fclose(report);
            ContinueDebugEvent(e.dwProcessId,e.dwThreadId,DBG_CONTINUE);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return static_cast<int>(code);
        }
        if(!ContinueDebugEvent(e.dwProcessId,e.dwThreadId,disposition)) break;
    }
    std::fflush(report); return 2;
}
