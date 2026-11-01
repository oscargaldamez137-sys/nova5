#pragma once
#include "loader/systemContent.h"
#include <cstdlib>
#include <string>
#include <string_view>

namespace Loader {
// Same title/version gate and boolean semantics as nova5:demonsSoulsOptions.h.
// Absent launcher switches retain the unified core's tested defaults. Other builds
// retain their own signature-checked adaptations, unaffected by launcher defaults.
inline bool Nova5DsSupported() {
	static const bool supported = [] {
		std::string title, version;
		SystemContentParamSfoGetString("TITLE_ID", &title);
		SystemContentParamSfoGetString("APP_VER", &version);
		return title == "PPSA01341" && version == "01.007.000";
	}();
	return supported;
}
inline bool Nova5DsSwitch(const char* name, bool fallback = true) {
	if (!Nova5DsSupported()) return fallback;
	const auto* value = std::getenv(name);
	return value == nullptr ? fallback : std::string_view(value) == "1";
}
}
