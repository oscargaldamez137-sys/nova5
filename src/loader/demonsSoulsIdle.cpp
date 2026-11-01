#include "loader/demonsSoulsIdle.h"

#include "common/logging/log.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/host_gpu/renderer/demonsSouls.h"
#include "kernel/memory.h"
#include "loader/guestCode.h"
#include "loader/runtimeLinker.h"
#include "loader/nova5DsSwitches.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// KYTY_SPIN_BACKOFF_US=<n> (live kyty_local_spin_backoff_us, default 50): microseconds the job workers' wait for work
// sleeps at each round of its spin (0: it spins as the game does, for 2 s of idleness before it blocks). Same process,
// standing at Boletaria 1-1: a laptop Ryzen 9 5900HX 40.2 -> 41.0 fps (render 18.8 -> 18.0 ms), the 22-thread desktop
// with an RTX 5090 58.6 / 58.4 (noise).
extern "C" {
[[gnu::used]] volatile std::atomic<uint32_t> kyty_local_spin_backoff_us {[] {
	const char* value = std::getenv("KYTY_SPIN_BACKOFF_US");
	return value != nullptr && value[0] != '\0' ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 50u;
}()};
}

namespace Loader::DemonsSoulsIdle {
namespace {
constexpr uint64_t     PageSize = 0x4000, CaveOffset = 0x8000000;
uint64_t               cave = 0, site = 0, spin_site = 0;
std::array<uint8_t, 5> installed_call {}, original_call {}, spin_installed {}, spin_original {};

void KYTY_SYSV_ABI WaitForWork() {
	static const bool enabled = Nova5DsSwitch("KYTY_DS_IDLE");
	if (enabled) Common::Thread::SleepMicroWithoutSpinning(50);
}

void KYTY_SYSV_ABI SpinBackoff() {
	if (const auto us = kyty_local_spin_backoff_us.load(std::memory_order_relaxed); us != 0)
		Common::Thread::SleepMicroWithoutSpinning(us);
}

bool Readable(uint64_t address, uint64_t size) {
	// Executable modules are private runtime allocations, not GPU backing aliases.
	return Libs::Graphics::HostMemoryRangeIsReadable(address, size);
}

// The idle loop's call of the poll (one such call site in the game's code) and the poll it calls.
bool FindSites(const Program& program, uint64_t* call, uint64_t* poll) {
	const auto site = GuestCode::FindUnique(program, GuestCode::Pattern(IdleCall));
	if (!site) return false;
	*call = *site + IdleCallAt;
	int32_t displacement = 0;
	std::memcpy(&displacement, reinterpret_cast<const void*>(*call + 1), sizeof(displacement));
	*poll = *call + 5 + static_cast<int64_t>(displacement);
	const GuestCode::Pattern prologue(PollPrologue);
	const uint64_t           base = program.base_vaddr, end = base + program.mapped_size;
	return *poll >= base && *poll <= end - prologue.Size() && Readable(*poll, prologue.Size()) &&
	       prologue.Matches(reinterpret_cast<const uint8_t*>(*poll));
}

// The wait for work's call of the TSC helper (one such call in the game's code), when it calls that helper: 0 if not.
uint64_t FindSpinCall(const Program& program) {
	const auto found = GuestCode::FindUnique(program, GuestCode::Pattern(SpinCall));
	if (!found) return 0;
	const uint64_t call         = *found + SpinCallAt;
	int32_t        displacement = 0;
	std::memcpy(&displacement, reinterpret_cast<const void*>(call + 1), sizeof(displacement));
	const uint64_t           helper = call + 5 + static_cast<int64_t>(displacement);
	const GuestCode::Pattern body(TscHelper);
	const uint64_t           base = program.base_vaddr, end = base + program.mapped_size;
	return helper >= base && helper <= end - body.Size() && Readable(helper, body.Size()) &&
	               body.Matches(reinterpret_cast<const uint8_t*>(helper))
	           ? call
	           : 0;
}
} // namespace

void Install(Program* program) {
#if defined(__x86_64__) || defined(_M_X64)
	if (cave != 0 || program == nullptr || !Libs::Graphics::DemonsSouls::IsSupportedGame() ||
	    program->file_name.filename() != "eboot.bin" || program->mapped_size > CaveOffset ||
	    program->base_vaddr > UINT64_MAX - CaveOffset - PageSize)
		return;
	uint64_t call = 0, poll = 0;
	if (!Nova5DsSwitch("KYTY_DS_SPIN_BACKOFF")) kyty_local_spin_backoff_us.store(0, std::memory_order_relaxed);
	if (!FindSites(*program, &call, &poll)) {
		std::printf("Demon's Souls idle wait: no idle loop of a known build found; retaining guest code\n");
		return;
	}
	const auto requested = program->base_vaddr + CaveOffset;
	const auto allocated = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    requested, PageSize, Common::VirtualMemory::Mode::ExecuteReadWrite,
	    "demons_souls_idle_wait", true);
	if (allocated != requested) {
		if (allocated) Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
		return;
	}
	Xbyak::ClearError();
	Xbyak::CodeGenerator code(PageSize, reinterpret_cast<void*>(allocated));
	EmitThunk(code, reinterpret_cast<const void*>(poll),
	          reinterpret_cast<const void*>(&WaitForWork));
	// The wait for work's backoff, beside it.
	const uint64_t spin_call  = FindSpinCall(*program);
	uint64_t       spin_thunk = 0;
	if (spin_call != 0) {
		code.align(16);
		spin_thunk = reinterpret_cast<uint64_t>(code.getCurr());
		EmitSpinThunk(code, reinterpret_cast<const void*>(&SpinBackoff));
	}
	code.ready();
	if (Xbyak::GetError() ||
	    !Common::VirtualMemory::FlushInstructionCache(allocated, code.getSize()) ||
	    !Libs::LibKernel::Memory::ProtectGuestMemory(
	        allocated, PageSize, Common::VirtualMemory::Mode::ExecuteRead, nullptr)) {
		Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
		return;
	}
	installed_call[0]       = 0xe8;
	const auto displacement = static_cast<int32_t>(allocated - (call + 5));
	std::memcpy(installed_call.data() + 1, &displacement, sizeof(displacement));
	std::memcpy(original_call.data(), reinterpret_cast<const void*>(call), original_call.size());
	// Installation occurs before module initializers or guest worker threads run.
	// An external patch with different bytes is deliberately left untouched.
	// SetProgramMemoryProtection keeps executable code writable for loader patches.
	std::memcpy(reinterpret_cast<void*>(call), installed_call.data(), installed_call.size());
	if (!Common::VirtualMemory::FlushInstructionCache(call, installed_call.size())) {
		std::memcpy(reinterpret_cast<void*>(call), original_call.data(), original_call.size());
		Common::VirtualMemory::FlushInstructionCache(call, installed_call.size());
		Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
		return;
	}
	cave = allocated;
	site = call;
	std::printf("Demon's Souls idle wait: installed 50 us backoff (call at eboot+0x%llx)\n",
	            static_cast<unsigned long long>(call - program->base_vaddr));
	if (spin_call != 0) {
		spin_installed[0]   = 0xe8;
		const auto to_thunk = static_cast<int32_t>(spin_thunk - (spin_call + 5));
		std::memcpy(spin_installed.data() + 1, &to_thunk, sizeof(to_thunk));
		std::memcpy(spin_original.data(), reinterpret_cast<const void*>(spin_call), spin_original.size());
		std::memcpy(reinterpret_cast<void*>(spin_call), spin_installed.data(), spin_installed.size());
		if (Common::VirtualMemory::FlushInstructionCache(spin_call, spin_installed.size())) {
			spin_site = spin_call;
			std::printf("Demon's Souls job wait: the spin's TSC read at eboot+0x%llx backs off %u us (live "
			            "kyty_local_spin_backoff_us)\n",
			            static_cast<unsigned long long>(spin_call - program->base_vaddr),
			            kyty_local_spin_backoff_us.load(std::memory_order_relaxed));
		} else {
			std::memcpy(reinterpret_cast<void*>(spin_call), spin_original.data(), spin_original.size());
			Common::VirtualMemory::FlushInstructionCache(spin_call, spin_original.size());
		}
	}
#endif
}

void Clear() {
	if (!cave) return;
	std::array<uint8_t, 5> bytes {};
	std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
	if (bytes == installed_call) {
		std::memcpy(reinterpret_cast<void*>(site), original_call.data(), bytes.size());
		Common::VirtualMemory::FlushInstructionCache(site, bytes.size());
	}
	if (spin_site != 0) {
		std::memcpy(bytes.data(), reinterpret_cast<const void*>(spin_site), bytes.size());
		if (bytes == spin_installed) {
			std::memcpy(reinterpret_cast<void*>(spin_site), spin_original.data(), bytes.size());
			Common::VirtualMemory::FlushInstructionCache(spin_site, bytes.size());
		}
	}
	Libs::LibKernel::Memory::FreeGuestMemory(cave, PageSize);
	cave = site = spin_site = 0;
}
} // namespace Loader::DemonsSoulsIdle
