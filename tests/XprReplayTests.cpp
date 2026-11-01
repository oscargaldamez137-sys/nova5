// Offline replay of captured XPR draws (src/local/xpr-capture.h), no game needed.
//
//   xpr_replay_tests <capture-dir> [--limit N] [--gpu] [--only NAME]
//
// Stage 1 (always): for every captured draw and shader stage, translate the
// captured code with the captured compiler inputs, evaluate its SRT against the
// captured guest reads, require the runtime resource snapshot to be reproduced
// exactly, and compile it to SPIR-V.
//
// Stage 2 (--gpu): replay the draw through the real renderer on a headless
// device. Guest memory is mapped at the captured addresses; captured ranges get
// their captured bytes, textures a pseudo-random pattern keyed by address,
// render targets their fill values; the shaders are registered, the register
// blocks restored, and RenderExecutor::DrawIndex runs with a synthetic index
// sequence. Every render target is read back and hashed. Each draw is replayed
// twice and must give identical results.
#include "common/common.h"
#include "common/config.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/gpuResourceManager.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "shader-warmup-cache.h"

#include <xxhash.h>

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Libs::Graphics;
namespace IR = ShaderRecompiler::IR;

#ifdef KYTY_LOCAL_VULKAN_RECORDING
extern "C" volatile std::atomic<uint32_t> kyty_local_native_xpr_debug;
#endif

namespace Libs::Graphics {
// Friend of RenderExecutor (render.h): the replay drives the real draw entry.
struct RenderExecutorTestAccess {
	static void DrawIndex(RenderExecutor& executor, uint64_t submit_id, CommandBuffer& buffer,
	                      const DrawIndexArgs& args) {
		executor.DrawIndex(submit_id, buffer, args);
	}
#ifdef KYTY_LOCAL_VULKAN_RECORDING
	static void RequestNativeStore(RenderExecutor& executor) { executor.NativeXprRequestStore(); }
	static bool NativeDraw(RenderExecutor& executor, CommandBuffer& buffer, uint64_t index_address,
	                       uint64_t index_bytes, bool index32, uint64_t args_address) {
		RenderExecutor::NativeXprDrawArgs args;
		args.index_address = index_address;
		args.index_bytes   = index_bytes;
		args.index_type    = index32 ? vk::IndexType::eUint32 : vk::IndexType::eUint16;
		args.args_address  = args_address;
		args.keep_clears   = true;
		return executor.NativeXprDraw(buffer, args);
	}
#endif
};
} // namespace Libs::Graphics

