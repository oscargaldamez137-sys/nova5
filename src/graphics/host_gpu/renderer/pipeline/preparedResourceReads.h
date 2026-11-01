#pragma once
// Ported from jetsku/main pipelineCache.cpp, PreparedResourceReads (6a7f10ee).
// Compare exact observed bytes; inject the clean reader to exercise this in host tests.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>
#include <vector>
namespace Libs::Graphics {
class PreparedResourceReads {
public:
	static constexpr size_t MaxBytes = 32u * 1024u;
	static constexpr size_t MaxRanges = 256u;

	void Begin(std::span<const uint32_t> user_data, uint64_t shader_base,
	           std::span<uint8_t, MaxBytes> scratch) {
		m_valid = false;
		m_recordable = true;
		m_ranges.clear();
		m_storage.clear();
		m_capture_scratch = scratch;
		m_user_data.assign(user_data.begin(), user_data.end());
		m_shader_base = shader_base;
	}

	void Invalidate() { m_valid = false; }
	bool Valid() const { return m_valid; }
	size_t CapacityBytes() const {
		return m_ranges.capacity() * sizeof(Range) + m_storage.capacity() +
		       m_user_data.capacity() * sizeof(uint32_t);
	}

	void Finish(bool success) {
		m_valid = success && m_recordable;
		m_capture_scratch = {};
	}

	static void Observe(void* userdata, uint64_t address, std::span<const uint32_t> values,
	                    bool success) {
		static_cast<PreparedResourceReads*>(userdata)->Record(address, values, success);
	}

	bool Matches(std::span<const uint32_t> user_data, uint64_t shader_base,
	             std::span<const uint32_t> dependencies, bool projected) const {
		if (!m_valid || shader_base != m_shader_base || user_data.size() != m_user_data.size()) return false;
		if (!projected) return std::ranges::equal(user_data, m_user_data);
		for (const auto index: dependencies) {
			if (index >= user_data.size() || user_data[index] != m_user_data[index]) return false;
		}
		return true;
	}

	template <typename Reader>
	bool Validate(std::span<uint8_t, MaxBytes> scratch, Reader&& read) const {
		if (!m_valid) return false;
		for (const auto& range: m_ranges) {
			if (!read(range.address, scratch.data(), range.size) ||
			    std::memcmp(scratch.data(), m_storage.data() + range.offset, range.size) != 0)
				return false;
		}
		return true;
	}
private:
	struct Range {
		uint64_t address;
		size_t offset;
		size_t size;
		uint64_t End() const { return address + size; }
	};

	void Record(uint64_t address, std::span<const uint32_t> values, bool success) {
		if (!m_recordable) return;
		constexpr uint64_t gpu_limit = uint64_t {1} << 40u;
		const auto bytes = values.size_bytes();
		if (!success || bytes == 0 || bytes > MaxBytes || address == 0 ||
		    address >= gpu_limit || bytes >= gpu_limit - address) {
			m_recordable = false;
			return;
		}
		const auto end = address + bytes;
		const auto* observed = reinterpret_cast<const uint8_t*>(values.data());
		auto first = std::lower_bound(m_ranges.begin(), m_ranges.end(), address,
		                             [](const Range& range, uint64_t value) {
			                             return range.address < value;
		                             });
		if (first != m_ranges.begin() && std::prev(first)->End() >= address) --first;
		auto last = first;
		uint64_t begin_union = address;
		uint64_t end_union = end;
		size_t replaced_bytes = 0;
		while (last != m_ranges.end() && last->address <= end_union) {
			// Reject differing snapshots of any overlapping bytes. No final reread is
			// substituted for what a branch, pointer or descriptor actually observed.
			const auto begin_overlap = std::max(address, last->address);
			const auto end_overlap = std::min(end, last->End());
			if (begin_overlap < end_overlap &&
			    std::memcmp(observed + (begin_overlap - address),
			                m_storage.data() + last->offset + (begin_overlap - last->address),
			                static_cast<size_t>(end_overlap - begin_overlap)) != 0) {
				m_recordable = false;
				return;
			}
			begin_union = std::min(begin_union, last->address);
			end_union = std::max(end_union, last->End());
			replaced_bytes += last->size;
			++last;
		}
		const auto merged_bytes = static_cast<size_t>(end_union - begin_union);
		const auto replaced_ranges = static_cast<size_t>(last - first);
		const auto stored_bytes = m_storage.size();
		const auto new_bytes = stored_bytes - replaced_bytes + merged_bytes;
		if (new_bytes > MaxBytes ||
		    m_ranges.size() - replaced_ranges + 1u > MaxRanges) {
			m_recordable = false;
			return;
		}
		if (replaced_ranges == 1u && begin_union == first->address &&
		    end_union == first->End()) return;
		// Allocate at most one bounded byte arena per source, then retain it across
		// misses. Range metadata likewise keeps its capacity when Begin clears it.
		if (m_storage.capacity() < MaxBytes) m_storage.reserve(MaxBytes);
		if (replaced_ranges == 1u && begin_union == first->address && last == m_ranges.end()) {
			// The usual ascending SRT sequence extends the final range in place.
			m_storage.resize(new_bytes);
			std::memcpy(m_storage.data() + first->offset + (address - begin_union), observed, bytes);
			first->size = merged_bytes;
			return;
		}
		// Existing ranges are packed in address order in the arena. Reconstruct the
		// changed union in shared scratch, shift its packed suffix, and keep no holes.
		const auto offset = first != m_ranges.end() ? first->offset : stored_bytes;
		const auto old_suffix = offset + replaced_bytes;
		for (auto it = first; it != last; ++it) {
			std::memcpy(m_capture_scratch.data() + (it->address - begin_union),
			            m_storage.data() + it->offset, it->size);
		}
		std::memcpy(m_capture_scratch.data() + (address - begin_union), observed, bytes);
		m_storage.resize(new_bytes);
		std::memmove(m_storage.data() + offset + merged_bytes,
		             m_storage.data() + old_suffix, stored_bytes - old_suffix);
		std::memcpy(m_storage.data() + offset, m_capture_scratch.data(), merged_bytes);
		const auto position = m_ranges.erase(first, last);
		for (auto it = position; it != m_ranges.end(); ++it) {
			it->offset += merged_bytes - replaced_bytes;
		}
		m_ranges.insert(position, Range {begin_union, offset, merged_bytes});
	}

	std::vector<Range> m_ranges;
	std::vector<uint8_t> m_storage;
	std::vector<uint32_t> m_user_data;
	std::span<uint8_t> m_capture_scratch;
	uint64_t m_shader_base = 0;
	bool m_valid = false;
	bool m_recordable = false;
};
} // namespace Libs::Graphics
