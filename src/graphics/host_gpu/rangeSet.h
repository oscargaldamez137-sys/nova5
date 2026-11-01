#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_

#include "common/assert.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <vector>

extern "C" {
// 1: Add returns early for a range already covered, and lookups first try the
// interval the previous lookup found (valid until the set changes).
extern volatile std::atomic<uint32_t> kyty_local_range_set_fast_mode;
}

namespace Libs::Graphics {

class RangeSet final {
public:
	// The owner reads this set from one thread only (lookups then update the hint).
	void AllowFastPath() { m_fast_eligible = true; }

	struct Range {
		uint64_t address = 0;
		uint64_t size    = 0;
	};

	void Add(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		if (Fast() && Covered(address, end)) return;
		++m_version;
		// Intervals that overlap or touch [address, end) merge with it.
		auto first = LowerBound(address);
		if (first != Pos {} && At(Prev(first)).end >= address) {
			first = Prev(first);
		}
		uint64_t begin = address;
		uint64_t last  = end;
		auto     it    = first;
		for (; it != EndPos() && At(it).begin <= last; it = Next(it)) {
			begin = std::min(begin, At(it).begin);
			last  = std::max(last, At(it).end);
		}
		const Interval merged {begin, last};
		Replace(first, it, &merged, 1);
	}

	void Subtract(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		++m_version;
		auto first = LowerBound(address);
		if (first != Pos {} && At(Prev(first)).end > address) {
			first = Prev(first);
		}
		auto last = first;
		while (last != EndPos() && At(last).begin < end) {
			last = Next(last);
		}
		if (first == last) {
			return;
		}
		// What remains of the first and the last interval replaces them all.
		Interval parts[2];
		size_t   count = 0;
		if (At(first).begin < address) {
			parts[count++] = {At(first).begin, address};
		}
		if (const auto tail = At(Prev(last)).end; tail > end) {
			parts[count++] = {end, tail};
		}
		Replace(first, last, parts, count);
	}

	// Subtract of each of `ranges` (sorted by address, disjoint: elements with address and size), the intervals they
	// overlap replaced once (a readback's parts, a download's copies).
	template <typename Ranges>
	void SubtractSorted(const Ranges& ranges) {
		if (std::empty(ranges) || Empty()) return;
		for (auto it = std::next(std::begin(ranges)); it != std::end(ranges); ++it) {
			if (it->address < End(std::prev(it)->address, std::prev(it)->size)) {
				// (Not sorted and disjoint: one at a time.)
				for (const auto& range: ranges) Subtract(range.address, range.size);
				return;
			}
		}
		const auto begin = std::begin(ranges)->address;
		const auto end   = End(std::prev(std::end(ranges))->address, std::prev(std::end(ranges))->size);
		auto       first = LowerBound(begin);
		if (first != Pos {} && At(Prev(first)).end > begin) {
			first = Prev(first);
		}
		auto last = first;
		while (last != EndPos() && At(last).begin < end) {
			last = Next(last);
		}
		if (first == last) {
			return;
		}
		++m_version;
		// What remains of [first, last): each interval cut by the ranges that overlap it.
		thread_local Intervals kept;
		kept.clear();
		auto range = std::begin(ranges);
		for (auto it = first; it != last; it = Next(it)) {
			const auto& interval = At(it);
			while (range != std::end(ranges) && End(range->address, range->size) <= interval.begin) ++range;
			uint64_t at = interval.begin;
			for (auto cut = range; at < interval.end; ++cut) {
				if (cut == std::end(ranges) || cut->address >= interval.end) {
					kept.push_back({at, interval.end});
					break;
				}
				if (cut->address > at) kept.push_back({at, cut->address});
				at = std::max(at, End(cut->address, cut->size));
			}
		}
		Replace(first, last, kept.data(), kept.size());
	}

	void Clear() {
		++m_version;
		m_blocks.clear();
		m_heads.clear();
		m_count = 0;
	}

	template <typename Func>
	void ForEach(Func&& func) const {
		for (const auto& block: m_blocks) {
			for (const auto& range: block) {
				func(range.begin, range.end);
			}
		}
	}

	[[nodiscard]] std::vector<Range> Intersections(uint64_t address, uint64_t size) const {
		std::vector<Range> result;
		ForEachIntersection(address, size, [&result](Range range) { result.push_back(range); });
		return result;
	}

	[[nodiscard]] bool Intersects(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		const auto it  = LowerBound(address);
		if (it != Pos {} && At(Prev(it)).end > address) {
			return true;
		}
		return it != EndPos() && At(it).begin < end;
	}

	[[nodiscard]] bool Contains(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		if (Fast()) return Covered(address, end);
		const auto it = UpperBound(address);
		if (it == Pos {}) {
			return false;
		}
		const auto& range = At(Prev(it));
		return range.begin <= address && range.end >= end;
	}

