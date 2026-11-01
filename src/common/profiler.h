#ifndef KYTY_COMMON_PROFILER_H_
#define KYTY_COMMON_PROFILER_H_

#include "common/common.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <tracy/Tracy.hpp> // IWYU pragma: export

namespace profiler::colors {

inline constexpr uint32_t RedA100        = 0xff8a80;
inline constexpr uint32_t Blue300        = 0x64b5f6;
inline constexpr uint32_t CyanA700       = 0x00b8d4;
inline constexpr uint32_t Green100       = 0xc8e6c9;
inline constexpr uint32_t Green200       = 0xa5d6a7;
inline constexpr uint32_t Green300       = 0x81c784;
inline constexpr uint32_t Green400       = 0x66bb6a;
inline constexpr uint32_t Amber300       = 0xffd54f;
inline constexpr uint32_t DeepOrangeA200 = 0xff6e40;

} // namespace profiler::colors

namespace Profiler {

// Set while the Tracy profiler runs (--profiler-direction Network). Blocks check it inline:
// without a profiler a block costs one load, not calls into the common and Tracy libraries.
inline std::atomic<bool> g_active {false};

class ScopedBlock {
public:
	explicit ScopedBlock(const tracy::SourceLocationData* source_location) {
		if (g_active.load(std::memory_order_relaxed)) {
			Begin(source_location);
		}
	}
	ScopedBlock(const ScopedBlock&)            = delete;
	ScopedBlock& operator=(const ScopedBlock&) = delete;
	ScopedBlock(ScopedBlock&&)                 = delete;
	ScopedBlock& operator=(ScopedBlock&&)      = delete;
	~ScopedBlock() {
		if (m_zone.has_value()) {
			End();
		}
	}

	void End();

private:
	void Begin(const tracy::SourceLocationData* source_location);

	std::optional<tracy::ScopedZone> m_zone;
};

void EndBlock();
void SetThreadName(const char* name);

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr const char* name               = "Profiler";
	static constexpr auto        initialize         = Profiler::Initialize;
	static constexpr auto        shutdown           = Profiler::Shutdown;
	static constexpr auto        emergency_shutdown = Profiler::Shutdown;
};

} // namespace Profiler

#define KYTY_PROFILER_CONCAT_IMPL(a, b) a##b
#define KYTY_PROFILER_CONCAT(a, b)      KYTY_PROFILER_CONCAT_IMPL(a, b)
#define KYTY_PROFILER_COLOR_OR_DEFAULT(default_color, ...)                                         \
	KYTY_PROFILER_COLOR_OR_DEFAULT_IMPL(default_color __VA_OPT__(, ) __VA_ARGS__, default_color)
#define KYTY_PROFILER_COLOR_OR_DEFAULT_IMPL(default_color, color, ...) color

#define KYTY_PROFILER_BLOCK(name, ...)                                                             \
	KYTY_PROFILER_BLOCK_IMPL(__LINE__, name __VA_OPT__(, ) __VA_ARGS__)
#define KYTY_PROFILER_BLOCK_IMPL(line, name, ...)                                                  \
	static constexpr tracy::SourceLocationData KYTY_PROFILER_CONCAT(                               \
	    kyty_profiler_source_location_, line) {name, TracyFunction, TracyFile,                     \
	                                           static_cast<uint32_t>(line),                        \
	                                           KYTY_PROFILER_COLOR_OR_DEFAULT(0, __VA_ARGS__)};    \
	Profiler::ScopedBlock KYTY_PROFILER_CONCAT(kyty_profiler_block_, line)(                        \
	    &KYTY_PROFILER_CONCAT(kyty_profiler_source_location_, line))

#define KYTY_PROFILER_FUNCTION(...) KYTY_PROFILER_FUNCTION_IMPL(__LINE__ __VA_OPT__(, ) __VA_ARGS__)
#define KYTY_PROFILER_FUNCTION_IMPL(line, ...)                                                     \
	static constexpr tracy::SourceLocationData KYTY_PROFILER_CONCAT(                               \
	    kyty_profiler_source_location_, line) {nullptr, TracyFunction, TracyFile,                  \
	                                           static_cast<uint32_t>(line),                        \
	                                           KYTY_PROFILER_COLOR_OR_DEFAULT(0, __VA_ARGS__)};    \
	Profiler::ScopedBlock KYTY_PROFILER_CONCAT(kyty_profiler_block_, line)(                        \
	    &KYTY_PROFILER_CONCAT(kyty_profiler_source_location_, line))

#define KYTY_PROFILER_END_BLOCK Profiler::EndBlock()

#define KYTY_PROFILER_THREAD(name) Profiler::SetThreadName(name)

#endif /* KYTY_COMMON_PROFILER_H_ */
