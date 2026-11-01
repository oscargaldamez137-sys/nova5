#ifndef KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_
#define KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_

namespace Loader::X64InstructionEmulator {

[[nodiscard]] bool TryEmulate(void* native_context);

// A guest debug break, int 0x41 (the game's assertion handler breaks into an attached debugger with
// it): in user mode the CPU raises a general protection fault. Without a debugger it does nothing and
// the guest code after it runs (the game reports the failure and goes on). True when it was one.
[[nodiscard]] bool TrySkipDebugBreak(void* native_context);

} // namespace Loader::X64InstructionEmulator

#endif /* KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_ */
