#include "graphics/host_gpu/renderer/passTiming.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderDraw.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/shader/shader.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace Libs::Graphics::PassTiming {
namespace {
constexpr uint32_t MaxDraws = 512, Queries = 2 + 2 * MaxDraws, MaxPools = 2048;
struct Draw {
	uint64_t vs = 0, ps = 0, args = 0;
	uint32_t count = 0, instances = 0, indirect_count = 0, ordinal = 0;
	bool indexed = false;
};
struct Result {
	double ms = 0;
	uint64_t frame = 0, pass = 0, tick = 0;
	uint32_t width = 0, height = 0, draws = 0;
	Draw draw;
	std::string targets;
};
struct State;
uint32_t Number(const char* name, uint32_t def, uint32_t max) {
	const char* text = std::getenv(name);
	return text ? std::clamp(static_cast<uint32_t>(std::strtoul(text, nullptr, 10)), 1u, max) : def;
}
struct Options { uint32_t every, top; bool draws; };
const Options& Settings() {
	static const Options options = [] {
		const char* s = std::getenv("KYTY_PASS_TIMING_DRAWS");
		return Options {Number("KYTY_PASS_TIMING_EVERY", 60, 10000),
		                Number("KYTY_PASS_TIMING_TOP", 12, 64), s && s[0] == '1'};
	}();
	return options;
}
std::mutex mutex;
std::map<CommandScheduler*, std::unique_ptr<State>> states;
}
struct Pass {
	State* owner = nullptr;
	vk::QueryPool pool {};
	bool busy = false;
	Result result;
	uint32_t measured = 0;
	std::array<Draw, MaxDraws> draws;
};
namespace {
struct State {
	struct Total { uint64_t calls = 0; double ms = 0, max_ms = 0; };
	vk::Device device;
	double period = 0;
	uint32_t bits = 0;
	uint64_t frame = 0, sequence = 0, dropped = 0, skipped_draws = 0, failed = 0;
	uint64_t completed = 0;
	double total_ms = 0;
	std::vector<std::unique_ptr<Pass>> pools;
	std::vector<Result> passes, draws;
	std::map<std::pair<uint64_t, uint64_t>, Total> shaders;
};
void Keep(std::vector<Result>& top, Result result) {
	const auto at = std::lower_bound(top.begin(), top.end(), result.ms,
	    [](const Result& r, double ms) { return r.ms > ms; });
	if (at != top.end() || top.size() < Settings().top) top.insert(at, std::move(result));
	if (top.size() > Settings().top) top.pop_back();
}
void Print(State& state) {
	std::printf("[PASS-TIMING] through_flip=%llu passes=%llu sum_inclusive_ms=%.3f dropped_passes=%llu skipped_draws=%llu query_errors=%llu\n",
	    state.frame, state.completed, state.total_ms, state.dropped, state.skipped_draws, state.failed);
	const auto print = [](const char* kind, const std::vector<Result>& results) {
		for (const auto& r : results) {
			std::printf("[%s] gpu_ms=%.3f flip=%llu pass=%llu tick=%llu draw=%u draws=%u area=%ux%u VS=%016llx PS=%016llx indexed=%u count=%u instances=%u indirect=0x%llx indirect_count=%u targets=%s\n",
			    kind, r.ms, r.frame, r.pass, r.tick, r.draw.ordinal, r.draws, r.width, r.height,
			    r.draw.vs, r.draw.ps, r.draw.indexed, r.draw.count, r.draw.instances, r.draw.args,
			    r.draw.indirect_count, r.targets.c_str());
		}
	};
	print("PASS-TIME", state.passes);
	print("DRAW-TIME", state.draws);
	std::vector<std::pair<std::pair<uint64_t, uint64_t>, State::Total>> sums(state.shaders.begin(), state.shaders.end());
	std::sort(sums.begin(), sums.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
	for (size_t i = 0; i < std::min<size_t>(Settings().top, sums.size()); ++i) {
		const auto& [hash, total] = sums[i];
		std::printf("[DRAW-SUM] VS=%016llx PS=%016llx calls=%llu sum_inclusive_ms=%.3f mean_ms=%.3f max_ms=%.3f\n",
		    hash.first, hash.second, total.calls, total.ms, total.ms / total.calls, total.max_ms);
	}
	state.shaders.clear();
	state.passes.clear(); state.draws.clear();
	state.completed = state.dropped = state.skipped_draws = state.failed = 0;
	state.total_ms = 0;
	std::fflush(stdout);
}
std::string Target(const char* name, const ImageInfo& info) {
	char text[160];
	std::snprintf(text, sizeof(text), "%s@0x%llx:%ux%u:fmt%u ", name, info.data.address,
	              info.extent.width, info.extent.height, static_cast<uint32_t>(info.pixel_format));
	return text;
}
} // namespace

static void QueueRead(CommandScheduler& scheduler, Pass* pass);

Pass* Begin(const CommandBuffer& buffer, const RenderState& render) {
	auto& scheduler = buffer.GetContext().GetCommandScheduler();
	auto& graphics = buffer.GetGraphics();
	std::unique_lock lock(mutex);
	auto& owned = states[&scheduler];
	if (!owned) {
		owned = std::make_unique<State>();
		owned->device = graphics.device;
		owned->period = graphics.GetPhysicalDeviceProperties().limits.timestampPeriod;
		const auto families = graphics.physical_device.getQueueFamilyProperties();
		if (graphics.queue_family < families.size()) owned->bits = families[graphics.queue_family].timestampValidBits;
		std::printf("[PASS-TIMING] enabled draws=%u every=%u top=%u timestamp_bits=%u normal_draw_path=1 intervals=inclusive-no-extra-barriers pass_metadata=first-draw draw_interval=guest-call indirect_counts=unknown-not-read-back\n",
		    Settings().draws, Settings().every, Settings().top, owned->bits);
	}
	auto& s = *owned;
	if (s.bits == 0) { ++s.dropped; return nullptr; }
	Pass* pass = nullptr;
	for (auto& p : s.pools) if (!p->busy) { pass = p.get(); break; }
	if (!pass) {
		if (s.pools.size() == MaxPools) { ++s.dropped; return nullptr; }
		auto p = std::make_unique<Pass>();
		vk::QueryPoolCreateInfo info {};
		info.queryType = vk::QueryType::eTimestamp;
		info.queryCount = Settings().draws ? Queries : 2;
		if (s.device.createQueryPool(&info, nullptr, &p->pool) != vk::Result::eSuccess) {
			++s.dropped; return nullptr;
		}
		p->owner = &s;
		pass = p.get();
		s.pools.push_back(std::move(p));
	}
	pass->busy = true;
	pass->measured = 0;
	pass->result = {};
	pass->result.frame = s.frame;
	pass->result.pass = ++s.sequence;
	pass->result.tick = scheduler.CurrentTick();
	pass->result.width = render.width;
	pass->result.height = render.height;
	// Also identify raw/internal renderings without a guest draw.
	char views[256];
	std::snprintf(views, sizeof(views), "color_views=%u:first=%p depth_view=%p clear=%u/%u",
	    render.num_color_attachments, static_cast<void*>(static_cast<VkImageView>(render.color_attachments[0].image_view)),
	    static_cast<void*>(static_cast<VkImageView>(render.depth_stencil_attachment.image_view)),
	    render.depth_stencil_attachment.depth_clear, render.depth_stencil_attachment.stencil_clear);
	pass->result.targets = views;
	// Reset must be outside rendering. Reserve all draw queries here, never reset inside a pass.
	buffer.Handle().resetQueryPool(pass->pool, 0, Settings().draws ? Queries : 2);
	buffer.Handle().writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, pass->pool, 0);
	lock.unlock();
	QueueRead(scheduler, pass);
	return pass;
}