namespace {

void Initialize() {
	static Common::Subsystems subsystems;
	Common::InitializeThreads();
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::OutputDirection::Silent;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
}

struct Cursor {
	std::span<const uint32_t> words;
	size_t                    at   = 0;
	bool                      good = true;
	uint32_t                  U32() {
        if (at >= words.size()) {
            good = false;
            return 0;
        }
        return words[at++];
	}
	uint64_t U64() {
		const uint64_t lo = U32();
		return lo | (uint64_t(U32()) << 32u);
	}
	std::vector<uint32_t> Vector(uint32_t count) {
		if (count > words.size() - std::min(at, words.size())) {
			good = false;
			return {};
		}
		std::vector<uint32_t> out(words.begin() + at, words.begin() + at + count);
		at += count;
		return out;
	}
};

struct GuestRange {
	uint64_t              address = 0, size = 0;
	std::vector<uint32_t> words;
};

struct Stage {
	LocalShaderWarmup::Record record;
	std::vector<uint32_t>     user_data;
	uint32_t                  push_start   = 0;
	uint64_t                  code_address = 0;
	IR::ResourceSnapshot      resources;
	std::vector<GuestRange>   buffers;
};

struct ShaderEntry {
	uint64_t address = 0, semantics = 0, user_data = 0;
	uint32_t type = 0, code_size = 0, scratch = 0, semantics_count = 0;
};

// Mirrors XprCapture::FillKind.
enum class FillKind : uint32_t { Texture = 1, Metadata = 2, ColorTarget = 3, DepthTarget = 4, Stencil = 5 };
struct Fill {
	uint64_t address = 0, size = 0;
	FillKind kind    = FillKind::Texture;
	uint32_t value   = 0;
};

struct Draw {
	std::array<uint32_t, 5> args {};
	Stage                   vertex;
	bool                    has_pixel = false;
	Stage                   pixel;
	std::vector<GuestRange> reads;
	std::vector<uint32_t>   context, shaders, user_config;
	std::vector<ShaderEntry> entries;
	std::vector<GuestRange> exact;
	std::vector<Fill>       fills, targets;
};

GuestRange ReadRange(Cursor& c) {
	GuestRange r;
	r.address = c.U64();
	r.size    = c.U64();
	r.words   = c.Vector(static_cast<uint32_t>((r.size + 3) / 4));
	return r;
}

void ReadSnapshot(Cursor& c, IR::ResourceSnapshot& s) {
	for (auto* values: {&s.buffers, &s.images, &s.samplers}) {
		const auto count = c.U32();
		values->resize(count);
		for (auto& value: *values) {
			value.dword_count = c.U32();
			for (auto& word: value.dwords) word = c.U32();
		}
	}
	for (auto* words: {&s.flattened_srt, &s.user_data}) *words = c.Vector(c.U32());
	s.uniform_fill.kind     = static_cast<IR::UniformFillKind>(c.U32());
	s.uniform_fill.resource = c.U32();
	for (auto& stride: s.uniform_fill.group_stride) stride = c.U32();
	s.uniform_fill.words = c.U32();
	s.uniform_fill.value = c.U32();
}

bool ReadStage(Cursor& c, Stage& stage) {
	const auto record_words = c.Vector(c.U32());
	LocalShaderWarmup::Reader reader {record_words};
	if (!c.good || !LocalShaderWarmup::Visit(reader, stage.record)) return false;
	stage.user_data    = c.Vector(c.U32());
	stage.push_start   = c.U32();
	stage.code_address = c.U64();
	ReadSnapshot(c, stage.resources);
	const auto buffers = c.U32();
	for (uint32_t i = 0; i < buffers && c.good; ++i) stage.buffers.push_back(ReadRange(c));
	return c.good;
}

bool ReadDraw(const std::filesystem::path& path, Draw& draw) {
	std::vector<uint32_t> words(std::filesystem::file_size(path) / 4);
	if (std::FILE* file = std::fopen(path.string().c_str(), "rb")) {
		const auto got = std::fread(words.data(), 4, words.size(), file);
		std::fclose(file);
		if (got != words.size()) return false;
	} else {
		return false;
	}
	Cursor c {words};
	if (c.U32() != 0x43525058u || c.U32() != 3u) return false;
	draw.has_pixel = c.U32() != 0;
	for (auto& arg: draw.args) arg = c.U32();
	if (!ReadStage(c, draw.vertex)) return false;
	if (draw.has_pixel && !ReadStage(c, draw.pixel)) return false;
	const auto reads = c.U32();
	for (uint32_t i = 0; i < reads && c.good; ++i) draw.reads.push_back(ReadRange(c));
	for (auto* block: {&draw.context, &draw.shaders, &draw.user_config}) *block = c.Vector(c.U32());
	const auto entries = c.U32();
	for (uint32_t i = 0; i < entries && c.good; ++i) {
		ShaderEntry e;
		e.address         = c.U64();
		e.type            = c.U32();
		e.code_size       = c.U32();
		e.scratch         = c.U32();
		e.semantics_count = c.U32();
		e.semantics       = c.U64();
		e.user_data       = c.U64();
		draw.entries.push_back(e);
	}
	const auto exact = c.U32();
	for (uint32_t i = 0; i < exact && c.good; ++i) draw.exact.push_back(ReadRange(c));
	for (auto* fills: {&draw.fills, &draw.targets}) {
		const auto count = c.U32();
		for (uint32_t i = 0; i < count && c.good; ++i) {
			Fill f;
			f.address = c.U64();
			f.size    = c.U64();
			f.kind    = static_cast<FillKind>(c.U32());
			f.value   = c.U32();
			fills->push_back(f);
		}
	}
	return c.good && c.at == words.size();
}

// ---------------------------------------------------------------------------
// Stage 1: offline translation and SRT evaluation.

// Guest memory as the SRT evaluation saw it, word-addressed.
struct Memory {
	std::unordered_map<uint64_t, uint32_t> words;
	uint64_t                               missing = 0, missing_address = 0;
	void Add(const GuestRange& range) {
		for (size_t i = 0; i < range.words.size(); ++i) words[range.address + 4 * i] = range.words[i];
	}
	bool Read(uint64_t address, uint32_t* value) {
		const auto it = words.find(address);
		if (it == words.end()) {
			if (missing++ == 0) missing_address = address;
			return false;
		}
		*value = it->second;
		return true;
	}
	static bool ReadWord(void* self, uint64_t address, uint32_t* value) {
		return static_cast<Memory*>(self)->Read(address, value);
	}
	static bool Sync(void*, uint64_t, uint64_t) { return true; }
	static bool Span(void* self, uint64_t address, uint32_t* values, uint32_t count, bool) {
		for (uint32_t i = 0; i < count; ++i)
			if (!static_cast<Memory*>(self)->Read(address + 4ull * i, values + i)) return false;
		return true;
	}
};

std::string Describe(const IR::ResourceSnapshot& a, const IR::ResourceSnapshot& b) {
	std::string out;
	if (!(a.buffers == b.buffers)) out += " buffers";
	if (!(a.images == b.images)) out += " images";
	if (!(a.samplers == b.samplers)) out += " samplers";
	if (a.flattened_srt != b.flattened_srt) out += " flattened_srt";
	if (a.user_data != b.user_data) out += " user_data";
	if (!(a.uniform_fill == b.uniform_fill)) out += " uniform_fill";
	return out;
}

size_t g_traced_stages = 0, g_untraced_stages = 0, g_traced_data = 0, g_traced_structural = 0, g_traced_feeds = 0;

std::string ReplayStage(Stage& stage, Memory& memory) {
	auto options    = LocalShaderWarmup::Options(stage.record, stage.user_data);
	options.dump_ir = false;
	auto translated = ShaderRecompiler::TranslateProgram(stage.record.code, options);
	auto plan       = IR::ExtractResourcePlan(translated.program);
	IR::ResourceSnapshot       resources;
	IR::ResourceSpecialization specialization;
	const IR::SrtRuntime runtime {
	    .user_data                  = stage.user_data,
	    .shader_base                = stage.code_address,
	    .read_memory                = Memory::ReadWord,
	    .userdata                   = &memory,
	    .read_specialization_memory = Memory::ReadWord,
	    .sync_memory                = Memory::Sync,
	    .try_read_memory_span       = Memory::Span,
	};
	IR::MaterializeReport report;
	memory.missing = 0;
	if (!IR::MaterializeResources(plan, runtime, resources, specialization, &report)) {
		char buffer[160];
		std::snprintf(buffer, sizeof(buffer), "materialization failed (%s), first missing 0x%" PRIx64,
		              report.reason.c_str(), memory.missing_address);
		return buffer;
	}
	if (const auto diff = Describe(resources, stage.resources); !diff.empty()) return "snapshot differs:" + diff;
	// The read trace native records rely on: every data read is a plain copy of
	// its address into its flat position, every structural read has the memory value.
	{
		IR::SrtReadTrace trace;
		if (IR::TraceLinearSrtReads(plan, runtime, trace)) {
			++g_traced_stages;
			g_traced_data += trace.data.size();
			g_traced_structural += trace.structural.size();
			if (trace.flat_words != resources.flattened_srt.size()) return "trace: flat size";
			for (const auto& [flat, address]: trace.data) {
				uint32_t value = 0;
				if (!memory.Read(address, &value) || value != resources.flattened_srt[flat]) return "trace: data read";
			}
			for (const auto& read: trace.structural) {
				uint32_t value = 0;
				if (!memory.Read(read.address, &value) || value != read.value) return "trace: structural read";
			}
			g_traced_feeds += trace.feeds.size();
			for (const auto& [read, flat]: trace.flat_feeds)
				if (flat >= resources.flattened_srt.size() || resources.flattened_srt[flat] != trace.structural[read].value)
					return "trace: flat feed";
			for (const auto& feed: trace.feeds) {
				const auto& values = feed.kind == IR::SrtReadTrace::Kind::Buffer  ? resources.buffers
				                     : feed.kind == IR::SrtReadTrace::Kind::Image ? resources.images
				                                                                  : resources.samplers;
				if (feed.index >= values.size() || feed.dword >= values[feed.index].dword_count)
					return "trace: feed target";
				// An image the evaluation found invalid is zeroed; its feeds do not apply.
				if (values[feed.index].dwords[feed.dword] != trace.structural[feed.read].value &&
				    !(feed.kind == IR::SrtReadTrace::Kind::Image &&
				      std::ranges::all_of(values[feed.index].dwords, [](uint32_t w) { return w == 0; })))
					return "trace: feed value";
			}
		} else {
			++g_untraced_stages;
		}
	}
	auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization,
	                                                 stage.push_start);
	return compiled.spirv.empty() ? "empty SPIR-V" : "";
}

