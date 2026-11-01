#pragma once

#include "common/abi.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#if defined(__x86_64__) || defined(_M_X64)
#ifndef XBYAK_NO_EXCEPTION
#define XBYAK_NO_EXCEPTION
#endif
#include <xbyak/xbyak.h>
#endif

namespace Loader {
struct Program;
namespace DemonsSoulsIdle {
void Install(Program* program);
void Clear();

// The job workers' idle loop as every build seen has it (01.005.000 and 01.007.000): the call of the poll for
// work, the test of its result and the loop back, the call's displacement left out (IdleCallAt: the call's
// offset in it); and the poll's prologue, its frame size left out (01.005.000 0x98 bytes, 01.007.000 0x88).
inline constexpr std::string_view IdleCall = "48 89 df 4c 89 f6 48 c7 45 c8 00 00 00 00 e8 ?? ?? ?? ?? 84 c0 74 d7 "
                                             "4c 8b 7d c8 49 8b b7 90 00 00 00 48 85 f6 74";
inline constexpr size_t           IdleCallAt   = 14;
inline constexpr std::string_view PollPrologue = "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec ?? ?? ?? ?? 4c 8b 35";

// The wait for work the idle loop calls before its poll: it scans the job queues, then `pause`, reads the TSC (a helper
// call) and goes round again until the worker has been idle for twice the TSC frequency (2 s) before it blocks. Work
// comes every frame, so a worker spins at ~98% of a CPU all through play (01.007.000: eboot+0x83b6fd); the helper call
// then sleeps first (KYTY_SPIN_BACKOFF_US, default 50 us). The pause, the load of the worker's last activity time, the
// helper call (SpinCallAt: the call's offset), the elapsed-time test.
inline constexpr std::string_view SpinCall   = "f3 90 4e 8b bc f3 10 0e 00 00 e8 ?? ?? ?? ?? 4c 29 f8 48 85 c0 0f 8e";
inline constexpr size_t           SpinCallAt = 10;
// The TSC helper it calls: rdtsc; shl rdx, 32; or rax, rdx; ret.
inline constexpr std::string_view TscHelper = "0f 31 48 c1 e2 20 48 09 d0 c3";

#if defined(__x86_64__) || defined(_M_X64)
// The poll's observable register state is retained even on the idle path.
// In particular this is not an ordinary C++ function replacement: its caller
// may rely on registers the original poll was known not to modify.
inline void EmitThunk(Xbyak::CodeGenerator& c, const void* poll, const void* sleep) {
	using namespace Xbyak::util;
	Xbyak::Label done;
	c.sub(rsp, 8);
	c.call(poll);
	c.add(rsp, 8);
	c.test(al, al);
	c.jnz(done, Xbyak::CodeGenerator::T_NEAR);
	c.pushfq();
	for (const auto& reg: {rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11})
		c.push(reg);
	// Entry RSP is 8 mod 16; the 80-byte save above retains that alignment.
	c.sub(rsp, 520);
	constexpr uint8_t save_fp[] {0x48, 0x0f, 0xae, 0x04, 0x24}; // FXSAVE64 [rsp]
	c.db(save_fp, sizeof(save_fp));
	c.mov(rax, reinterpret_cast<uint64_t>(sleep));
	c.call(rax); // Explicit SysV callback bridges to the platform sleep API.
	c.fxrstor64(ptr[rsp]);
	c.add(rsp, 520);
	for (const auto& reg: {r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax})
		c.pop(reg);
	c.popfq();
	c.L(done);
	c.ret();
}

// The TSC helper's stand-in in the wait for work: the backoff (its own call, every register but RAX and RDX and the
// flags kept), then the helper's own result (RAX = the TSC, RDX = its high half shifted, the flags of the OR).
inline void EmitSpinThunk(Xbyak::CodeGenerator& c, const void* backoff) {
	using namespace Xbyak::util;
	c.pushfq();
	for (const auto& reg: {rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11})
		c.push(reg);
	// Entry RSP is 8 mod 16; the 80-byte save keeps that, the 520 bytes make it 0 for FXSAVE and the call.
	c.sub(rsp, 520);
	constexpr uint8_t save_fp[] {0x48, 0x0f, 0xae, 0x04, 0x24}; // FXSAVE64 [rsp]
	c.db(save_fp, sizeof(save_fp));
	c.mov(rax, reinterpret_cast<uint64_t>(backoff));
	c.call(rax); // Explicit SysV callback bridges to the platform sleep API.
	c.fxrstor64(ptr[rsp]);
	c.add(rsp, 520);
	for (const auto& reg: {r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax})
		c.pop(reg);
	c.popfq();
	c.rdtsc();
	c.shl(rdx, 32);
	c.or_(rax, rdx);
	c.ret();
}
#endif
} // namespace DemonsSoulsIdle
} // namespace Loader
