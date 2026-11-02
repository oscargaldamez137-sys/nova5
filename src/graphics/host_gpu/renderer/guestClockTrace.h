#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace Libs::Graphics {

// Explicit guest timer pairs only: consecutive EOP packets need not belong to the same timer.
// Counts our completed writes, not the values a concurrent guest reader actually consumed.
class GuestClockPairTrace {
public:
	enum class Source { Unknown, Cpu, Gpu };
	struct Counts { uint64_t gpu = 0, cpu = 0, mixed = 0, unresolved = 0; };
	void Add(uint64_t begin, uint64_t end) {
		for (const auto& pair: m_pairs) if (pair.begin == begin && pair.end == end) return;
		m_cells.try_emplace(begin);
		m_cells.try_emplace(end);
		m_pairs.push_back({begin, end, 0});
	}
	void Begin(uint64_t address) {
		if (auto it = m_cells.find(address); it != m_cells.end()) it->second = {++m_sequence, Source::Unknown};
	}
	void Written(uint64_t address, Source source) {
		if (auto it = m_cells.find(address); it != m_cells.end()) {
			it->second.source = source;
			for (auto& pair: m_pairs) {
				const auto& begin = m_cells.at(pair.begin);
				const auto& end = m_cells.at(pair.end);
				if (!begin.sequence || end.sequence <= begin.sequence || pair.reported == end.sequence ||
				    begin.source == Source::Unknown || end.source == Source::Unknown) continue;
				pair.reported = end.sequence;
				if (begin.source != end.source) ++m_counts.mixed;
				else if (begin.source == Source::Gpu) ++m_counts.gpu;
				else ++m_counts.cpu;
			}
		}
	}
	void Cancel(uint64_t address, uint64_t size) {
		if (!size) return;
		for (auto& [other, cell]: m_cells) {
			if (address <= other ? other - address < size : address - other < 8) cell.source = Source::Unknown;
		}
	}
	Counts Snapshot() const {
		auto counts = m_counts;
		for (const auto& pair: m_pairs) {
			const auto& end = m_cells.at(pair.end);
			if (end.sequence != 0 && pair.reported != end.sequence) ++counts.unresolved;
		}
		return counts;
	}
	uint32_t Size() const { return static_cast<uint32_t>(m_pairs.size()); }

private:
	struct Cell { uint64_t sequence = 0; Source source = Source::Unknown; };
	struct Pair { uint64_t begin, end, reported; };
	std::map<uint64_t, Cell> m_cells;
	std::vector<Pair> m_pairs;
	uint64_t m_sequence = 0;
	Counts m_counts;
};
} // namespace Libs::Graphics
