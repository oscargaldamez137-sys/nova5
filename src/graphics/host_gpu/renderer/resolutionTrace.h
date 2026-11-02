#pragma once

#include <array>
#include <cstdint>
#include <map>

namespace Libs::Graphics {
namespace HW { class Context; }

// Guest draw dimensions, independent of the presentation window. Owned by the GPU command lane.
class ResolutionTrace {
public:
	void Observe(const HW::Context& context, uint32_t draws = 1);
	void OnFlip();

private:
	using Dimensions = std::array<uint32_t, 4>; // viewport width/height, color target width/height
	std::map<Dimensions, uint32_t> m_draws;
	Dimensions m_previous {};
	uint64_t m_flip = 0;
	bool m_reported = false;
};
} // namespace Libs::Graphics
