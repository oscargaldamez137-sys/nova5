#include "graphics/shader/recompiler/CodegenOptions.h"

#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::ShaderRecompiler {
namespace {

CodegenOptions FromEnvironment() {
	CodegenOptions options;
	const auto enabled = [](const char* name) {
		const auto* value = std::getenv(name);
		return value == nullptr || std::strcmp(value, "0") != 0;
	};
	options.ps_append_live_election = enabled("KYTY_PS_APPEND_LIVE_ELECTION");
	options.ps_helper_atomics_skip  = enabled("KYTY_PS_HELPER_ATOMICS_SKIP");
	if (const auto* mode = std::getenv("KYTY_PS_LIVE_EXEC"); mode != nullptr && mode[0] != '\0') {
		options.ps_live_exec = std::strcmp(mode, "all") == 0 ? PsLiveExec::All
		                       : std::strcmp(mode, "0") == 0 ? PsLiveExec::Off
		                                                     : PsLiveExec::AppendConsume;
	}
	return options;
}

CodegenOptions& Storage() {
	static auto options = FromEnvironment();
	return options;
}

} // namespace

const CodegenOptions& GetCodegenOptions() {
	return Storage();
}

void SetCodegenOptions(const CodegenOptions& options) {
	Storage() = options;
}

} // namespace Libs::Graphics::ShaderRecompiler
