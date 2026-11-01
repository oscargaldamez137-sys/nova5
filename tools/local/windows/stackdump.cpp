// stackdump <pid>: suspend every thread of a process, print its name, rip, a dbghelp
// stack walk and the stack words that resolve to symbols (for frames without unwind info).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#pragma comment(lib, "dbghelp.lib")

static HANDLE g_process;

static bool Describe(DWORD64 address, char* out, size_t size) {
	alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] {};
	auto*                     symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
	symbol->SizeOfStruct             = sizeof(SYMBOL_INFO);
	symbol->MaxNameLen               = 511;
	DWORD64 displacement             = 0;
	if (!SymFromAddr(g_process, address, &displacement, symbol)) return false;
	IMAGEHLP_LINE64 line {};
	line.SizeOfStruct = sizeof(line);
	DWORD column      = 0;
	if (SymGetLineFromAddr64(g_process, address, &column, &line))
		snprintf(out, size, "%s+0x%llx (%s:%lu)", symbol->Name, displacement, line.FileName, line.LineNumber);
	else
		snprintf(out, size, "%s+0x%llx", symbol->Name, displacement);
	return true;
}

int main(int argc, char** argv) {
	if (argc < 2) return 1;
	const DWORD pid = strtoul(argv[1], nullptr, 10);
	const int   scan = argc > 2 ? atoi(argv[2]) : 4096;
	g_process        = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
	if (!g_process) { printf("OpenProcess failed %lu\n", GetLastError()); return 1; }
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
	if (!SymInitialize(g_process, nullptr, TRUE)) { printf("SymInitialize failed\n"); return 1; }
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	THREADENTRY32 entry {};
	entry.dwSize = sizeof(entry);
	for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
		if (entry.th32OwnerProcessID != pid) continue;
		HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, entry.th32ThreadID);
		if (!thread) continue;
		SuspendThread(thread);
		CONTEXT context {};
		context.ContextFlags = CONTEXT_FULL;
		GetThreadContext(thread, &context);
		PWSTR name = nullptr;
		GetThreadDescription(thread, &name);
		char text[1024];
		printf("=== thread %lu \"%ls\" rip=%016llx rsp=%016llx\n", entry.th32ThreadID, name ? name : L"",
		       context.Rip, context.Rsp);
		if (name) LocalFree(name);
		// dbghelp walk (host frames with unwind info).
		CONTEXT      walk_context = context;
		STACKFRAME64 frame {};
		frame.AddrPC.Offset    = context.Rip;
		frame.AddrPC.Mode      = AddrModeFlat;
		frame.AddrStack.Offset = context.Rsp;
		frame.AddrStack.Mode   = AddrModeFlat;
		frame.AddrFrame.Offset = context.Rbp;
		frame.AddrFrame.Mode   = AddrModeFlat;
		for (int i = 0; i < 40; ++i) {
			if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, g_process, thread, &frame, &walk_context, nullptr,
			                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
			    frame.AddrPC.Offset == 0)
				break;
			if (Describe(frame.AddrPC.Offset, text, sizeof(text)))
				printf("  #%d %016llx %s\n", i, frame.AddrPC.Offset, text);
			else
				printf("  #%d %016llx ?\n", i, frame.AddrPC.Offset);
		}
		// Stack scan: words that resolve to a symbol (guest frames have no unwind info).
		std::vector<DWORD64> words(scan);
		SIZE_T               got = 0;
		ReadProcessMemory(g_process, reinterpret_cast<void*>(context.Rsp), words.data(), words.size() * 8, &got);
		int shown = 0;
		for (size_t i = 0; i < got / 8 && shown < 30; ++i) {
			const DWORD64 w = words[i];
			if (w < 0x10000 || SymGetModuleBase64(g_process, w) == 0) continue;
			if (Describe(w, text, sizeof(text))) {
				printf("  [rsp+%zx] %016llx %s\n", i * 8, w, text);
				++shown;
			}
		}
		ResumeThread(thread);
		CloseHandle(thread);
	}
	CloseHandle(snapshot);
	SymCleanup(g_process);
	return 0;
}