// ---------------------------------------------------------------------------
// Stage 2: replay through the real renderer.

constexpr uint64_t RegionAlign = 0x10000;

struct Region {
	uint64_t address = 0, size = 0;
	int64_t  direct  = -1;
};

struct TargetResult {
	uint64_t hash = 0, changed = 0, words = 0;
	bool     found = false;
	bool     operator==(const TargetResult&) const = default;
};

class GpuReplay {
public:
	GpuReplay() {
		if (!CreateHeadlessGraphicContext(m_graphics)) {
			std::printf("GPU_REPLAY unavailable: no headless Vulkan device\n");
			return;
		}
		m_context = std::make_unique<RenderContext>(m_graphics);
		Libs::LibKernel::Memory::InstallGpuResources(&m_context->GetGpuResources());
		GuestGpu::SetOfflineGpuThread(true);
		m_context->GetCommandScheduler().Begin(m_registers, m_user_config, m_shaders);
	}
	[[nodiscard]] bool Available() const { return m_context != nullptr; }
	bool m_force_depth_pass = true;

	// Replays one draw; returns false with a reason when it cannot be set up.
	// With `native`, the normal draw also stores a native record; the targets are
	// then reset to their fill values and the same draw goes through the native
	// path (GPU-side indirect arguments), with its targets in `native`.
	bool Run(const Draw& draw, std::vector<TargetResult>& out, std::string& why,
	         std::vector<TargetResult>* native = nullptr) {
		out.clear();
		if (native != nullptr) native->clear();
		std::vector<std::pair<uint64_t, uint64_t>> ranges;
		const auto add = [&](uint64_t address, uint64_t size) {
			if (address != 0 && size != 0) ranges.emplace_back(address, size);
		};
		for (const auto* stage: {&draw.vertex, draw.has_pixel ? &draw.pixel : nullptr}) {
			if (stage == nullptr) continue;
			for (const auto& r: stage->buffers) add(r.address, r.size);
		}
		for (const auto& r: draw.reads) add(r.address, r.size);
		for (const auto& r: draw.exact) add(r.address, r.size);
		for (const auto& f: draw.fills) add(f.address, f.size);
		for (const auto& f: draw.targets) add(f.address, f.size);
		uint64_t highest = 0;
		for (const auto& [address, size]: ranges) highest = std::max(highest, address + size);
		// Synthetic index buffer after every captured range.
		const uint64_t index_type  = draw.args[4];
		const uint64_t index_bytes = index_type == 0 ? 2 : 4;
		const uint32_t index_count =
		    std::min<uint32_t>(draw.args[0], index_type == 0 ? 0xfffeu : 30000u);
		const uint64_t index_base = (highest + 0x1000000 + RegionAlign - 1) & ~(RegionAlign - 1);
		add(index_base, uint64_t(index_count) * index_bytes);
		// Indirect arguments for the native path, as the culling shader writes them.
		const uint64_t args_address = (index_base + uint64_t(index_count) * index_bytes + 0xff) & ~uint64_t {0xff};
		add(args_address, sizeof(vk::DrawIndexedIndirectCommand));
		auto regions = Merge(ranges);
		if (!MapRegions(regions, why)) return false;

		// Contents: fill patterns first, captured bytes over them.
		for (const auto& f: draw.fills) FillRange(f);
		for (const auto* stage: {&draw.vertex, draw.has_pixel ? &draw.pixel : nullptr}) {
			if (stage == nullptr) continue;
			for (const auto& r: stage->buffers) CopyRange(r);
		}
		for (const auto& r: draw.reads) CopyRange(r);
		for (const auto& r: draw.exact) CopyRange(r);
		for (uint32_t i = 0; i < index_count; ++i) {
			if (index_bytes == 2) reinterpret_cast<uint16_t*>(index_base)[i] = static_cast<uint16_t>(i);
			else reinterpret_cast<uint32_t*>(index_base)[i] = i;
		}
		const vk::DrawIndexedIndirectCommand indirect {index_count, std::max(draw.args[1], 1u), 0,
		                                               static_cast<int32_t>(draw.args[2]), draw.args[3]};
		std::memcpy(reinterpret_cast<void*>(args_address), &indirect, sizeof(indirect));
		auto& resources = m_context->GetGpuResources();
		for (const auto& region: regions) resources.MapMemory(region.address, region.size);

		for (const auto& e: draw.entries) {
			ShaderMappedData data {};
			data.type                = static_cast<Prospero::ShaderBinaryType>(e.type);
			data.user_data           = reinterpret_cast<ShaderUserData*>(e.user_data);
			data.input_semantics     = reinterpret_cast<ShaderSemantic*>(e.semantics);
			data.num_input_semantics = e.semantics_count;
			data.code_size_bytes     = e.code_size;
			data.scratch_size_dwords = e.scratch;
			ShaderMapUserData(e.address, data);
		}
		if (!Restore(m_registers, draw.context) || !Restore(m_shaders, draw.shaders) ||
		    !Restore(m_user_config, draw.user_config)) {
			why = "register block size mismatch";
			UnmapRegions(regions);
			return false;
		}
		if (m_force_depth_pass) {
			// The replay's depth buffer holds a uniform far value, which an EQUAL test
			// (GBuffer after the prepass) never matches. Both paths get the same state.
			auto control                = m_registers.GetDepthControl();
			control.zfunc               = 7; // always
			control.stencil_enable      = false;
			control.depth_bounds_enable = false;
			m_registers.SetDepthControl(control);
		}

		auto&               scheduler = m_context->GetCommandScheduler();
		const DrawIndexArgs args {.index_count         = index_count,
		                          .index_addr          = reinterpret_cast<const void*>(index_base),
		                          .instance_count      = std::max(draw.args[1], 1u),
		                          .index_type_and_size = draw.args[4],
		                          .base_vertex         = static_cast<int32_t>(draw.args[2]),
		                          .first_instance      = draw.args[3],
		                          .offset_source       = DrawOffsetSource::IndirectArgs};
#ifdef KYTY_LOCAL_VULKAN_RECORDING
		if (native != nullptr) RenderExecutorTestAccess::RequestNativeStore(m_context->GetRenderExecutor());
#endif
		RenderExecutorTestAccess::DrawIndex(m_context->GetRenderExecutor(), ++m_submit_id,
		                                    scheduler.Current(), args);
		for (const auto& target: draw.targets) out.push_back(ReadTarget(target));
		if (native != nullptr) {
#ifdef KYTY_LOCAL_VULKAN_RECORDING
			for (const auto& target: draw.targets) ResetTarget(target);
			if (!RenderExecutorTestAccess::NativeDraw(m_context->GetRenderExecutor(), scheduler.Current(),
			                                          index_base, uint64_t(index_count) * index_bytes,
			                                          index_bytes == 4, args_address)) {
				why = "native draw refused (no record)";
				UnmapRegions(regions);
				return false;
			}
			for (const auto& target: draw.targets) native->push_back(ReadTarget(target));
#else
			why = "native path needs KYTY_LOCAL_VULKAN_RECORDING";
			UnmapRegions(regions);
			return false;
#endif
		}
		UnmapRegions(regions);
		return true;
	}

private:
	template <typename T>
	static bool Restore(T& value, const std::vector<uint32_t>& words) {
		if (words.size() != (sizeof(T) + 3) / 4) return false;
		std::memcpy(&value, words.data(), sizeof(T));
		return true;
	}

