#include "graphics/host_gpu/renderer/pipeline/textureBindingMemoPolicy.h"
#include <array>
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

using namespace Libs::Graphics::TextureMemo;
static unsigned checks = 0;
static void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "TextureBindingMemoTests: FAILED %s\n", name); std::abort(); }
}

int main() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    Token token{1};
    const uint64_t prepared_tag = token.tag;
    std::array<uint32_t, 8> descriptor{0x1000, 1, 2, 3, 4, 5, 6, 7};
    const auto saved = descriptor;
    Check(token.MatchesKey(saved, descriptor), "unchanged descriptor");
    for (size_t word = 0; word < descriptor.size(); ++word) {
        descriptor[word] ^= 0x100;
        Check(!token.MatchesKey(saved, descriptor), "rewrite any descriptor word");
        descriptor[word] ^= 0x100;
    }
    Check(token.Matches(prepared_tag), "prepared binding owns entry");
    ++token.tag; // Another binding overwrites the direct-mapped slot, then restores the same descriptor.
    Check(token.MatchesKey(saved, descriptor) && !token.Matches(prepared_tag), "ABA slot collision");
    token = {};
    Check(!token.Matches(0) && !token.MatchesKey(saved, descriptor), "unpublished entry");

    const Proof proof{10, 123};
    const State clean{123, true, true, false, false, true};
    Check(Validate(proof, clean) == Reason::Hit, "clean registered image");
    auto state = clean;
    state.owner = false;
    Check(Validate(proof, state) == Reason::Owner, "image unregistered or requires rediscovery");
    state = clean; ++state.serial;
    Check(Validate(proof, state) == Reason::Owner, "slot reused for another image");
    state = clean; state.alias = true;
    Check(Validate(proof, state) == Reason::Alias, "alias owner changes after preparation");
    state = clean; state.dirty = true;
    Check(Validate(proof, state) == Reason::Dirty, "CPU/buffer/stencil dirty after preparation");
    state = clean; state.tracked = false;
    Check(Validate(proof, state) == Reason::Tracking, "partial watch or untracked holes");
    state = clean; state.epoch_holds = false;
    Check(Validate(proof, state) == Reason::Epoch, "overlapping registration");
    // Dirty -> refreshed -> clean does not invalidate an immutable VkImageView;
    // the live proof must refuse it while dirty, then may use it again.
    state.dirty = true; state.epoch_holds = true;
    Check(Validate(proof, state) != Reason::Hit, "dirty phase");
    state.dirty = false;
    Check(Validate(proof, state) == Reason::Hit, "after full refresh");

    unsigned page_walks = 0;
    uint64_t last_overlap = 0;
    auto changed = [&](uint64_t epoch) { ++page_walks; return last_overlap > epoch; };
    Check(EpochHolds(proof, 10, changed) && page_walks == 0, "same epoch avoids page walk");
    Check(EpochHolds(proof, 11, changed) && page_walks == 1, "unrelated registration remains valid");
    last_overlap = 11;
    Check(!EpochHolds(proof, 11, changed), "overlapping registration invalidates");
    Check(!EpochHolds(Proof{}, 11, changed), "missing proof cannot validate");
    Proof refreshed{11, 123};
    Check(EpochHolds(refreshed, 12, changed), "new certificate after complete acquisition");
    last_overlap = 12; // Unregister/re-register the same object: serial unchanged, epoch must refuse.
    Check(!EpochHolds(refreshed, 12, changed), "parked image reregistered");

    // Exhaust live combinations so simultaneous invalidations never mask a refusal.
    for (unsigned bits = 0; bits < 64; ++bits) {
        state = {bits & 32 ? 124u : 123u, bool(bits & 1), bool(bits & 2),
                 bool(bits & 4), bool(bits & 8), bool(bits & 16)};
        const bool expected = bits == (1 | 2 | 16);
        Check((Validate(proof, state) == Reason::Hit) == expected, "combined invalidations");
    }
    std::printf("TextureBindingMemoTests: %u checks passed\n", checks);
}