void End(const CommandBuffer& buffer, Pass* pass) {
	if (!pass) return;
	buffer.Handle().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pass->pool, 1);
}

static void QueueRead(CommandScheduler& scheduler, Pass* pass) {
	// Register while recording is open, before Shutdown enters Draining. A pass cannot
	// complete its submission before EndRendering writes the final timestamp.
	// Same completion infrastructure as DispatchTiming; no wait, Flush or Finish.
	scheduler.DeferOperation([pass] {
		std::lock_guard lock(mutex);
		auto& s = *pass->owner;
		std::array<uint64_t, Queries> stamps {};
		const uint32_t count = 2 + pass->measured * 2;
		const auto result = s.device.getQueryPoolResults(pass->pool, 0, count, count * sizeof(uint64_t),
		    stamps.data(), sizeof(uint64_t), vk::QueryResultFlagBits::e64);
		if (result == vk::Result::eSuccess) {
			const uint64_t mask = s.bits == 64 ? UINT64_MAX : (uint64_t{1} << s.bits) - 1;
			const auto ms = [&](uint32_t i) { return double((stamps[i + 1] - stamps[i]) & mask) * s.period / 1e6; };
			pass->result.ms = ms(0);
			++s.completed; s.total_ms += pass->result.ms;
			Keep(s.passes, pass->result);
			for (uint32_t i = 0; i < pass->measured; ++i) {
				auto r = pass->result;
				r.ms = ms(2 + i * 2); r.draw = pass->draws[i];
				const auto key = std::pair{r.draw.vs, r.draw.ps};
				if (s.shaders.contains(key) || s.shaders.size() < 8192) {
					auto& total = s.shaders[key];
					++total.calls; total.ms += r.ms; total.max_ms = std::max(total.max_ms, r.ms);
				}
				Keep(s.draws, std::move(r));
			}
		} else ++s.failed;
		// A pool is reusable only after this read, not on a wrapping sequence number.
		pass->busy = false;
	});
}

