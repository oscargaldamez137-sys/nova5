#pragma once

#include "common/assert.h"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string_view>

// Opt-in control-flow diagnostics. A loop guard may leave outputs unwritten;
// the SCC vote is an experimental port of Nova5's ConditionRef semantics.
// Read once before compilation. Both off leave emitted modules unchanged.
namespace ShaderLoopGuard {
struct Configuration {
    uint32_t limit = 0;
    uint64_t hash = 0; // KYTY_SHADER_LOOP_HASH selects either diagnostic; zero means all VS/PS
    uint32_t pc = UINT32_MAX; // optional guest loop header; dispatcher uses UINT32_MAX
    bool scc_vote = false;
    bool helper_exit = false; // KYTY_SHADER_LOOP_HELPER=1: helper invocations leave a guarded loop at once
    bool Active() const { return limit != 0 || scc_vote; }
    bool Matches(uint64_t candidate) const { return Active() && (hash == 0 || hash == candidate); }
};

inline const Configuration& Settings() {
    static const auto config = [] {
        Configuration result;
        const auto number = [](const char* name, int base, uint64_t fallback) {
            const char* value = std::getenv(name);
            if (value == nullptr || *value == 0) return fallback;
            std::string_view text(value);
            if (base == 16 && (text.starts_with("0x") || text.starts_with("0X"))) text.remove_prefix(2);
            uint64_t parsed = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed, base);
            if (error != std::errc{} || end != text.data() + text.size()) EXIT("Invalid %s=%s", name, value);
            return parsed;
        };
        const auto limit = number("KYTY_SHADER_LOOP_LIMIT", 10, 0);
        if (limit > 1000000) EXIT("KYTY_SHADER_LOOP_LIMIT must be 0..1000000");
        result.limit = static_cast<uint32_t>(limit);
        const auto vote = number("KYTY_SHADER_SCC_VOTE", 10, 0);
        if (vote > 1) EXIT("KYTY_SHADER_SCC_VOTE must be 0 or 1");
        result.scc_vote = vote != 0;
        result.helper_exit = number("KYTY_SHADER_LOOP_HELPER", 10, 0) != 0;
        if (result.Active()) {
            result.hash = number("KYTY_SHADER_LOOP_HASH", 16, 0);
            const auto pc = number("KYTY_SHADER_LOOP_PC", 16, UINT32_MAX);
            if (pc > UINT32_MAX) EXIT("KYTY_SHADER_LOOP_PC exceeds 32 bits");
            result.pc = static_cast<uint32_t>(pc);
        }
        return result;
    }();
    return config;
}
} // namespace ShaderLoopGuard
