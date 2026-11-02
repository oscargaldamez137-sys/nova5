#include "graphics/host_gpu/renderer/resolutionTrace.h"

#include "graphics/guest_gpu/hardwareContext.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics {
namespace {
bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_RENDER_RESOLUTION_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}
}

void ResolutionTrace::Observe(const HW::Context& context, uint32_t draws) {
	if (!Enabled()) return;
	const auto& control = context.GetRenderControl();
	if (!context.GetDepthControl().z_enable || context.GetColorControl().mode != 1 ||
	    control.depth_clear_enable || control.stencil_clear_enable ||
	    control.copy_depth_to_color || control.copy_stencil_to_color) return;
	const auto& viewport = context.GetScreenViewport().viewports[0];
	const float width = std::abs(viewport.xscale * 2.0f);
	const float height = std::abs(viewport.yscale * 2.0f);
	// Exclude small auxiliary passes. These are scene candidates, not the game's DRS index.
	if (!std::isfinite(width) || !std::isfinite(height) || width < 640 || height < 360 ||
	    width > 16384 || height > 16384) return;
	const auto mask = context.GetRenderTargetMask() & context.GetShaderRegisters().m_cbShaderMask;
	for (uint32_t slot = 0; slot < 8; ++slot) {
		const auto& target = context.GetRenderTarget(slot);
		if (((mask >> (slot * 4u)) & 0xfu) == 0 || target.base.addr == 0) continue;
		m_draws[{static_cast<uint32_t>(std::lround(width)), static_cast<uint32_t>(std::lround(height)),
		         target.attrib2.width + 1u, target.attrib2.height + 1u}] += draws;
		break;
	}
}

void ResolutionTrace::OnFlip() {
	if (!Enabled()) return;
	++m_flip;
	Dimensions dominant {};
	uint32_t most = 0, total = 0;
	for (const auto& [dimensions, draws]: m_draws) {
		total += draws;
		if (draws > most) {
			most = draws;
			dominant = dimensions;
		}
	}
	if (!m_reported || dominant != m_previous) {
		std::printf("[RENDER_RESOLUTION] flip=%llu scene_candidate=%ux%u color_target=%ux%u "
		            "depth_color_draws=%u/%u candidates=",
		            static_cast<unsigned long long>(m_flip), dominant[0], dominant[1], dominant[2], dominant[3],
		            most, total);
		for (const auto& [dimensions, draws]: m_draws) {
			std::printf(" %ux%u/rt%ux%u:%u", dimensions[0], dimensions[1], dimensions[2], dimensions[3], draws);
		}
		if (m_draws.empty()) std::printf("none");
		std::printf("\n");
		std::fflush(stdout);
		m_previous = dominant;
		m_reported = true;
	}
	m_draws.clear();
}
} // namespace Libs::Graphics