	template <typename Func>
	void ForEachIntersection(uint64_t address, uint64_t size, Func&& func) const {
		const auto end = End(address, size);
		auto       it  = UpperBound(address);
		if (it != Pos {}) {
			it = Prev(it);
		}
		for (; it != EndPos() && At(it).begin < end; it = Next(it)) {
			const auto begin = std::max(address, At(it).begin);
			const auto last  = std::min(end, At(it).end);
			if (begin < last) {
				func(Range {begin, last - begin});
			}
		}
	}

	// The intersections with each interval of `queries` in turn, as ForEachIntersection gives them one query at a time:
	// one search for the first, then a walk (the queries are sorted and disjoint, as a set's own). A search per query
	// against ~50K GPU-written ranges after an area streamed in took 80 ms of a walk's slow frames (guest readbacks).
	template <typename Func>
	void ForEachIntersection(const RangeSet& queries, Func&& func) const {
		if (queries.Empty() || Empty()) return;
		auto it = UpperBound(queries.m_blocks.front().front().begin);
		if (it != Pos {}) {
			it = Prev(it);
		}
		queries.ForEach([&](uint64_t query_begin, uint64_t query_end) {
			while (it != EndPos() && At(it).end <= query_begin) {
				it = Next(it);
			}
			for (auto at = it; at != EndPos() && At(at).begin < query_end; at = Next(at)) {
				const auto begin = std::max(query_begin, At(at).begin);
				const auto last  = std::min(query_end, At(at).end);
				if (begin < last) {
					func(Range {begin, last - begin});
				}
			}
		});
	}

	[[nodiscard]] bool Empty() const { return m_count == 0; }
	[[nodiscard]] size_t Count() const { return m_count; }

private:
	[[nodiscard]] bool Fast() const {
		return m_fast_eligible && kyty_local_range_set_fast_mode.load(std::memory_order_relaxed) != 0;
	}

	// [address, end) lies inside one interval; remembers that interval. An interval found at
	// version V covers the same bytes while the set stays at V: a few are kept, since lookups
	// alternate between ranges (a dispatch's written buffers).
	bool Covered(uint64_t address, uint64_t end) const {
		for (const auto& hint: m_hints)
			if (hint.version == m_version && hint.begin <= address && end <= hint.end) return true;
		const auto it = UpperBound(address);
		if (it == Pos {}) return false;
		const auto& range = At(Prev(it));
		if (range.begin > address || range.end < end) return false;
		m_hints[m_next_hint++ % m_hints.size()] = {m_version, range.begin, range.end};
		return true;
	}

	// Disjoint, non-adjacent intervals sorted by address, in blocks of at most BlockMax (none empty) with each block's
	// first address in m_heads: lookups (thousands per frame, most of them for SRT words and bindings) search two short
	// arrays; a change moves one block's tail. (One array of the thousands of GPU-written ranges moved its tail at each
	// insertion and removal: 2% of the GPU thread at the 1-1 ramparts.)
	struct Interval {
		uint64_t begin = 0, end = 0;
	};
	using Intervals = std::vector<Interval>;
	static constexpr size_t BlockMax = 128;

	// An interval's place: its block and its index there ({block count, 0}: the end; {0, 0}: the first interval).
	struct Pos {
		size_t block = 0, index = 0;
		bool   operator==(const Pos& other) const = default;
	};
	[[nodiscard]] Pos EndPos() const { return {m_blocks.size(), 0}; }
	[[nodiscard]] const Interval& At(Pos pos) const { return m_blocks[pos.block][pos.index]; }
	[[nodiscard]] Pos Next(Pos pos) const {
		return ++pos.index < m_blocks[pos.block].size() ? pos : Pos {pos.block + 1, 0};
	}
	[[nodiscard]] Pos Prev(Pos pos) const {
		return pos.index != 0 ? Pos {pos.block, pos.index - 1} : Pos {pos.block - 1, m_blocks[pos.block - 1].size() - 1};
	}
	// The first interval beginning at or after the address (LowerBound), after it (UpperBound): in the last block whose
	// first one begins before (at or before) it, else the next block's first.
	[[nodiscard]] Pos LowerBound(uint64_t address) const {
		const auto block = static_cast<size_t>(std::lower_bound(m_heads.begin(), m_heads.end(), address) - m_heads.begin());
		if (block == 0) return {};
		const auto& ranges = m_blocks[block - 1];
		const auto  index  = static_cast<size_t>(
            std::lower_bound(ranges.begin(), ranges.end(), address,
                             [](const Interval& range, uint64_t value) { return range.begin < value; }) -
            ranges.begin());
		return index < ranges.size() ? Pos {block - 1, index} : Pos {block, 0};
	}
	[[nodiscard]] Pos UpperBound(uint64_t address) const {
		const auto block = static_cast<size_t>(std::upper_bound(m_heads.begin(), m_heads.end(), address) - m_heads.begin());
		if (block == 0) return {};
		const auto& ranges = m_blocks[block - 1];
		const auto  index  = static_cast<size_t>(
            std::upper_bound(ranges.begin(), ranges.end(), address,
                             [](uint64_t value, const Interval& range) { return value < range.begin; }) -
            ranges.begin());
		return index < ranges.size() ? Pos {block - 1, index} : Pos {block, 0};
	}

