#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO_INTERNAL_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO_INTERNAL_H_

#include "common/common.h"

namespace Libs::Audio::AudioInternal {

enum class Format {
	Unknown,
	Signed16bitMono,
	Signed16bitStereo,
	Signed16bit8Ch,
	FloatMono,
	FloatStereo,
	Float8Ch,
	Signed16bit8ChStd,
	Float8ChStd,
	Float12Ch,
};

struct OutputParam {
	int         handle = 0;
	const void* data   = nullptr;
	// One gain for each of the port's channels, from the guest's per-channel gain attribute
	// (AudioOut2 port attribute 1), or null when the guest never set one: the pad's speaker and
	// vibration ports ask for their level there, and without it they play far too quietly.
	const float* gains = nullptr;
};

static constexpr int OUT_PORTS_MAX = 32;

int      AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format);
void     AudioOutClose(int handle);
bool     AudioOutHasDevice(int handle);
uint32_t AudioOutOutputs(const OutputParam* params, uint32_t num, bool blocking = true);

// What the game is told its main output is (KYTY_AUDIO_OUTPUT, the launcher's Audio output; issue #207): the ports'
// connection (AudioOutGetPortState, AudioOut2PortGetState) and the speaker type (AudioOut2GetSpeakerInfo). Demon's Souls
// shows it in Settings > Sound and mixes for it: tv (connection 1, type 0: the default), headphones (connection 0x40),
// amplifier (type 1), soundbar (type 2). (Type 3 played nothing there.)
struct MainOutput {
	uint16_t connection   = 1;
	uint8_t  speaker_type = 0;
};
MainOutput MainOutputReported();

} // namespace Libs::Audio::AudioInternal

#endif // EMULATOR_INCLUDE_EMULATOR_AUDIO_INTERNAL_H_
