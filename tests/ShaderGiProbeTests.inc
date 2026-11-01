// Astro Bot GI-probe path tests (included by ShaderRecompilerComputeTests.cpp after
// the Vulkan harness).
//
// The ray-bundle GI passes (GfxGiBox PS 0x4a55033774c8f01e and the 222 material ray-bundle pixel
// shaders) build per-pixel fragment linked lists: DS_APPEND on a GDS counter gives each fragment
// a node index (base + V_MBCNT of EXEC), IMAGE_ATOMIC_SWAP links it into the pixel's list, and
// GfxGiProbeTraceLinkedList (CS 0x657ad04626bf9d55) walks the lists until index 0. A duplicate
// node index can link a list to itself, and the unbounded walk then hangs the GPU.

namespace GiProbeTests {

using ShaderRecompiler::GetCodegenOptions;
using ShaderRecompiler::PsLiveExec;

struct ScopedCodegenOptions {
  ShaderRecompiler::CodegenOptions saved = ShaderRecompiler::GetCodegenOptions();
  explicit ScopedCodegenOptions(const ShaderRecompiler::CodegenOptions &options) {
    ShaderRecompiler::SetCodegenOptions(options);
  }
  ~ScopedCodegenOptions() { ShaderRecompiler::SetCodegenOptions(saved); }
};

size_t CountSpirv(const std::vector<u32> &words, std::string_view needle) {
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  std::string text;
  Require("GiProbeCodegen", "SPIR-V", tools.Disassemble(words, &text), "disassembly failed");
  size_t count = 0;
  for (size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size()) count++;
  return count;
}

// The GfxGiBox-shaped append: M0 = GDS range, DS_APPEND (GDS) at offset 0, index = base +
// MBCNT(EXEC), exported as a float so the target holds it exactly.
std::vector<u32> MakePixelAppendIndexShader(u32 gds_m0) {
  std::vector<u32> code;
  AppendSMovLiteral(&code, 124u, gds_m0);
  code.push_back(EncodeDs0(0x3eu, 0u, true));
  code.push_back(EncodeDs1(1u, 0u, 0u));
  code.push_back(EncodeVop1(0x01u, 3u, InlineU32(0u)));
  code.push_back(EncodeVop2(0x23u, 2u, 126u, 3u));
  code.push_back(EncodeVop2(0x24u, 2u, 127u, 2u));
  code.push_back(EncodeVop2(0x25u, 1u, Vgpr(1u), 2u));
  code.push_back(EncodeVop1(0x06u, 1u, Vgpr(1u)));
  code.push_back(EncodeExp0(0u, 0xfu, true, false, true));
  code.push_back(EncodeExp1(1u, 1u, 1u, 1u));
  AppendEnd(&code);
  return code;
}

// One small triangle around the centre of every pixel (x, y) in `pixels`, in NDC for a
// width x height viewport. Each triangle covers exactly that pixel's centre.
std::vector<u32> MakePixelTriangles(std::span<const std::pair<u32, u32>> pixels, u32 width,
                                    u32 height) {
  std::vector<u32> vertices;
  const auto push = [&](float x, float y) {
    const float ndc_x = 2.0f * x / static_cast<float>(width) - 1.0f;
    const float ndc_y = 2.0f * y / static_cast<float>(height) - 1.0f;
    for (const float value : {ndc_x, ndc_y, 0.0f, 0.0f, 0.0f, 1.0f}) {
      vertices.push_back(std::bit_cast<u32>(value));
    }
  };
  for (const auto &[x, y] : pixels) {
    const auto fx = static_cast<float>(x);
    const auto fy = static_cast<float>(y);
    push(fx + 0.2f, fy + 0.2f);
    push(fx + 0.8f, fy + 0.2f);
    push(fx + 0.5f, fy + 0.8f);
  }
  return vertices;
}

struct AppendOutcome {
  u32 covered = 0;
  u32 duplicates = 0;
  u32 out_of_range = 0;
  u32 unexpected_coverage = 0;
  u32 counter = 0;
};

AppendOutcome RunPixelAppend(VulkanHarness *vulkan, const char *name, bool live_election,
                             std::span<const std::pair<u32, u32>> pixels, u32 width,
                             u32 height, u32 initial_counter,
                             PsLiveExec live_exec = PsLiveExec::Off) {
  auto options = GetCodegenOptions();
  options.ps_append_live_election = live_election;
  options.ps_live_exec = live_exec;
  ScopedCodegenOptions scoped(options);

  GraphicsCase test;
  test.name = name;
  test.fragment_code = MakePixelAppendIndexShader(0x00000100u);
  const auto compiled = CompileFragmentCase(test);
  const auto helper_decorations = CountSpirv(compiled.spirv, "BuiltIn HelperInvocation");
  Require(name, "SPIR-V",
          helper_decorations == (live_election || live_exec != PsLiveExec::Off ? 1u : 0u),
          "gl_HelperInvocation declaration does not follow KYTY_PS_APPEND_LIVE_ELECTION and "
          "KYTY_PS_LIVE_EXEC");

  std::vector<u32> gds_initial(64u, 0u);
  gds_initial[0] = initial_counter;
  auto gds = vulkan->CreateStorageBuffer(name, gds_initial, gds_initial.size());
  const auto vertices = MakePixelTriangles(pixels, width, height);
  const auto words = vulkan->RenderFragmentWithGds(name, compiled, width, height, vertices, gds);
  AppendOutcome outcome;
  outcome.counter = vulkan->ReadBuffer(name, gds, 1u)[0];
  vulkan->DestroyBuffer(&gds);

  std::set<u32> seen;
  for (u32 y = 0; y < height; y++) {
    for (u32 x = 0; x < width; x++) {
      const float value = std::bit_cast<float>(words[(y * width + x) * 4u]);
      const bool expected = std::ranges::any_of(
          pixels, [&](const auto &pixel) { return pixel.first == x && pixel.second == y; });
      if (value == -1.0f) {
        outcome.unexpected_coverage += expected ? 1u : 0u;
        continue;
      }
      if (!expected) {
        outcome.unexpected_coverage++;
        continue;
      }
      outcome.covered++;
      const auto index = static_cast<u32>(value);
      if (static_cast<float>(index) != value || index < initial_counter ||
          index >= outcome.counter) {
        outcome.out_of_range++;
      }
      if (!seen.insert(index).second) {
        outcome.duplicates++;
      }
    }
  }
  return outcome;
}

// Only the lower-right pixel of every 2x2 quad is covered, so the first EXEC lane of every quad
// (and of every host subgroup) is a helper invocation.
void CheckPixelAppendHelperElection(VulkanHarness *vulkan) {
  constexpr const char *name = "PixelAppendHelperElection";
  constexpr u32 width = 16;
  constexpr u32 height = 16;
  constexpr u32 initial = 100;
  std::vector<std::pair<u32, u32>> pixels;
  for (u32 y = 1; y < height; y += 2) {
    for (u32 x = 1; x < width; x += 2) {
      pixels.emplace_back(x, y);
    }
  }
  const auto off = RunPixelAppend(vulkan, name, false, pixels, width, height, initial);
  std::printf("[graphics] %-31s switch off: covered %u, duplicate indices %u, out of range %u, "
              "counter %u -> %u\n",
              name, off.covered, off.duplicates, off.out_of_range, initial, off.counter);
  const auto on = RunPixelAppend(vulkan, name, true, pixels, width, height, initial);
  Require(name, "coverage", on.covered == pixels.size() && on.unexpected_coverage == 0,
          "the test triangles did not cover exactly one pixel each");
  Require(name, "append", on.duplicates == 0 && on.out_of_range == 0,
          "a pixel-shader DS_APPEND returned duplicate or out-of-range indices");
  Require(name, "counter", on.counter >= initial + pixels.size(),
          "the GDS append counter advanced by less than the live lane count");
  std::printf("[graphics] %-31s ok (switch on: %u unique indices, counter %u -> %u)\n", name,
              on.covered, initial, on.counter);
}

// KYTY_PS_LIVE_EXEC: with EXEC starting at the non-helper invocations, the append counts and
// indexes exactly the covered pixels, as on the PS5 (no slot is left for a helper lane).
void CheckPixelAppendLiveExec(VulkanHarness *vulkan) {
  constexpr const char *name = "PixelAppendLiveExec";
  constexpr u32 width = 16;
  constexpr u32 height = 16;
  constexpr u32 initial = 100;
  std::vector<std::pair<u32, u32>> pixels;
  for (u32 y = 1; y < height; y += 2) {
    for (u32 x = 1; x < width; x += 2) {
      pixels.emplace_back(x, y);
    }
  }
  const auto count = static_cast<u32>(pixels.size());
  for (const bool live_election : {false, true}) {
    const auto on = RunPixelAppend(vulkan, name, live_election, pixels, width, height, initial,
                                   PsLiveExec::AppendConsume);
    Require(name, "coverage", on.covered == count && on.unexpected_coverage == 0,
            "the test triangles did not cover exactly one pixel each");
    Require(name, "append", on.duplicates == 0 && on.out_of_range == 0,
            "a pixel-shader DS_APPEND returned duplicate or out-of-range indices");
    Require(name, "counter", on.counter == initial + count,
            "the GDS append counter did not advance by exactly the covered pixel count");
  }
  std::printf("[graphics] %-31s ok (%u unique indices, counter %u -> %u)\n", name, count, initial,
              initial + count);
}

// Saves EXEC, switches to whole quad mode and exports (S_BCNT1 of the WQM EXEC, S_BCNT1 of the
// saved EXEC) as floats under the saved EXEC.
std::vector<u32> MakePixelWqmCountShader() {
  return {
      EncodeSop1(0x04u, 0u, 126u),   // s_mov_b64 s[0:1], exec
      EncodeSop1(0x0au, 126u, 126u), // s_wqm_b64 exec, exec
      EncodeSop1(0x10u, 2u, 126u),   // s_bcnt1_i32_b64 s2, exec
      EncodeSop1(0x10u, 3u, 0u),     // s_bcnt1_i32_b64 s3, s[0:1]
      EncodeVop1(0x01u, 1u, 2u),     // v_mov_b32 v1, s2
      EncodeVop1(0x06u, 1u, Vgpr(1u)),
      EncodeVop1(0x01u, 2u, 3u),     // v_mov_b32 v2, s3
      EncodeVop1(0x06u, 2u, Vgpr(2u)),
      EncodeSop1(0x04u, 126u, 0u),   // s_mov_b64 exec, s[0:1]
      EncodeExp0(0u, 0xfu, true, false, true),
      EncodeExp1(1u, 2u, 1u, 1u),
      0xbf810000u,
  };
}

// KYTY_PS_LIVE_EXEC=all: every quad of the test has exactly one covered pixel, so the saved
// (live) EXEC counts one lane per quad of the subgroup and S_WQM_B64 four; with the switch off
// both count every lane.
void CheckPixelWqmLiveExec(VulkanHarness *vulkan) {
  constexpr const char *name = "PixelWqmLiveExec";
  constexpr u32 width = 16;
  constexpr u32 height = 16;
  std::vector<std::pair<u32, u32>> pixels;
  for (u32 y = 0; y < height; y += 2) {
    for (u32 x = 0; x < width; x += 2) {
      pixels.emplace_back(x + ((x / 2u + y / 2u) & 1u), y + ((y / 2u) & 1u));
    }
  }
  const auto vertices = MakePixelTriangles(pixels, width, height);
  for (const auto mode : {PsLiveExec::Off, PsLiveExec::All}) {
    auto options = GetCodegenOptions();
    options.ps_live_exec = mode;
    ScopedCodegenOptions scoped(options);
    GraphicsCase test;
    test.name = name;
    test.fragment_code = MakePixelWqmCountShader();
    const auto compiled = CompileFragmentCase(test);
    const std::vector<u32> gds_initial(16u, 0u);
    auto gds = vulkan->CreateStorageBuffer(name, gds_initial, gds_initial.size());
    const auto words = vulkan->RenderFragmentWithGds(name, compiled, width, height, vertices, gds);
    vulkan->DestroyBuffer(&gds);
    u32 covered = 0;
    u32 exact = 0;
    u32 min_live = ~0u;
    u32 max_live = 0;
    for (const auto &[x, y] : pixels) {
      const float wqm_value = std::bit_cast<float>(words[(y * width + x) * 4u + 0u]);
      const float live_value = std::bit_cast<float>(words[(y * width + x) * 4u + 1u]);
      if (wqm_value == -1.0f) {
        continue;
      }
      covered++;
      const auto wqm = static_cast<u32>(wqm_value);
      const auto live = static_cast<u32>(live_value);
      exact += (live >= 1u && wqm == 4u * live) ? 1u : 0u;
      min_live = std::min(min_live, live);
      max_live = std::max(max_live, live);
    }
    Require(name, "coverage", covered == pixels.size(),
            "the test triangles did not cover exactly one pixel each");
    if (mode == PsLiveExec::Off) {
      std::printf("[graphics] %-31s switch off: %u of %u pixels see WQM lanes = 4 x live lanes "
                  "(live %u..%u)\n",
                  name, exact, covered, min_live, max_live);
      continue;
    }
    Require(name, "exec", exact == covered,
            "the initial EXEC did not hold exactly the covered pixels, or S_WQM_B64 did not add "
            "their quads");
    std::printf("[graphics] %-31s ok (switch all: WQM lanes = 4 x live lanes, live %u..%u per "
                "subgroup)\n",
                name, min_live, max_live);
  }
}

// CPU only: KYTY_PS_LIVE_EXEC=1 reads gl_HelperInvocation in pixel shaders with DS_APPEND or
// DS_CONSUME, =all in every pixel shader.
void CheckPixelLiveExecCodegen() {
  constexpr const char *name = "PixelLiveExecCodegen";
  const ShaderRecompiler::CodegenOptions defaults;
  Require(name, "defaults",
          defaults.ps_live_exec == PsLiveExec::AppendConsume && defaults.ps_append_live_election,
          "KYTY_PS_LIVE_EXEC=1 and KYTY_PS_APPEND_LIVE_ELECTION=1 are not the defaults");
  for (const auto mode : {PsLiveExec::Off, PsLiveExec::AppendConsume, PsLiveExec::All}) {
    auto options = GetCodegenOptions();
    options.ps_append_live_election = false;
    options.ps_live_exec = mode;
    ScopedCodegenOptions scoped(options);
    GraphicsCase append;
    append.name = name;
    append.fragment_code = MakePixelAppendIndexShader(0x00000100u);
    GraphicsCase plain;
    plain.name = name;
    plain.fragment_code = MakePixelWqmCountShader();
    const auto append_helpers =
        CountSpirv(CompileFragmentCase(append).spirv, "BuiltIn HelperInvocation");
    const auto plain_helpers =
        CountSpirv(CompileFragmentCase(plain).spirv, "BuiltIn HelperInvocation");
    Require(name, "SPIR-V", append_helpers == (mode == PsLiveExec::Off ? 0u : 1u),
            "the append pixel shader does not follow KYTY_PS_LIVE_EXEC");
    append.fragment_code[2] = EncodeDs0(0x3du, 0u, true); // DS_CONSUME
    Require(name, "SPIR-V", CountSpirv(CompileFragmentCase(append).spirv, "BuiltIn HelperInvocation") ==
                (mode == PsLiveExec::Off ? 0u : 1u),
            "the consume pixel shader does not follow KYTY_PS_LIVE_EXEC");
    Require(name, "SPIR-V", plain_helpers == (mode == PsLiveExec::All ? 1u : 0u),
            "a pixel shader without DS_APPEND does not follow KYTY_PS_LIVE_EXEC");
  }
  std::printf("[host]    %-32s ok\n", name);
}

// CPU only: the switch adds gl_HelperInvocation to pixel shaders with DS_APPEND/DS_CONSUME and
// to nothing else.
void CheckPixelAppendElectionCodegen() {
  constexpr const char *name = "PixelAppendElectionCodegen";
  for (const bool live_election : {false, true}) {
    auto options = GetCodegenOptions();
    options.ps_append_live_election = live_election;
    options.ps_live_exec = PsLiveExec::Off;
    ScopedCodegenOptions scoped(options);
    GraphicsCase test;
    test.name = name;
    test.fragment_code = MakePixelAppendIndexShader(0x00000100u);
    const auto compiled = CompileFragmentCase(test);
    Require(name, "SPIR-V",
            CountSpirv(compiled.spirv, "BuiltIn HelperInvocation") ==
                (live_election ? 1u : 0u),
            "gl_HelperInvocation declaration does not follow KYTY_PS_APPEND_LIVE_ELECTION");
    test.fragment_code[2] = EncodeDs0(0x3du, 0u, true);
    const auto consume = CompileFragmentCase(test);
    Require(name, "SPIR-V", CountSpirv(consume.spirv, "BuiltIn HelperInvocation") == (live_election ? 1u : 0u) &&
                CountSpirv(consume.spirv, "OpAtomicISub") == 1u,
            "DS_CONSUME does not follow live election or lost its subtraction");
    GraphicsCase plain;
    plain.name = name;
    plain.fragment_code = {EncodeVop1(0x01u, 1u, InlineU32(0u)), EncodeExp0(0u, 0xfu, true, false, true),
                           EncodeExp1(1u, 1u, 1u, 1u), 0xbf810000u};
    const auto plain_compiled = CompileFragmentCase(plain);
    Require(name, "SPIR-V",
            CountSpirv(plain_compiled.spirv, "BuiltIn HelperInvocation") == 0u,
            "a pixel shader without DS_APPEND declared gl_HelperInvocation");
  }
  std::printf("[host]    %-32s ok\n", name);
}
// The store-byte fixture from afb40969, also exercised as a short store.
void CheckPixelHelperAtomicsCodegen() {
  constexpr const char *name = "PixelHelperAtomicsCodegen";
  Require(name, "defaults", ShaderRecompiler::CodegenOptions{}.ps_helper_atomics_skip,
          "helper atomic skipping is not the default");
  for (const u32 opcode : {0x18u, 0x1au}) {
    size_t branches_off = 0;
    for (const bool skip : {false, true}) {
      auto options = GetCodegenOptions();
      options.ps_helper_atomics_skip = skip;
      options.ps_append_live_election = false;
      options.ps_live_exec = PsLiveExec::Off;
      ScopedCodegenOptions scoped(options);
      GraphicsCase test;
      test.name = name;
      auto &code = test.fragment_code;
      AppendVMovU32(&code, 1u, 0x5au);
      AppendVMovU32(&code, 31u, 0u);
      AppendBufferStoreOpcode(&code, opcode, 1u, 31u);
      code.push_back(EncodeVop1(0x01u, 2u, InlineU32(0u)));
      code.push_back(EncodeExp0(0u, 0xfu, true, false, true));
      code.push_back(EncodeExp1(2u, 2u, 2u, 2u));
      AppendEnd(&code);
      const auto compiled = CompileFragmentCase(test);
      Require(name, "SPIR-V", CountSpirv(compiled.spirv, "OpAtomicCompareExchange") == 1u,
              "subword store is not one compare-exchange loop");
      Require(name, "SPIR-V", CountSpirv(compiled.spirv, "BuiltIn HelperInvocation") == (skip ? 1u : 0u),
              "helper declaration does not follow the switch");
      const auto branches = CountSpirv(compiled.spirv, "OpBranchConditional");
      if (!skip) branches_off = branches;
      else Require(name, "SPIR-V", branches == branches_off + 1u,
                   "helpers do not have a branch around the CAS loop");
    }
  }
  std::printf("[host]    %-32s ok\n", name);
}

void CheckComputeCodegenUnchanged() {
  constexpr const char *name = "GiProbeComputeUnchanged";
  TestCase test;
  test.name = name;
  test.initial.assign(64u, 0u);
  AppendSMovLiteral(&test.code, 124u, 0x100u);
  test.code.push_back(EncodeDs0(0x3eu, 0u, true));
  test.code.push_back(EncodeDs1(1u, 0u, 0u));
  AppendVMovU32(&test.code, 31u, 0u);
  AppendBufferStoreOpcode(&test.code, 0x18u, 1u, 31u);
  AppendEnd(&test.code);
  test.has_compute_info = true;
  test.compute_info.threads_num[0] = 64;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  for (const u32 wave : {32u, 64u}) {
    test.compute_info.wave_size = wave;
    std::vector<u32> off;
    for (const bool enabled : {false, true}) {
      auto options = GetCodegenOptions();
      options.ps_append_live_election = enabled;
      options.ps_helper_atomics_skip = enabled;
      options.ps_live_exec = enabled ? PsLiveExec::All : PsLiveExec::Off;
      ScopedCodegenOptions scoped(options);
      const auto compiled = CompileCase(test, 32u);
      Require(name, "SPIR-V", CountSpirv(compiled.spirv, "BuiltIn HelperInvocation") == 0,
              "a compute shader declared a pixel built-in");
      if (!enabled) off = compiled.spirv;
      else Require(name, "SPIR-V", off == compiled.spirv,
                   "pixel helper fixes changed compute append or CAS code");
    }
  }
  std::printf("[host]    %-32s ok\n", name);
}

void CheckCodegenFeatureKeys() {
  constexpr const char *name = "GiProbeFeatureKeys";
  namespace F = ShaderRecompiler::Frontend;
  ScopedCodegenOptions restore(GetCodegenOptions());
  const auto stored_features = F::TranslatorFeatureSet();
  ShaderRecompiler::CodegenOptions options;
  ShaderRecompiler::SetCodegenOptions(options);
  const auto defaults = F::TranslatorFeatureSet();
  const auto helper_bits = F::TranslatorFeaturePsAppendLiveElection | F::TranslatorFeaturePsLiveExec |
                           F::TranslatorFeaturePsHelperAtomicsSkip;
  Require(name, "defaults", (defaults & helper_bits) == helper_bits &&
              (defaults & F::TranslatorFeaturePsLiveExecAll) == 0,
          "helper defaults are not stamped in the cache key");
  F::SetTranslatorFeatures(0);
  Require(name, "old store", (F::TranslatorFeatureSet() & helper_bits) == helper_bits,
          "an old static store disabled the correctness fixes");
  F::SetTranslatorFeatures(stored_features);
  for (u32 variant = 0; variant < 4; variant++) {
    options = {};
    u32 changed = 0;
    switch (variant) {
      case 0: options.ps_append_live_election = false; changed = F::TranslatorFeaturePsAppendLiveElection; break;
      case 1: options.ps_live_exec = PsLiveExec::Off; changed = F::TranslatorFeaturePsLiveExec; break;
      case 2: options.ps_live_exec = PsLiveExec::All; changed = F::TranslatorFeaturePsLiveExecAll; break;
      case 3: options.ps_helper_atomics_skip = false; changed = F::TranslatorFeaturePsHelperAtomicsSkip; break;
    }
    ShaderRecompiler::SetCodegenOptions(options);
    Require(name, "cache key", (F::TranslatorFeatureSet() ^ defaults) == changed,
            "a helper option does not have its own cache key bit");
  }
  std::printf("[host]    %-32s ok\n", name);
}

} // namespace GiProbeTests

