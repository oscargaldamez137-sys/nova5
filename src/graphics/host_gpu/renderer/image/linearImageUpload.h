#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics::LinearImageUpload {
struct Rect {
    uint32_t x, y, width, height;
    uint64_t offset, size;
};

// Dirty byte intervals relative to mip 0. Copy only touched texels, never a
// bounding rectangle over clean rows: those may still hold GPU-owned pixels.
// Source row padding is not image content. No compressed blocks or 3D slices.
inline bool Plan(uint32_t width, uint32_t height, uint32_t pitch, uint32_t bytes,
                 uint64_t capacity, std::span<const std::pair<uint64_t, uint64_t>> dirty,
                 bool exact_texels, std::vector<Rect>& result) {
    result.clear();
    if (!width || !height || pitch < width || !bytes || bytes > 16 || (bytes & (bytes - 1))) return false;
    const uint64_t row = uint64_t{pitch} * bytes;
    if (height > UINT64_MAX / row || row * (height - 1) + uint64_t{width} * bytes > capacity) return false;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (auto [begin, end] : dirty) {
        if (begin > end) return false;
        begin = std::min(begin, row * height);
        end = std::min(end, row * height);
        if (begin == end) continue;
        if (exact_texels && ((begin % bytes) || (end % bytes))) return false;
        begin -= begin % bytes;
        end = (end - 1) / bytes * bytes + bytes;
        ranges.emplace_back(begin, end);
    }
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<uint64_t, uint64_t>> merged;
    for (const auto& r : ranges) {
        if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
        else merged.push_back(r);
    }
    for (auto [begin, end] : merged) {
        for (uint64_t y = begin / row; y < height && y * row < end; ++y) {
            const auto a = std::max(begin, y * row);
            const auto b = std::min(end, y * row + uint64_t{width} * bytes);
            if (a >= b) continue;
            const auto x = uint32_t((a - y * row) / bytes);
            const auto count = uint32_t((b - a) / bytes);
            if (!result.empty() && x == 0 && count == pitch && result.back().x == 0 &&
                result.back().width == pitch && result.back().y + result.back().height == y) {
                ++result.back().height;
                result.back().size += b - a;
            } else result.push_back({x, uint32_t(y), count, 1, a, b - a});
            if (result.size() > 512) { result.clear(); return false; }
        }
    }
    return true;
}
} // namespace Libs::Graphics::LinearImageUpload