	static std::vector<Region> Merge(std::vector<std::pair<uint64_t, uint64_t>> ranges) {
		for (auto& [address, size]: ranges) {
			const auto begin = address & ~(RegionAlign - 1);
			const auto end   = (address + size + RegionAlign - 1) & ~(RegionAlign - 1);
			address          = begin;
			size             = end - begin;
		}
		std::ranges::sort(ranges);
		std::vector<Region> out;
		for (const auto& [address, size]: ranges) {
			if (!out.empty() && address <= out.back().address + out.back().size) {
				out.back().size = std::max(out.back().address + out.back().size, address + size) -
				                  out.back().address;
			} else {
				out.push_back({address, size});
			}
		}
		return out;
	}

	bool MapRegions(std::vector<Region>& regions, std::string& why) {
		namespace M = Libs::LibKernel::Memory;
		for (auto& region: regions) {
			if (M::KernelAllocateDirectMemory(0, M::KernelGetDirectMemorySize(), region.size, RegionAlign, 0,
			                                  &region.direct) != 0) {
				why = "direct allocation failed";
				return false;
			}
			void* mapped = reinterpret_cast<void*>(region.address);
			if (M::KernelMapDirectMemory(&mapped, region.size, 0x3, 0x10, region.direct, RegionAlign) != 0 ||
			    mapped != reinterpret_cast<void*>(region.address)) {
				char buffer[96];
				std::snprintf(buffer, sizeof(buffer), "cannot map 0x%" PRIx64 "+0x%" PRIx64, region.address,
				              region.size);
				why = buffer;
				M::KernelReleaseDirectMemory(region.direct, region.size);
				region.direct = -1;
				return false;
			}
			std::memset(mapped, 0, region.size);
		}
		return true;
	}

