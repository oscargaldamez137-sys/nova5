#pragma once

#include <cstdint>

namespace Libs::Graphics::TextureMemo {

// Phase 1 of Jetsku's TextureBindingMemo: immutable descriptor keys and tagged
// entries, followed by a live ownership/refresh proof at final view acquisition.
enum class Reason : uint8_t { Hit, Policy, Key, Token, NoView, Epoch, Owner, Alias, Dirty, Tracking, Command, Count };
inline constexpr const char* ReasonNames[] = {
    "hit", "policy", "key", "token", "no_view", "epoch", "owner", "alias", "dirty", "tracking", "command"};

struct Proof {
    uint64_t epoch = 0;
    uint64_t serial = 0; // ImageId slots can be reused; Image::serial cannot.
};

struct State {
    uint64_t serial = 0;
    bool owner = false;
    bool epoch_holds = false;
    bool alias = false;
    bool dirty = false;
    bool tracked = false;
};

template<class RegistrationsSince>
[[nodiscard]] bool EpochHolds(const Proof& proof, uint64_t current, RegistrationsSince&& changed) {
    return proof.epoch != 0 && (proof.epoch == current || !changed(proof.epoch));
}

[[nodiscard]] constexpr Reason Validate(const Proof& proof, const State& state) {
    if (!state.owner || state.serial == 0 || (proof.serial != 0 && proof.serial != state.serial))
        return Reason::Owner;
    if (!state.epoch_holds) return Reason::Epoch;
    if (state.alias) return Reason::Alias;
    if (state.dirty) return Reason::Dirty;
    if (!state.tracked) return Reason::Tracking;
    return Reason::Hit;
}

// A binding may survive a direct-mapped slot collision while other bindings are
// prepared. A tag, not the slot number alone, authorizes its saved view.
struct Token {
    uint64_t tag = 0;
    [[nodiscard]] constexpr bool Matches(uint64_t binding_tag) const {
        return tag != 0 && tag == binding_tag;
    }
    template<class Key>
    [[nodiscard]] constexpr bool MatchesKey(const Key& saved, const Key& requested) const {
        return tag != 0 && saved == requested;
    }
};

} // namespace Libs::Graphics::TextureMemo
