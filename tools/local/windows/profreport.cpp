// profreport <pid> <samples> [top] [focus]: symbolize live `prof` samples (16 words each: pc, word at
// rsp, frame-pointer return addresses) against the running process; print self and inclusive
// percentages by function, and the same with waiting samples removed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "dbghelp.lib")

static HANDLE g_process;
static std::unordered_map<DWORD64, std::string> g_names;

// KYTY_MAP=<lld-link /lldmap file>: names for every function of the executable (the PDB of a
// release build only has public symbols, so internal functions get a neighbour's name).
static std::vector<std::pair<DWORD64, std::string>> g_map;
static DWORD64                                      g_map_base = 0x140000000ull;

static void LoadMap(const char* path) {
	FILE* file = fopen(path, "r");
	if (!file) return;
	char line[4096];
	while (fgets(line, sizeof(line), file)) {
		unsigned long long address = 0, size = 0;
		unsigned            align   = 0;
		int                 used    = 0;
		if (sscanf(line, "%llx %llx %u %n", &address, &size, &align, &used) != 3 || size != 0 || align != 0) continue;
		std::string name = line + used;
		while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) name.pop_back();
		if (name.empty()) continue;
		g_map.emplace_back(address, std::move(name));
	}
	fclose(file);
	std::sort(g_map.begin(), g_map.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
}

static const std::string& Name(DWORD64 address) {
	auto it = g_names.find(address);
	if (it != g_names.end()) return it->second;
	std::string name;
	if (!g_map.empty() && address >= g_map_base + g_map.front().first && address < g_map_base + 0x2000000ull) {
		const DWORD64 rva = address - g_map_base;
		auto it = std::upper_bound(g_map.begin(), g_map.end(), rva, [](DWORD64 v, const auto& e) { return v < e.first; });
		if (it != g_map.begin()) {
			--it;
			// Strip the calling convention and access words the map prints.
			std::string text = it->second;
			for (const char* prefix: {"public: ", "private: ", "protected: ", "static ", "virtual "})
				if (text.rfind(prefix, 0) == 0) text = text.substr(strlen(prefix));
			const auto convention = text.find("__cdecl ");
			if (convention != std::string::npos) text = text.substr(convention + 8);
			return g_names.emplace(address, "kyty_emulator!" + text).first->second;
		}
	}
	alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] {};
	auto* symbol         = reinterpret_cast<SYMBOL_INFO*>(buffer);
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	symbol->MaxNameLen   = 511;
	DWORD64 displacement = 0;
	IMAGEHLP_MODULE64 module {};
	module.SizeOfStruct = sizeof(module);
	const bool has_module = SymGetModuleInfo64(g_process, address, &module) != 0;
	if (SymFromAddr(g_process, address, &displacement, symbol)) {
		name = std::string(has_module ? module.ModuleName : "?") + "!" + symbol->Name;
	} else if (has_module) {
		name = std::string(module.ModuleName) + "!?";
	} else if (address >= 0x800000000ull && address < 0x1000000000ull) {
		name = "guest-code";
	} else {
		name = "?";
	}
	return g_names.emplace(address, std::move(name)).first->second;
}

static bool IsWait(const std::string& name) {
	static const char* const waits[] = {"NtWaitFor", "ZwWaitFor", "NtDelayExecution", "ZwDelayExecution",
	                                    "NtWaitForAlertByThreadId", "ZwWaitForAlertByThreadId", "SleepEx",
	                                    "NtYieldExecution", "ZwYieldExecution", "SwitchToThread",
	                                    "NtDxgk", "NtGdiDdDDIWait", "ZwRemoveIoCompletion", "NtRemoveIoCompletion"};
	for (const char* wait: waits)
		if (name.find(wait) != std::string::npos) return true;
	return false;
}

static void Print(const char* title, const std::map<std::string, size_t>& counts, size_t total, size_t top) {
	std::vector<std::pair<size_t, std::string>> sorted;
	for (const auto& [name, count]: counts) sorted.emplace_back(count, name);
	std::sort(sorted.rbegin(), sorted.rend());
	printf("== %s (%zu samples)\n", title, total);
	for (size_t i = 0; i < sorted.size() && i < top; ++i)
		printf("%6.2f%% %7zu  %s\n", 100.0 * sorted[i].first / total, sorted[i].first, sorted[i].second.substr(0, 150).c_str());
}