	void UnmapRegions(const std::vector<Region>& regions) {
		namespace M = Libs::LibKernel::Memory;
		auto& resources = m_context->GetGpuResources();
		for (const auto& region: regions) {
			if (region.direct < 0) continue;
			resources.UnmapMemory(region.address, region.size);
			M::KernelMunmap(region.address, region.size);
			M::KernelReleaseDirectMemory(region.direct, region.size);
		}
	}

	static void CopyRange(const GuestRange& range) {
		std::memcpy(reinterpret_cast<void*>(range.address), range.words.data(), range.size);
	}

	static void FillRange(const Fill& fill) {
		auto*          words = reinterpret_cast<uint32_t*>(fill.address & ~uint64_t {3});
		const uint64_t count = (fill.size + 3) / 4;
		switch (fill.kind) {
			case FillKind::Texture: {
				// Keyed by address so the same texture always holds the same pattern.
				uint64_t state = fill.address * 0x9e3779b97f4a7c15ull + 1;
				for (uint64_t i = 0; i < count; ++i) {
					state ^= state << 13u;
					state ^= state >> 7u;
					state ^= state << 17u;
					words[i] = static_cast<uint32_t>(state);
				}
				break;
			}
			case FillKind::DepthTarget:
				for (uint64_t i = 0; i < count; ++i) words[i] = fill.value;
				break;
			default: std::memset(words, 0, count * 4); break;
		}
	}

