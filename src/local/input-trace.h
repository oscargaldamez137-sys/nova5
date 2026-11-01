#pragma once

#include <cstdlib>
#include <string_view>

// Opt-in diagnostics only; does not inject input or change connection/routing.
namespace InputTrace {
inline bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_INPUT_TRACE");
		return value != nullptr && std::string_view(value) == "1";
	}();
	return enabled;
}
}
