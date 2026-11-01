#pragma once

#include <array>
#include <cstdint>

// Positive ownership proofs only. Never stores bytes or host mappings. The
// caller certifies absence of images, and supplies epochs for new buffer writes
// and image registrations. Failed page proofs do NOT reject exact clean reads.
namespace CleanReadCache {
struct Stamp {
    const void* owner = nullptr;
    uint64_t buffers = 0, images = 0;
    bool operator==(const Stamp&) const = default;
};
class Table {
    struct Entry {
        Stamp stamp;
        uint64_t page = UINT64_MAX, begin = 0, end = 0;
        bool page_clean = false;
    };
    std::array<Entry, 64> entries{};
public:
    // proof must be side-effect-free. Success proves every byte clean even if
    // the host mapping does not exist; the caller still reads checked backing.
    template<class Proof>
    bool Prove(uint64_t address, uint64_t size, Stamp stamp, Proof&& proof, bool& hit) {
        hit = false;
        if (!size || size > 4096 || address > UINT64_MAX - size ||
            (address & 4095) + size > 4096) return false;
        const auto page = address & ~uint64_t{4095};
        auto& entry = entries[(page >> 12) % entries.size()];
        if (entry.page != page || !(entry.stamp == stamp)) {
            entry = {stamp, page, 0, 0, proof(page, 4096)};
            if (entry.page_clean) return true;
        } else if (entry.page_clean || (address >= entry.begin && address + size <= entry.end)) {
            hit = true;
            return true;
        }
        if (!proof(address, size)) return false;
        if (entry.end && address <= entry.end && address + size >= entry.begin) {
            entry.begin = address < entry.begin ? address : entry.begin;
            entry.end = address + size > entry.end ? address + size : entry.end;
        } else {
            entry.begin = address;
            entry.end = address + size;
        }
        return true;
    }
};
} // namespace CleanReadCache
