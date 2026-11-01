#include "clean-read-cache.h"
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static void Check(bool ok) { if (!ok) std::abort(); }
int main() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    CleanReadCache::Table cache;
    CleanReadCache::Stamp stamp{&cache, 1, 1};
    bool dirty = false, image = false, hit = false;
    uint64_t probes = 0;
    auto proof = [&](uint64_t address, uint64_t size) {
        ++probes;
        return !image && (!dirty || address + size <= 0x1080 || address >= 0x1090);
    };
    Check(cache.Prove(0x1000, 4, stamp, proof, hit) && !hit && probes == 1);
    Check(cache.Prove(0x1080, 4, stamp, proof, hit) && hit && probes == 1);
    dirty = true; ++stamp.buffers;
    Check(!cache.Prove(0x1080, 4, stamp, proof, hit) && !hit);
    Check(cache.Prove(0x1000, 4, stamp, proof, hit) && !hit);
    Check(cache.Prove(0x1004, 4, stamp, proof, hit) && !hit);
    Check(cache.Prove(0x1000, 8, stamp, proof, hit) && hit); // adjacent proofs
    Check(!cache.Prove(0x1080, 4, stamp, proof, hit)); // no gap was certified
    image = true; ++stamp.images;
    Check(!cache.Prove(0x1000, 4, stamp, proof, hit));
    image = false; ++stamp.images;
    Check(cache.Prove(0x1000, 4, stamp, proof, hit));
    // Exact fallbacks remain necessary for cross-page, oversized and invalid queries.
    Check(!cache.Prove(0x1ffe, 4, stamp, proof, hit));
    Check(!cache.Prove(UINT64_MAX - 2, 4, stamp, proof, hit));
    Check(!cache.Prove(0x1000, 0, stamp, proof, hit));
    // A model of changing dirty coverage and image registrations: never false clean.
    for (uint64_t i = 0; i < 100000; ++i) {
        if (i % 11 == 0) { dirty = !dirty; ++stamp.buffers; }
        if (i % 37 == 0) { image = !image; ++stamp.images; }
        auto address = 0x1000 + (i * 17 % 256);
        if (cache.Prove(address, 4, stamp, proof, hit)) Check(proof(address, 4));
    }
    std::puts("CleanReadCacheTests: passed (100000 changing ownership queries)");
}
