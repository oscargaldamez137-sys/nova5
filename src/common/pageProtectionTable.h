#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

namespace Common {

// Sparse host page numbers, not guest addresses. In particular, high native
// mappings must not alias the guest's 40-bit address space. Externally locked
// by the same mutex as the platform's allocation/protection bookkeeping.
class PageProtectionTable {
public:
	static constexpr uintptr_t PagesPerRegion = 1024;
	static constexpr uint8_t   Unknown        = 0xff;

	[[nodiscard]] int Get(uintptr_t page) const {
		const auto found = m_regions.find(page / PagesPerRegion);
		if (found == m_regions.end()) return -1;
		const auto value = found->second->pages[page % PagesPerRegion];
		return value == Unknown ? -1 : value;
	}

	void Set(uintptr_t first, uintptr_t last, uint8_t protection) {
		if (first > last || protection == Unknown) return;
		for (;;) {
			auto& region = m_regions[first / PagesPerRegion];
			if (!region) region = std::make_unique<Region>();
			const auto offset = first % PagesPerRegion;
			const auto count  = std::min(last - first, PagesPerRegion - offset - 1) + 1;
			const auto begin  = region->pages.begin() + offset;
			region->live += static_cast<uint32_t>(std::count(begin, begin + count, Unknown));
			std::fill(begin, begin + count, protection);
			if (last - first == count - 1) break;
			first += count;
		}
	}

	void Erase(uintptr_t first, uintptr_t last) {
		if (first > last) return;
		const auto last_region = last / PagesPerRegion;
		auto       it          = m_regions.lower_bound(first / PagesPerRegion);
		for (; it != m_regions.end() && it->first <= last_region;) {
			const auto offset = it->first == first / PagesPerRegion ? first % PagesPerRegion : 0;
			const auto end = it->first == last_region ? last % PagesPerRegion + 1 : PagesPerRegion;
			if (offset == 0 && end == PagesPerRegion) {
				it = m_regions.erase(it);
				continue;
			}
			auto&      region = *it->second;
			const auto begin  = region.pages.begin() + offset;
			const auto finish = region.pages.begin() + end;
			region.live -= static_cast<uint32_t>(
			    std::count_if(begin, finish, [](uint8_t value) { return value != Unknown; }));
			std::fill(begin, finish, Unknown);
			if (region.live == 0)
				it = m_regions.erase(it);
			else
				++it;
		}
	}

	[[nodiscard]] size_t RegionCount() const { return m_regions.size(); }

private:
	struct Region {
		std::array<uint8_t, PagesPerRegion> pages;
		uint32_t                            live = 0;
		Region() { pages.fill(Unknown); }
	};
	std::map<uintptr_t, std::unique_ptr<Region>> m_regions;
};
} // namespace Common
