#pragma once
#include "common/pageWatchDiagnostics.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

// Cold-path diagnostics independent of the configured (possibly Silent) logger.
// Slots are published once and never reused: a fault can read them without the
// loader lock, allocation, or traversing a vector being changed by another thread.
namespace Common::Termination {
struct Module {
    std::atomic<bool> ready {false};
    std::atomic<bool> active {false};
    uint64_t base = 0, size = 0;
    char name[256] {};
};
inline std::array<Module, 1024> modules;
inline std::atomic<unsigned> module_count {0};

inline void ModuleLoaded(uint64_t base, uint64_t size, const char* name) {
    const auto slot = module_count.fetch_add(1, std::memory_order_relaxed);
    if (slot >= modules.size()) {
        std::fprintf(stdout, "[MODULE_MAP] capacity exceeded base=0x%llx name=%s\n",
                     (unsigned long long)base, name);
        std::fflush(stdout);
        return;
    }
    auto& m = modules[slot];
    m.base = base; m.size = size;
    std::snprintf(m.name, sizeof(m.name), "%s", name);
    m.active.store(true, std::memory_order_relaxed);
    m.ready.store(true, std::memory_order_release);
    std::fprintf(stdout, "[MODULE_MAP] load base=0x%llx size=0x%llx name=%s\n",
                 (unsigned long long)base, (unsigned long long)size, m.name);
    std::fflush(stdout);
}
inline void ModuleUnloaded(uint64_t base) {
    for (auto& m: modules) {
        if (m.ready.load(std::memory_order_acquire) && m.base == base && m.active.exchange(false)) {
            std::fprintf(stdout, "[MODULE_MAP] unload base=0x%llx name=%s\n",
                         (unsigned long long)base, m.name);
        }
    }
    std::fflush(stdout);
}
inline bool Describe(uint64_t address, char* text, size_t size) {
    // Newest mapping wins if an address was reused during a concurrent unload.
    auto count = module_count.load(std::memory_order_acquire);
    if (count > modules.size()) count = static_cast<unsigned>(modules.size());
    while (count != 0) {
        const auto& m = modules[--count];
        if (m.ready.load(std::memory_order_acquire) && m.active.load() &&
            address >= m.base && address - m.base < m.size) {
            std::snprintf(text, size, "%s+0x%llx", m.name, (unsigned long long)(address - m.base));
            return true;
        }
    }
    std::snprintf(text, size, "0x%llx", (unsigned long long)address);
    return false;
}
inline void Report(const char* cause, uint64_t code = 0, uint64_t caller = 0,
                   bool dump_modules = false, const char* origin = "unknown") {
    PageWatchDiagnostics::Report(cause);
    char where[300] {};
    Describe(caller, where, sizeof(where));
    std::fprintf(stdout, "[TERMINATION] cause=%s code=0x%llx caller=%s origin=%s\n",
                 cause, (unsigned long long)code, where, origin);
    if (dump_modules) {
        std::fprintf(stdout, "[MODULE_MAP] active mappings at termination:\n");
        for (const auto& m: modules) {
            if (m.ready.load(std::memory_order_acquire) && m.active.load())
                std::fprintf(stdout, "[MODULE_MAP] base=0x%llx size=0x%llx name=%s\n",
                             (unsigned long long)m.base, (unsigned long long)m.size, m.name);
        }
    }
    std::fflush(stdout);
}
} // namespace Common::Termination
