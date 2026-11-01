#include "loader/demonsSoulsIdle.h"
#include "loader/guestCode.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void Check(bool pass, const char* text) {
	if (!pass) {
		std::fprintf(stderr, "idle wait: %s\n", text);
		std::abort();
	}
}
} // namespace

int main() {
	using namespace Loader::DemonsSoulsIdle;
	using Loader::GuestCode::Pattern;
	// The idle loop's call site in 01.005.000 and 01.007.000 (eboot+0x820f2d and +0x83b58d), and the two
	// builds' poll prologues.
	constexpr std::array<uint8_t, 38> call {0x48, 0x89, 0xdf, 0x4c, 0x89, 0xf6, 0x48, 0xc7, 0x45, 0xc8, 0x00, 0x00, 0x00,
	                                        0x00, 0xe8, 0xe0, 0x02, 0x00, 0x00, 0x84, 0xc0, 0x74, 0xd7, 0x4c, 0x8b, 0x7d,
	                                        0xc8, 0x49, 0x8b, 0xb7, 0x90, 0x00, 0x00, 0x00, 0x48, 0x85, 0xf6, 0x74};
	constexpr std::array<uint8_t, 23> poll_107 {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
	                                            0x53, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x35};
	auto poll_105 = poll_107;
	poll_105[16]  = 0x98;
	const Pattern idle_call(IdleCall), prologue(PollPrologue);
	Check(idle_call.Valid() && idle_call.Size() == call.size() && call[IdleCallAt] == 0xe8, "call site pattern");
	Check(idle_call.Matches(call.data()), "the known call site");
	auto moved = call;
	moved[IdleCallAt + 2] ^= 0x40;
	Check(idle_call.Matches(moved.data()), "a call to the poll elsewhere");
	auto changed = call;
	changed[IdleCallAt + 7] ^= 1;
	Check(!idle_call.Matches(changed.data()), "modified code beyond the call must be rejected");
	Check(prologue.Valid() && prologue.Matches(poll_107.data()) && prologue.Matches(poll_105.data()), "both polls");
	auto other = poll_107;
	other[13] = 0x4c;
	Check(!prologue.Matches(other.data()), "another prologue must be rejected");
	Check(!Pattern("55 4").Valid() && !Pattern("55 zz").Valid() && !Pattern("?? ??").Valid(), "malformed patterns");
	std::array<uint8_t, 256> image {};
	for (size_t i = 0; i < image.size(); i++) image[i] = static_cast<uint8_t>(i * 7 + 3);
	std::memcpy(image.data() + 100, call.data(), call.size());
	Check(idle_call.Find(image, 2) == std::vector<size_t> {100}, "one match");
	std::memcpy(image.data() + 200, moved.data(), moved.size());
	Check(idle_call.Find(image, 2) == std::vector<size_t> {100, 200}, "two matches");
	Check(idle_call.Find(std::span(image).first(237), 2) == std::vector<size_t> {100}, "a match cut off at the end");
	Check(idle_call.Find(std::span(image).subspan(100, call.size()), 2) == std::vector<size_t> {0}, "an exact fit");
#if defined(__x86_64__) || defined(_M_X64)
	using namespace Xbyak::util;
	uint32_t             waits = 0;
	Xbyak::CodeGenerator code(4096);
	const auto*          sleep = code.getCurr();
	code.mov(rax, reinterpret_cast<uint64_t>(&waits));
	code.inc(dword[rax]);
	for (const auto& r: {rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11})
		code.xor_(r, r);
	code.pxor(xmm0, xmm0);
	code.pxor(xmm15, xmm15);
	code.ret();
	const auto* poll = code.getCurr();
	code.mov(rax, UINT64_C(0x1122334455667700));
	code.or_(rax, rdi); // The success value is AL, not the full return register.
	unsigned i = 1;
	for (const auto& r: {rcx, rdx, rsi, rdi, r8, r9, r10, r11})
		code.mov(r, 0xabcdef00u + i++);
	code.movq(xmm0, rax);
	code.pcmpeqd(xmm15, xmm15);
	code.ret();
	const auto* thunk = code.getCurr();
	EmitThunk(code, poll, sleep);
	// SysV runner: (output, busy, target). Save host callee-saved registers,
	// including Win64's nonvolatile XMM registers through the compiler bridge.
	const auto* run = code.getCurr();
	code.push(r12);
	code.mov(r12, rdi);
	code.mov(rdi, rsi);
	code.call(rdx);
	unsigned offset = 0;
	for (const auto& r: {rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11}) {
		code.mov(qword[r12 + offset], r);
		offset += 8;
	}
	code.movdqu(ptr[r12 + offset], xmm0);
	code.movdqu(ptr[r12 + offset + 16], xmm15);
	code.pop(r12);
	code.ret();
	code.ready();
	Check(!Xbyak::GetError(), "thunk generation");
	using Run         = KYTY_SYSV_ABI void (*)(void*, uint64_t, const void*);
	const auto runner = reinterpret_cast<Run>(reinterpret_cast<uintptr_t>(run));
	for (uint64_t busy: {0, 1}) {
		std::array<uint8_t, 104> expected {}, actual {};
		runner(expected.data(), busy, poll);
		const auto before = waits;
		runner(actual.data(), busy, thunk);
		Check(expected == actual, "poll GPR and SIMD state must survive the host callback");
		Check(waits == before + (busy == 0), "only an empty poll may wait");
	}
#endif
	std::puts("Demon's Souls idle wait tests passed");
}
