#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_CODEGENOPTIONS_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_CODEGENOPTIONS_H_

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler {

enum class PsLiveExec : uint8_t { Off, AppendConsume, All };

struct CodegenOptions {
	// KYTY_PS_APPEND_LIVE_ELECTION=0 restores election from EXEC including helpers.
	bool ps_append_live_election = true;
	// KYTY_PS_LIVE_EXEC=0|1|all: start with the pixel valid mask for append/consume shaders or all PS.
	PsLiveExec ps_live_exec = PsLiveExec::AppendConsume;
	// KYTY_PS_HELPER_ATOMICS_SKIP=0 restores helper participation in emulated CAS loops.
	bool ps_helper_atomics_skip = true;
};

[[nodiscard]] const CodegenOptions& GetCodegenOptions();
// Test hook; call only while no shader is being compiled.
void SetCodegenOptions(const CodegenOptions& options);

} // namespace Libs::Graphics::ShaderRecompiler

#endif /* EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_CODEGENOPTIONS_H_ */
