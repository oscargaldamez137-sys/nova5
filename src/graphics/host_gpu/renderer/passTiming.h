#pragma once
#include <cstdint>
#include <cstdlib>

namespace Libs::Graphics {
class CommandBuffer;
class CommandScheduler;
struct RenderState;
struct DrawRenderState;
struct DrawCallInfo;
struct DrawEmitInfo;
namespace PassTiming {
struct Pass;
inline bool Enabled() {
	static const bool enabled = [] { const char* v = std::getenv("KYTY_PASS_TIMING"); return v && v[0] == '1'; }();
	return enabled;
}
Pass* Begin(const CommandBuffer& buffer, const RenderState& state);
void End(const CommandBuffer& buffer, Pass* pass);
uint32_t BeginDraw(CommandBuffer& buffer, const DrawRenderState& state,
                   const DrawCallInfo& draw, const DrawEmitInfo& emit);
void EndDraw(CommandBuffer& buffer, uint32_t query);
void Flip(CommandScheduler& scheduler);
void Shutdown(CommandScheduler& scheduler);
} // namespace PassTiming
} // namespace Libs::Graphics
