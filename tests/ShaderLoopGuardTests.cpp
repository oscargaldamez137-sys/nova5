#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "local/shader-loop-guard.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include <spirv-tools/libspirv.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

using namespace Libs::Graphics;
static void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "ShaderLoopGuardTests: %s\n", message); std::exit(1); }
}

// Execute only the integer/control-flow subset used by the resource-free scalar
// fixture below. This checks actual generated control flow without a Vulkan device.
// Unknown instructions fail rather than being silently approximated.
static uint32_t Execute(const std::vector<uint32_t>& words) {
    std::unordered_map<uint32_t, uint32_t> values, memory;
    std::unordered_map<uint32_t, std::array<uint32_t, 2>> pairs;
    std::unordered_map<uint32_t, size_t> labels;
    uint32_t main = 0, function = 0, budget_id = 0;
    size_t entry = 0;
    for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
        const auto op = words[i] & 65535;
        const auto* w = words.data() + i;
        if (op == 15) main = w[2]; // OpEntryPoint
        if (op == 5 && std::string(reinterpret_cast<const char*>(w + 2)) == "diagnostic_loop_budget") budget_id = w[1];
        if (op == 41 || op == 42) values[w[2]] = op == 41;
        if (op == 43) values[w[2]] = w[3];
        if (op == 54) function = w[2];
        if (op == 248 && function == main) { labels[w[1]] = i; if (!entry) entry = i; }
    }
    Check(entry != 0, "missing entry block");
    uint32_t from = 0, current = 0;
    size_t at = entry;
    for (unsigned steps = 0; steps < 10000; ++steps) {
        const auto* w = words.data() + at;
        const auto op = w[0] & 65535, size = w[0] >> 16;
        const auto get = [&](unsigned i) { Check(values.contains(w[i]), "undefined fixture value"); return values.at(w[i]); };
        switch (op) {
        case 248: { // PHIs on entry are parallel copies.
            current = w[1]; at += size;
            const auto incoming = values;
            while ((words[at] & 65535) == 245) {
                const auto* phi = words.data() + at;
                bool found = false;
                for (unsigned j = 3; j < phi[0] >> 16; j += 2) if (phi[j + 1] == from) {
                    Check(incoming.contains(phi[j]), "undefined PHI input");
                    values[phi[2]] = incoming.at(phi[j]); found = true;
                }
                Check(found, "wrong predecessor after guard insertion"); at += phi[0] >> 16;
            }
            continue;
        }
        case 59: memory[w[2]] = size > 4 ? get(4) : 0; break;
        case 61: Check(memory.contains(w[3]), "unknown pointer"); values[w[2]] = memory.at(w[3]); break;
        case 62: memory[w[1]] = get(2); break;
        case 128: values[w[2]] = get(3) + get(4); break;
        case 149: { // OpIAddCarry
            const uint64_t sum = uint64_t{get(3)} + get(4);
            pairs[w[2]] = {static_cast<uint32_t>(sum), static_cast<uint32_t>(sum >> 32)}; break;
        }
        case 81: Check(w[4] < 2, "unexpected composite index"); values[w[2]] = pairs.at(w[3])[w[4]]; break;
        case 170: values[w[2]] = get(3) == get(4); break;
        case 171: values[w[2]] = get(3) != get(4); break;
        case 176: values[w[2]] = get(3) < get(4); break;
        case 168: values[w[2]] = !get(3); break;
        case 166: values[w[2]] = get(3) || get(4); break;
        case 167: values[w[2]] = get(3) && get(4); break;
        case 169: values[w[2]] = get(3) ? get(4) : get(5); break;
        case 246: case 247: break; // structured merge declarations
        case 249: from = current; at = labels.at(w[1]); continue;
        case 250: from = current; at = labels.at(get(1) ? w[2] : w[3]); continue;
        case 253: return budget_id == 0 ? 0 : memory.at(budget_id);
        default: std::fprintf(stderr, "unexpected fixture opcode %u\n", op); Check(false, "unsupported fixture instruction");
        }
        at += size;
    }
    Check(false, "fixture failed to terminate within 10000 host steps"); return 0;
}

int main(int argc, char** argv) {
    Common::InitializeThreads(); Common::Subsystems subsystems;
    subsystems.Initialize<Config::Lifecycle>(); Config::ConfigOptions config;
    config.printf_direction = Config::OutputDirection::Silent; Config::Load(config);
    subsystems.Initialize<Log::Lifecycle>();
    // s0=0; while(s0<3) ++s0; end. Header PC=4. Carries a PHI through a backedge.
    const uint32_t code[] = {0xbe800380, 0xbf0a8300, 0xbf840002, 0x80008100, 0xbf82fffc, 0xbf810000};
    ShaderPixelInputInfo pixel{}; ShaderRecompiler::CompileOptions options;
    options.stage = ShaderType::Pixel; options.input_info.pixel = &pixel; options.shader_hash = 0x1234;
    auto translated = ShaderRecompiler::TranslateProgram(code, options);
    const auto& settings = ShaderLoopGuard::Settings();
    // The SCC candidate must affect the branch IR, and only for the selected hash.
    const bool expect_vote = settings.scc_vote && settings.Matches(options.shader_hash);
    bool has_vote = false;
    for (const auto* block: translated.program.blocks) for (const auto& inst: *block)
        has_vote |= inst.GetOpcode() == ShaderRecompiler::IR::ValueOpcode::AnyLane;
    Check(has_vote == expect_vote, "SCC vote selection mismatch");
    if (argc > 2 && std::string(argv[2]) == "dispatcher") translated.program.dispatcher_fallback = true;
    auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, {}, 0);
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char* m) { std::fprintf(stderr, "%s\n", m); });
    Check(validator.Validate(compiled.spirv), "guarded SPIR-V is invalid");
    if (argc > 1) { std::ofstream file(argv[1], std::ios::binary); file.write(reinterpret_cast<const char*>(compiled.spirv.data()), compiled.spirv.size()*4); }
    if (!expect_vote && !compiled.program.dispatcher_fallback) {
        const bool selected = settings.limit != 0 && settings.Matches(options.shader_hash) &&
                              (settings.pc == UINT32_MAX || settings.pc == 4);
        const auto visits = Execute(compiled.spirv);
        Check(visits == (selected ? std::min(settings.limit + 1, 4u) : 0u), "guard did not bound header visits");
    }
    std::puts("ShaderLoopGuardTests: passed (host only)"); subsystems.Destroy();
}