uint32_t BeginDraw(CommandBuffer& buffer, const DrawRenderState& state,
                   const DrawCallInfo& draw, const DrawEmitInfo& emit) {
	Pass* pass = buffer.TimingPass();
	if (!pass) return UINT32_MAX;
	Draw d;
	d.vs = state.vs_input_info.stage.program ? state.vs_input_info.stage.program->shader_hash : 0;
	d.ps = state.ps_active && state.ps_input_info.stage.program ? state.ps_input_info.stage.program->shader_hash : 0;
	d.count = draw.index_count; d.instances = draw.instance_count;
	d.indexed = emit.indexed; d.args = emit.gpu_args; d.indirect_count = emit.gpu_args_count;
	d.ordinal = ++pass->result.draws;
	if (d.ordinal == 1) {
		pass->result.draw = d;
		for (uint32_t i = 0; i < state.color_count; ++i)
			pass->result.targets += " " + Target(("color" + std::to_string(i)).c_str(), state.color_info[i].desc.info);
		if (state.depth_info.image_id) pass->result.targets += " " + Target("depth", state.depth_info.desc.info);
	}
	if (!Settings().draws) return UINT32_MAX;
	if (pass->measured == MaxDraws) {
		std::lock_guard lock(mutex); ++pass->owner->skipped_draws; return UINT32_MAX;
	}
	const auto index = pass->measured++;
	pass->draws[index] = d;
	const uint32_t query = 2 + index * 2;
	buffer.Handle().writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, pass->pool, query);
	return query;
}
void EndDraw(CommandBuffer& buffer, uint32_t query) {
	if (query != UINT32_MAX)
		buffer.Handle().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, buffer.TimingPass()->pool, query + 1);
}
void Flip(CommandScheduler& scheduler) {
	if (!Enabled()) return;
	std::unique_lock lock(mutex);
	const auto it = states.find(&scheduler);
	if (it == states.end()) return;
	auto* s = it->second.get();
	const bool report = ++s->frame % Settings().every == 0;
	lock.unlock();
	if (report) scheduler.DeferOperation([s] {
		std::lock_guard lock(mutex); Print(*s);
	});
}
void Shutdown(CommandScheduler& scheduler) {
	if (!Enabled()) return;
	std::lock_guard lock(mutex);
	const auto it = states.find(&scheduler);
	if (it == states.end()) return;
	Print(*it->second);
	for (const auto& p : it->second->pools) it->second->device.destroyQueryPool(p->pool);
	states.erase(it);
}
} // namespace Libs::Graphics::PassTiming
