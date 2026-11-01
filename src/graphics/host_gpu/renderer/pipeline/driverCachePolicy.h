#pragma once

#include <algorithm>
#include <string_view>

namespace Libs::Graphics {
// The local launcher supplies the SHA-256 of the executable it verified. Only
// this fixed alphabet/length may become a directory component or cache identity.
[[nodiscard]] constexpr bool IsBinaryDriverCacheKey(std::string_view key) {
	return key.size() == 64 && std::ranges::all_of(key, [](char c) {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	});
}
} // namespace Libs::Graphics
