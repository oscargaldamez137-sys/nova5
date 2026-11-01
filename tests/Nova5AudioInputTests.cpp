#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "common/logging/log.h"
#include "kernel/pthread.h"
#include "kernel/semaphore.h"
#include "libs/audio.h"
#include "libs/controller.h"
#include "libs/dualSenseHaptics.h"
#include "libs/errno.h"
#define SDL_MAIN_HANDLED
#include "SDL.h"
#include <cstdio>
#include <cstdlib>
#include <array>
#ifdef _WIN32
#include <windows.h>
#endif

// This harness has no guest clock, GPU, game or real microphone. SDL's dummy backend only.
namespace Libs::LibKernel {
uint64_t KYTY_SYSV_ABI KernelGetProcessTime() { return SDL_GetTicks64() * 1000u; }
}
// audio.cpp also contains output/3D entry points. They must never run in an input test:
// fail if a regression reaches a controller or starts a 3D playback worker.
namespace Libs::Controller {
void RumbleFromVibrationAudio(const void*, uint32_t, uint32_t, bool, const float*) { std::abort(); }
namespace DualSenseHaptics {
bool Open(bool) { std::abort(); }
void Close(bool) { std::abort(); }
bool Available() { std::abort(); }
bool SpeakerActive() { std::abort(); }
bool Write(const void*, uint32_t, uint32_t, uint32_t, bool, bool, const float*) { std::abort(); }
}
}
namespace Libs::LibKernel::Semaphore {
int KYTY_SYSV_ABI KernelCreateSema(KernelSema*, const char*, uint32_t, int, int, void*) { std::abort(); }
int KYTY_SYSV_ABI KernelWaitSema(KernelSema, int, KernelUseconds*) { std::abort(); }
int KYTY_SYSV_ABI KernelSignalSema(KernelSema, int) { std::abort(); }
}
int main() {
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
	Common::InitializeThreads();
	Config::Initialize();
	Config::ConfigOptions options;
	Config::Load(options);
	Log::Initialize();
	Libs::Audio::Initialize();
	int failures = 0;
	const auto check = [&](bool ok, const char* name) {
		if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
	};
	using namespace Libs::Audio::AudioIn;
	using namespace Libs::Audio;
	check(AudioInOpen(1000, 1, 0, 0, 48000, 1) == AUDIO_IN_ERROR_INVALID_SIZE, "zero-length grain rejected");
	check(AudioInOpen(1000, 1, 0, 256, 0, 1) == AUDIO_IN_ERROR_INVALID_FREQ, "zero frequency rejected");
	for (int iteration = 0; iteration < 16; ++iteration) {
		// Reopen more than the eight available ports: Close must release the slot.
		const int handle = AudioInOpen(1000, 1, 0, 256, 48000, 1);
		check(handle > 0, "input open");
		if (handle <= 0) break;
		check(AudioInGetSilentState(handle) == 1, "no selected microphone means device-none");
		std::array<int16_t, 258> samples;
		samples.fill(1234);
		check(AudioInInput(handle, samples.data()) == 256, "sample count");
		for (size_t i = 0; i < 256; ++i) check(samples[i] == 0, "silence initializes each guest sample");
		check(samples[256] == 1234 && samples[257] == 1234, "does not overwrite past grain");
		check(AudioInClose(handle) == 0, "input close");
		check(AudioInClose(handle) == AUDIO_IN_ERROR_INVALID_HANDLE, "double close rejected");
	}
	for (const auto param: {2u, 0x10u, 0x11u, 0x12u}) {
		const int handle = AudioInOpen(1000, 0, 0, 128, 16000, param);
		check(handle > 0, "stereo/float and standard input type");
		if (handle <= 0) continue;
		std::array<uint8_t, 1032> bytes;
		bytes.fill(0xcd);
		const size_t size = 128 * (param == 0x12 ? 8 : 4);
		check(AudioInInput(handle, bytes.data()) == 128, "stereo/float sample count");
		for (size_t i = 0; i < size; ++i) check(bytes[i] == 0, "stereo/float silence");
		for (size_t i = size; i < bytes.size(); ++i) check(bytes[i] == 0xcd, "stereo/float boundary");
		AudioInClose(handle);
	}
	options.audio_input_device = "Nova5 harness nonexistent microphone";
	Config::Load(options);
	const int missing = AudioInOpen(1000, 1, 0, 256, 48000, 2);
	check(missing > 0 && AudioInGetSilentState(missing) == 1, "missing named microphone falls back to silence");
	if (missing > 0) AudioInClose(missing);
	Libs::Audio::Shutdown();
	Log::Shutdown();
	Config::Shutdown();
	SDL_Quit();
	std::printf("Nova5 audio input tests: %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures != 0;
}