	// Back to the fill value the texture cache uploaded before the first draw.
	void ResetTarget(const Fill& target) {
		auto&      cache = m_context->GetTextureCache();
		const auto id    = cache.FindImageFromRange(target.address, target.size, false);
		if (!id) return;
		auto& scheduler = m_context->GetCommandScheduler();
		auto& image     = cache.GetImage(id);
		scheduler.Current().EndRendering();
		const auto command = scheduler.Current().Handle();
		image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {}, command);
		if (image.info.IsDepth()) {
			const bool  d16   = image.info.pixel_format == vk::Format::eD16Unorm ||
			                  image.info.pixel_format == vk::Format::eD16UnormS8Uint;
			const float depth = target.kind != FillKind::DepthTarget ? 0.0f
			                    : d16 ? static_cast<float>(target.value & 0xffffu) / 65535.0f
			                          : std::bit_cast<float>(target.value);
			auto aspects = vk::ImageAspectFlags {vk::ImageAspectFlagBits::eDepth};
			if (image.info.HasStencil()) aspects |= vk::ImageAspectFlagBits::eStencil;
			const vk::ClearDepthStencilValue value {depth, 0};
			const vk::ImageSubresourceRange  range {aspects, 0, VK_REMAINING_MIP_LEVELS, 0,
			                                        VK_REMAINING_ARRAY_LAYERS};
			command.clearDepthStencilImage(image.backing.image, vk::ImageLayout::eTransferDstOptimal,
			                               &value, 1, &range);
		} else {
			const vk::ClearColorValue       value {};
			const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, VK_REMAINING_MIP_LEVELS,
			                                       0, VK_REMAINING_ARRAY_LAYERS};
			command.clearColorImage(image.backing.image, vk::ImageLayout::eTransferDstOptimal, &value, 1,
			                        &range);
		}
	}

	TargetResult ReadTarget(const Fill& target) {
		TargetResult result;
		auto&      cache = m_context->GetTextureCache();
		const auto id    = cache.FindImageFromRange(target.address, target.size, false);
		if (!id) return result;
		auto&      scheduler = m_context->GetCommandScheduler();
		auto&      image     = cache.GetImage(id);
		const bool depth     = image.info.IsDepth();
		const auto block     = image.info.IsBlock() ? 4u : 1u;
		const uint32_t texel = depth ? (image.info.pixel_format == vk::Format::eD16Unorm ||
		                                        image.info.pixel_format == vk::Format::eD16UnormS8Uint
		                                    ? 2u
		                                    : 4u)
		                             : image.info.bytes_per_block;
		const auto extent    = image.info.extent;
		const uint64_t bytes = uint64_t(texel) * ((extent.width + block - 1) / block) *
		                       ((extent.height + block - 1) / block);
		auto& device = m_graphics.device;
		vk::BufferCreateInfo buffer_info {};
		buffer_info.size  = bytes;
		buffer_info.usage = vk::BufferUsageFlagBits::eTransferDst;
		const auto buffer = device.createBuffer(buffer_info).value;
		const auto requirements = device.getBufferMemoryRequirements(buffer);
		uint32_t   type = 0;
		const auto& properties = m_graphics.physical_device_memory_properties;
		for (; type < properties.memoryTypeCount; ++type) {
			const auto flags = properties.memoryTypes[type].propertyFlags;
			if ((requirements.memoryTypeBits & (1u << type)) &&
			    (flags & vk::MemoryPropertyFlagBits::eHostVisible) &&
			    (flags & vk::MemoryPropertyFlagBits::eHostCoherent))
				break;
		}
		vk::MemoryAllocateInfo allocate {};
		allocate.allocationSize  = requirements.size;
		allocate.memoryTypeIndex = type;
		const auto memory = device.allocateMemory(allocate).value;
		(void)device.bindBufferMemory(buffer, memory, 0);

		scheduler.Current().EndRendering();
		image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
		              scheduler.Current().Handle());
		vk::BufferImageCopy copy {};
		copy.imageSubresource = {depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		copy.imageExtent      = {extent.width, extent.height, 1};
		scheduler.Current().Handle().copyImageToBuffer(image.backing.image,
		                                               vk::ImageLayout::eTransferSrcOptimal, buffer, 1, &copy);
		vk::BufferMemoryBarrier barrier {};
		barrier.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
		barrier.dstAccessMask       = vk::AccessFlagBits::eHostRead;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.buffer              = buffer;
		barrier.size                = VK_WHOLE_SIZE;
		scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                                             vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1,
		                                             &barrier, 0, nullptr);
		scheduler.Finish();
		const auto* data = static_cast<const uint32_t*>(device.mapMemory(memory, 0, bytes).value);
		result.found = true;
		result.words = bytes / 4;
		result.hash  = XXH3_64bits(data, bytes);
		const uint32_t fill = target.kind == FillKind::DepthTarget ? target.value : 0u;
		for (uint64_t i = 0; i < result.words; ++i) result.changed += data[i] != fill ? 1u : 0u;
		device.unmapMemory(memory);
		device.destroyBuffer(buffer);
		device.freeMemory(memory);
		return result;
	}

	GraphicContext                 m_graphics {};
	std::unique_ptr<RenderContext> m_context;
	HW::Context                    m_registers {};
	HW::UserConfig                 m_user_config {};
	HW::Shader                     m_shaders {};
	uint64_t                       m_submit_id = 0;
};

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <capture-dir> [--limit N] [--gpu] [--once] [--native] [--keep-depth-test] [--only NAME]\n", argv[0]);
		return 2;
	}
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	Initialize();
	size_t      limit = SIZE_MAX;
	bool        gpu   = false, once = false, keep_depth_test = false, native = false;
	std::string only;
	for (int i = 2; i < argc; ++i) {
		if (std::strcmp(argv[i], "--limit") == 0 && i + 1 < argc) limit = std::strtoull(argv[++i], nullptr, 10);
		else if (std::strcmp(argv[i], "--gpu") == 0) gpu = true;
		else if (std::strcmp(argv[i], "--once") == 0) once = true;
		else if (std::strcmp(argv[i], "--native") == 0) gpu = native = true;
		else if (std::strcmp(argv[i], "--keep-depth-test") == 0) keep_depth_test = true;
		else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) only = argv[++i];
	}

	std::vector<std::filesystem::path> files;
	for (const auto& entry: std::filesystem::directory_iterator(argv[1]))
		if (entry.path().extension() == ".xprc" && (only.empty() || entry.path().filename().string() == only))
			files.push_back(entry.path());
	std::ranges::sort(files);
	if (files.size() > limit) files.resize(limit);

