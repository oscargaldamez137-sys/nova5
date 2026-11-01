#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <cstring>
#include <type_traits>

// Opt-in CPU submission metadata, not a GPU readback or proof of completion.
// A bounded history permits matching a reader to overlapping writer bindings,
// fills and copies without touching protected guest memory or waiting for the GPU.
namespace ResourceProvenance {
inline bool Enabled() {
    static const bool enabled = [] { const char* s = std::getenv("KYTY_RESOURCE_TRACE"); return s && s[0] == '1' && s[1] == 0; }();
    return enabled;
}
struct Event {
    uint64_t sequence = 0, tick = 0, address = 0, size = 0, hash = 0, source = 0;
    uint64_t timeline = 0, buffer = 0, offset = 0, range = 0, adjustment = 0;
    uint64_t source_buffer = 0, source_offset = 0;
    uint32_t stage = 0, binding = 0, value = 0;
    const char* kind = "binding-write";
    bool gds = false;
};
inline std::mutex mutex;
inline std::array<Event, 1024> history;
inline uint64_t sequence = 0, reads = 0, candidates = 0;
inline std::array<Event, 2> watched; // Latest heads/nodes ranges, learned from shader bindings.
inline uint64_t cache_reports = 0;
template<class Handle> uint64_t HandleId(Handle handle) {
    using Native = typename Handle::CType;
    if constexpr (std::is_pointer_v<Native>) return reinterpret_cast<uint64_t>(static_cast<Native>(handle));
    else return static_cast<uint64_t>(static_cast<Native>(handle));
}
inline bool Overlaps(uint64_t address, uint64_t size, const Event& e) {
    return size && e.size && (e.address <= address ? address - e.address < e.size
                                                 : e.address - address < size);
}
template<class Scheduler, class Buffer>
Event BufferEvent(Scheduler& scheduler, const Buffer& buffer, uint64_t offset, uint64_t size, const char* kind) {
    Event e; e.tick = scheduler.CurrentTick(); e.timeline = HandleId(scheduler.GetMasterSemaphore().Handle());
    e.address = buffer.CpuAddress() + offset; e.size = size; e.buffer = HandleId(buffer.Handle());
    e.offset = offset; e.range = size; e.kind = kind;
    return e;
}
inline Event Add(Event e) {
    e.sequence = ++sequence;
    history[(sequence - 1) % history.size()] = e;
    return e;
}
inline void Print(const Event& e, const char* label = "prior") {
    std::printf("[RESOURCE_TRACE] %s seq=%llu timeline=0x%llx tick=%llu kind=%s stage=%u hash=%016llx b=%u %s=0x%llx+0x%llx source=0x%llx vk-buffer=0x%llx offset=0x%llx range=0x%llx adjustment=0x%llx",
        label, (unsigned long long)e.sequence, (unsigned long long)e.timeline, (unsigned long long)e.tick, e.kind, e.stage,
        (unsigned long long)e.hash, e.binding, e.gds ? "gds" : "guest",
        (unsigned long long)e.address, (unsigned long long)e.size, (unsigned long long)e.source,
        (unsigned long long)e.buffer, (unsigned long long)e.offset,
        (unsigned long long)e.range, (unsigned long long)e.adjustment);
    // Only a fill has a known value. A binding-write is NOT a sample of GPU contents.
    if (std::strcmp(e.kind, "fill") == 0) std::printf(" fill-value=0x%08x", e.value);
    if (e.source_buffer) std::printf(" source-buffer=0x%llx source-offset=0x%llx",
        (unsigned long long)e.source_buffer, (unsigned long long)e.source_offset);
    std::putchar('\n');
}
// Upload events describe staging preparation, not GPU execution. Routing events below
// distinguish copies moved to a submission prologue from in-order copies.
inline void Cache(Event e) {
    if (!Enabled()) return;
    std::lock_guard lock(mutex);
    e = Add(e);
    if (Overlaps(e.address, e.size, watched[0]) || Overlaps(e.address, e.size, watched[1])) {
        if (cache_reports++ < 512) Print(e, "cache");
        else if (cache_reports == 513) std::puts("[RESOURCE_TRACE] cache report limit=512 reached; ring history still active");
    }
}
inline void Transfer(uint64_t tick, uint64_t address, uint64_t size, bool gds,
                     const char* kind, uint32_t value = 0, uint64_t source = 0, uint64_t timeline = 0) {
    if (!Enabled()) return;
    std::lock_guard lock(mutex);
    Event e; e.tick = tick; e.address = address; e.size = size; e.gds = gds;
    e.kind = kind; e.value = value; e.source = source; e.timeline = timeline; Add(e);
}
inline bool Sample(uint64_t n) { return n <= 8 || (n <= 4096 && (n & (n - 1)) == 0); }
template<class Bindings> void Record(const Bindings& bindings, uint64_t tick, uint64_t timeline) {
    if (!Enabled() || !bindings.runtime || !bindings.runtime->program) return;
    const auto& p = *bindings.runtime->program;
    std::lock_guard lock(mutex);
    const bool reader = p.shader_hash == 0x5457609506174517ull && Sample(++reads);
    const bool candidate = p.shader_hash == 0x6216dac0cc17c2d4ull && Sample(++candidates);
    if (reader || candidate) {
        const auto& data = bindings.runtime->resources.user_data;
        std::printf("[RESOURCE_TRACE] bind timeline=0x%llx tick=%llu stage=%u hash=%016llx user14=0x%08x metadata-only\n",
            (unsigned long long)timeline, (unsigned long long)tick, (unsigned)p.stage, (unsigned long long)p.shader_hash,
            data.size() > 14 ? data[14] : 0);
    }
    for (size_t i = 0; i < bindings.buffer_sources.size() && i < p.info.buffers.size(); ++i) {
        const auto& s = bindings.buffer_sources[i];
        const auto& r = p.info.buffers[i];
        Event current; current.tick = tick; current.timeline = timeline;
        current.address = s.address; current.size = s.size; current.hash = p.shader_hash;
        current.stage = unsigned(p.stage); current.binding = unsigned(i);
        if (i < bindings.buffers.size()) {
            const auto& native = bindings.buffers[i];
            current.buffer = HandleId(native.buffer); current.offset = native.offset; current.range = native.range;
            const auto dword = p.bindings.memory_offset_dword + i / 4;
            if (dword < bindings.shader_data.size()) current.adjustment = (bindings.shader_data[dword] >> ((i % 4) * 8)) & 255u;
        }
        if (p.shader_hash == 0x6216dac0cc17c2d4ull && (i == 3 || i == 6)) watched[i == 6 ? 0 : 1] = current;
        if (p.shader_hash == 0x5457609506174517ull && (i == 11 || i == 12)) watched[i == 11 ? 0 : 1] = current;
        if (candidate || (reader && (i == 11 || i == 12))) {
            std::printf("[RESOURCE_TRACE] buffer hash=%016llx b=%zu descriptor=%u first-pc=0x%x guest=0x%llx+0x%llx written=%u\n",
                (unsigned long long)p.shader_hash, i, r.source, r.first_use_pc,
                (unsigned long long)s.address, (unsigned long long)s.size, unsigned(r.written));
            current.kind = "binding"; Print(current, "resolved");
            unsigned matches = 0;
            const bool match = reader || (candidate && (i == 3 || i == 6));
            for (size_t n = 0; match && n < history.size() && n < sequence; ++n) {
                const auto& e = history[(sequence - 1 - n) % history.size()];
                if (!e.gds && Overlaps(s.address, s.size, e) && matches++ < 16) Print(e);
            }
            if (match && !matches) std::puts("[RESOURCE_TRACE] no overlapping event in last 1024 metadata events");
        }
        if (r.written && s.size) {
            current.kind = "binding-write"; Add(current);
        }
    }
    if (candidate) {
        unsigned matches = 0;
        for (size_t n = 0; n < history.size() && n < sequence && matches < 8; ++n) {
            const auto& e = history[(sequence - 1 - n) % history.size()];
            if (e.gds) { Print(e); ++matches; }
        }
    }
}
} // namespace ResourceProvenance