	// The intervals [first, last) replaced with `count` intervals that keep the order.
	void Replace(Pos first, Pos last, const Interval* items, size_t count) {
		if (m_blocks.empty()) {
			if (count == 0) return;
			m_blocks.emplace_back(items, items + count);
			m_heads.push_back(items[0].begin);
			m_count = count;
			Fix(0);
			return;
		}
		// (The end is the last block's end; a block's start, the previous block's end when it closes a removal.)
		const auto at_end = [&](Pos& pos) {
			if (pos.block == m_blocks.size()) pos = {pos.block - 1, m_blocks[pos.block - 1].size()};
		};
		at_end(first);
		at_end(last);
		if (last.index == 0 && last.block > first.block) last = {last.block - 1, m_blocks[last.block - 1].size()};
		if (first.block == last.block) {
			auto& block = m_blocks[first.block];
			m_count     = m_count - (last.index - first.index) + count;
			block.erase(block.begin() + static_cast<ptrdiff_t>(first.index), block.begin() + static_cast<ptrdiff_t>(last.index));
			block.insert(block.begin() + static_cast<ptrdiff_t>(first.index), items, items + count);
			Fix(first.block);
			return;
		}
		// The first block's tail, the blocks between and the last block's head go; the items end the first block.
		auto& head_block = m_blocks[first.block];
		auto& tail_block = m_blocks[last.block];
		size_t removed   = (head_block.size() - first.index) + last.index;
		for (auto b = first.block + 1; b < last.block; ++b) removed += m_blocks[b].size();
		m_count = m_count - removed + count;
		tail_block.erase(tail_block.begin(), tail_block.begin() + static_cast<ptrdiff_t>(last.index));
		head_block.erase(head_block.begin() + static_cast<ptrdiff_t>(first.index), head_block.end());
		head_block.insert(head_block.end(), items, items + count);
		m_blocks.erase(m_blocks.begin() + static_cast<ptrdiff_t>(first.block + 1),
		               m_blocks.begin() + static_cast<ptrdiff_t>(last.block));
		m_heads.erase(m_heads.begin() + static_cast<ptrdiff_t>(first.block + 1),
		              m_heads.begin() + static_cast<ptrdiff_t>(last.block));
		Fix(first.block + 1); // (the last block, next to the first now: fixed first, the first block's index holds)
		Fix(first.block);
	}

	// A changed block: removed when empty, split when over BlockMax, joined with the next when both are small; its
	// head set again.
	void Fix(size_t b) {
		if (m_blocks[b].empty()) {
			m_blocks.erase(m_blocks.begin() + static_cast<ptrdiff_t>(b));
			m_heads.erase(m_heads.begin() + static_cast<ptrdiff_t>(b));
			return;
		}
		if (m_blocks[b].size() > BlockMax) {
			std::vector<Intervals> pieces;
			auto&                  block = m_blocks[b];
			for (size_t at = BlockMax / 2; at < block.size(); at += BlockMax / 2)
				pieces.emplace_back(block.begin() + static_cast<ptrdiff_t>(at),
				                    block.begin() + static_cast<ptrdiff_t>(std::min(block.size(), at + BlockMax / 2)));
			block.resize(BlockMax / 2);
			for (size_t i = 0; i < pieces.size(); ++i) {
				m_heads.insert(m_heads.begin() + static_cast<ptrdiff_t>(b + 1 + i), pieces[i].front().begin);
				m_blocks.insert(m_blocks.begin() + static_cast<ptrdiff_t>(b + 1 + i), std::move(pieces[i]));
			}
		} else if (b + 1 < m_blocks.size() && m_blocks[b].size() + m_blocks[b + 1].size() <= BlockMax / 2) {
			auto& next = m_blocks[b + 1];
			m_blocks[b].insert(m_blocks[b].end(), next.begin(), next.end());
			m_blocks.erase(m_blocks.begin() + static_cast<ptrdiff_t>(b + 1));
			m_heads.erase(m_heads.begin() + static_cast<ptrdiff_t>(b + 1));
		}
		m_heads[b] = m_blocks[b].front().begin;
	}

	static uint64_t End(uint64_t address, uint64_t size) {
		if (size == 0 || size > UINT64_MAX - address) {
			EXIT("invalid range-set address or size\n");
		}
		return address + size;
	}

	std::vector<Intervals> m_blocks;
	std::vector<uint64_t>  m_heads;
	size_t                 m_count         = 0;
	bool                   m_fast_eligible = false;
	uint64_t               m_version       = 1;
	struct Hint {
		uint64_t version = 0, begin = 0, end = 0;
	};
	mutable std::array<Hint, 4> m_hints {};
	mutable uint32_t            m_next_hint = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