int main(int argc, char** argv) {
	if (argc < 3) return 1;
	const DWORD  pid = strtoul(argv[1], nullptr, 10);
	const size_t top = argc > 3 ? strtoul(argv[3], nullptr, 10) : 50;
	g_process        = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
	if (!g_process) { printf("OpenProcess failed %lu\n", GetLastError()); return 1; }
	if (const char* map = getenv("KYTY_MAP")) LoadMap(map);
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	if (!SymInitialize(g_process, nullptr, TRUE)) { printf("SymInitialize failed\n"); return 1; }
	FILE* file = fopen(argv[2], "rb");
	if (!file) { printf("cannot open %s\n", argv[2]); return 1; }
	std::vector<DWORD64> words;
	DWORD64 word;
	while (fread(&word, 8, 1, file) == 1) words.push_back(word);
	fclose(file);
	size_t count = words.size() / 16;
	// KYTY_PROF_RANGE=<first s>:<last s>: only samples of that part of the capture (the sampler
	// takes one every 250 us, so sample i is at about i / 4000 s).
	if (const char* range = getenv("KYTY_PROF_RANGE")) {
		double first = 0, last = 1e9;
		const double hz = getenv("KYTY_PROF_HZ") != nullptr ? atof(getenv("KYTY_PROF_HZ")) : 4000.0;
		if (sscanf(range, "%lf:%lf", &first, &last) == 2) {
			const size_t begin = static_cast<size_t>(first * hz), end = static_cast<size_t>(last * hz);
			if (begin < count) {
				const size_t stop = end < count ? end : count;
				words.erase(words.begin() + stop * 16, words.end());
				words.erase(words.begin(), words.begin() + begin * 16);
				count = stop - begin;
			}
		}
	}
	std::map<std::string, size_t> self, inclusive, self_busy, inclusive_busy, wait_callers;
	size_t busy = 0, waits = 0;
	for (size_t s = 0; s < count; ++s) {
		const DWORD64* sample = &words[s * 16];
		const std::string& leaf = Name(sample[0]);
		// The caller of a leaf is sample[1] when the leaf did not push a frame yet; the chain
		// starts at sample[2]. Inclusive counts each function once per sample.
		std::set<std::string> seen {leaf};
		for (int i = 1; i < 16; ++i)
			if (sample[i] != 0) {
				const std::string& caller = Name(sample[i]);
				if (caller != "?") seen.insert(caller);
			}
		bool waiting = IsWait(leaf);
		for (const auto& name: seen)
			if (IsWait(name)) waiting = true;
		++self[leaf];
		for (const auto& name: seen) ++inclusive[name];
		if (!waiting) {
			++busy;
			++self_busy[leaf];
			for (const auto& name: seen) ++inclusive_busy[name];
		} else {
			// The first two emulator frames above the wait: who waits, and from where.
			++waits;
			std::string who;
			int found = 0;
			for (int i = 1; i < 16 && found < 2; ++i) {
				if (sample[i] == 0) continue;
				const std::string& caller = Name(sample[i]);
				if (caller.rfind("kyty_emulator!", 0) == 0) {
					who += (found ? "  <-  " : "") + caller.substr(14, 70);
					++found;
				}
			}
			++wait_callers[who.empty() ? leaf : who];
		}
	}
	Print("self, all samples", self, count, top);
	Print("self, busy samples only", self_busy, busy, top);
	Print("inclusive, busy samples only", inclusive_busy, busy, top);
	Print("waiting samples by emulator caller", wait_callers, waits, 25);
	// focus: for leaves whose name contains the text, the hottest leaf addresses (module
	// offsets, for a disassembler) and the nearest callers.
	if (argc > 4) {
		std::map<std::string, size_t> addresses, callers;
		size_t matched = 0;
		for (size_t s = 0; s < count; ++s) {
			const DWORD64* sample = &words[s * 16];
			if (Name(sample[0]).find(argv[4]) == std::string::npos) continue;
			++matched;
			IMAGEHLP_MODULE64 module {};
			module.SizeOfStruct = sizeof(module);
			char text[128];
			if (SymGetModuleInfo64(g_process, sample[0], &module))
				snprintf(text, sizeof(text), "%s+0x%llx", module.ModuleName,
				         static_cast<unsigned long long>(sample[0] - module.BaseOfImage));
			else
				snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(sample[0]));
			++addresses[text];
			// KYTY_PROF_CHAIN=<n>: callers per chain (default 3); KYTY_PROF_EMU=1: emulator frames
			// only (past the allocator or system DLL frames of a leaf such as RtlAllocateHeap).
			static const int  chain_length = [] {
				const char* value = getenv("KYTY_PROF_CHAIN");
				const int   n     = value ? atoi(value) : 3;
				return n >= 1 && n <= 15 ? n : 3;
			}();
			static const bool emulator_only = getenv("KYTY_PROF_EMU") != nullptr;
			std::string chain;
			for (int i = 1, found = 0; i < 16 && found < chain_length; ++i) {
				if (sample[i] == 0) continue;
				const std::string& caller = Name(sample[i]);
				if (caller == "?") continue;
				if (emulator_only && caller.rfind("kyty_emulator!", 0) != 0) continue;
				chain += (found ? "  <-  " : "") + caller;
				++found;
			}
			++callers[chain];
		}
		Print("focus: leaf addresses", addresses, matched, top);
		Print("focus: callers", callers, matched, top);
	}
	return 0;
}