#ifdef KYTY_LOCAL_VULKAN_RECORDING
	// Negative controls (see native-xpr.inc): 1 corrupts constants, 2 skips the draw.
	if (const auto* control = std::getenv("KYTY_NATIVE_XPR_DEBUG"))
		kyty_local_native_xpr_debug.store(static_cast<uint32_t>(std::strtoul(control, nullptr, 10)));
#endif
	std::unique_ptr<GpuReplay> replay;
	if (gpu) {
		ShaderInit();
		replay = std::make_unique<GpuReplay>();
		if (!replay->Available()) return 1;
		replay->m_force_depth_pass = !keep_depth_test;
	}

	size_t draws = 0, stages = 0, passed = 0, unreadable = 0;
	size_t gpu_ok = 0, gpu_failed = 0, gpu_unstable = 0, gpu_empty = 0;
	size_t native_equal = 0, native_differ = 0;
	std::map<std::string, size_t> failures;
	for (const auto& path: files) {
		const auto name = path.filename().string();
		Draw draw;
		if (!ReadDraw(path, draw)) {
			++unreadable;
			std::printf("UNREADABLE %s\n", name.c_str());
			continue;
		}
		++draws;
		Memory memory;
		for (const auto& range: draw.reads) memory.Add(range);
		for (auto* stage: {&draw.vertex, draw.has_pixel ? &draw.pixel : nullptr}) {
			if (stage == nullptr) continue;
			++stages;
			const auto why = ReplayStage(*stage, memory);
			if (why.empty()) {
				++passed;
				continue;
			}
			++failures[why];
			std::printf("FAIL %s stage=%s hash=%016" PRIx64 " %s\n", name.c_str(),
			            stage == &draw.vertex ? "vs" : "ps", stage->record.hash, why.c_str());
		}
		if (!replay) continue;
		std::vector<TargetResult> first, second, native_targets;
		std::string               why;
		if (!replay->Run(draw, first, why, native ? &native_targets : nullptr) ||
		    (!once && !replay->Run(draw, second, why))) {
			++gpu_failed;
			++failures["gpu: " + why];
			std::printf("GPU_FAIL %s %s\n", name.c_str(), why.c_str());
			continue;
		}
		const bool stable = once || first == second;
		uint64_t   changed = 0;
		std::string hashes;
		for (const auto& target: first) {
			changed += target.changed;
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), " %016" PRIx64 "/%" PRIu64, target.hash, target.changed);
			hashes += target.found ? buffer : " missing";
		}
		if (!stable) ++gpu_unstable;
		else if (changed == 0) ++gpu_empty;
		else ++gpu_ok;
		std::string native_note;
		if (native) {
			const bool equal = native_targets == first;
			++(equal ? native_equal : native_differ);
			native_note = equal ? " native=equal" : " native=DIFF";
			if (!equal)
				for (const auto& target: native_targets) {
					char buffer[64];
					std::snprintf(buffer, sizeof(buffer), " %016" PRIx64 "/%" PRIu64, target.hash, target.changed);
					native_note += target.found ? buffer : " missing";
				}
		}
		std::printf("GPU %s vs=%016" PRIx64 " ps=%016" PRIx64 " stable=%d targets:%s%s\n", name.c_str(),
		            draw.vertex.record.hash, draw.has_pixel ? draw.pixel.record.hash : 0, stable ? 1 : 0,
		            hashes.c_str(), native_note.c_str());
	}
	std::printf("XPR_REPLAY draws=%zu stages=%zu passed=%zu unreadable=%zu\n", draws, stages, passed,
	            unreadable);
	std::printf("SRT_TRACE traced=%zu untraced=%zu data_reads=%zu structural_reads=%zu feeds=%zu\n", g_traced_stages,
	            g_untraced_stages, g_traced_data, g_traced_structural, g_traced_feeds);
	if (replay)
		std::printf("GPU_REPLAY ok=%zu empty=%zu unstable=%zu failed=%zu\n", gpu_ok, gpu_empty,
		            gpu_unstable, gpu_failed);
	if (native) std::printf("NATIVE_REPLAY equal=%zu differ=%zu\n", native_equal, native_differ);
	for (const auto& [why, count]: failures) std::printf("  %zu x %s\n", count, why.c_str());
	return passed == stages && unreadable == 0 && gpu_failed == 0 && gpu_unstable == 0 &&
	               native_differ == 0
	           ? 0
	           : 1;
}
