#include "libatrac9.h"

#include <array>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {
void Check(bool ok, const char* message) {
	if (!ok) {
		std::fprintf(stderr, "Atrac9InitializationTests: %s\n", message);
		std::abort();
	}
}

void CheckConfig(void* handle, unsigned rate_index, unsigned channels, unsigned superframe) {
	const unsigned config_word =
	    0xfe000000u | (rate_index << 20u) | (channels << 17u) | (255u << 5u) | (superframe << 3u);
	std::array<unsigned char, 4> config {
	    static_cast<unsigned char>(config_word >> 24u),
	    static_cast<unsigned char>(config_word >> 16u),
	    static_cast<unsigned char>(config_word >> 8u), static_cast<unsigned char>(config_word)};
	Check(Atrac9InitDecoder(handle, config.data()) == 0, "valid initialization rejected");
	Atrac9CodecInfo info {};
	Check(Atrac9GetCodecInfo(handle, &info) == 0, "codec info failed");
	constexpr std::array rates {11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
	                            44100, 48000, 64000, 88200, 96000, 128000, 176400, 192000};
	constexpr std::array channel_counts {1, 2, 2, 6, 8, 4};
	constexpr std::array frame_samples {64, 64, 128, 128, 128, 256, 256, 256};
	Check(info.channels == channel_counts[channels] && info.samplingRate == rates[rate_index] &&
	          info.frameSamples == frame_samples[rate_index % 8] &&
	          info.framesInSuperframe == (1u << superframe) &&
	          info.superframeSize == (256u << superframe),
	      "another decoder configuration leaked into this handle");
}
} // namespace

int main() {
	// A rejected first initialization must not prevent later valid initializations.
	auto* invalid = Atrac9GetHandle();
	Check(invalid != nullptr, "allocation failed");
	std::array<unsigned char, 4> bad_config {};
	Check(Atrac9InitDecoder(invalid, bad_config.data()) != 0, "bad config accepted");
	Atrac9ReleaseHandle(invalid);

	// Start with a cold library. This also exercises initialization publication under TSan.
	std::barrier start(16);
	std::vector<std::jthread> workers;
	for (unsigned rate = 0; rate < 16; ++rate) {
		workers.emplace_back([&, rate] {
			auto* handle = Atrac9GetHandle();
			Check(handle != nullptr, "allocation failed");
			start.arrive_and_wait();
			for (unsigned pass = 0; pass < 4; ++pass)
				for (unsigned channels = 0; channels < 6; ++channels)
					for (unsigned superframe = 0; superframe < 4; ++superframe)
						CheckConfig(handle, (rate + pass) % 16, channels, superframe);
			Atrac9ReleaseHandle(handle);
		});
	}
	workers.clear();
	std::puts("ATRAC9 concurrent initialization and per-handle configuration passed");
}
