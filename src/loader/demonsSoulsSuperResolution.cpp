#include "loader/demonsSoulsSuperResolution.h"
#include "loader/demonsSoulsHdOutput.h"

#include "graphics/host_gpu/renderer/demonsSouls.h"
#include "loader/guestCode.h"
#include "loader/runtimeLinker.h"
#include "local/frame-gen.h"
#include "local/fsr-native.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace Loader::DemonsSoulsSuperResolution {
namespace {

// The renderer's constructor: `if (IsTrinity()) mSuperResolution = new SuperResolution` (0x88 bytes; 01.007.000
// eboot+0x9e4b33): the call, its test, the allocation and the constructor's call.
constexpr std::string_view CreateCheck = "e8 ?? ?? ?? ?? 84 c0 74 ?? bf 88 00 00 00 e8 ?? ?? ?? ?? 48 89 c7 49 89 c7 e8";
// mov al, 1; nop dword [rax] (the call's five bytes).
constexpr uint8_t Taken[5] {0xb0, 0x01, 0x0f, 0x1f, 0x00};

bool g_active = false;
bool g_hd     = false; // with the HD output (KYTY_HD_OUTPUT): a 1920x1080 output

} // namespace

bool Active() {
	return g_active;
}

bool HdOutput() {
	return g_hd;
}

bool SetLeastHeight(Program* program, uint32_t height) {
	const auto site = program != nullptr ? GuestCode::FindUnique(*program, GuestCode::Pattern(EnabledCheck)) : std::nullopt;
	if (!site) return false;
	// Installation occurs before module initializers or guest threads run; program memory is writable to the host.
	const float value        = static_cast<float>(height);
	const auto  displacement = static_cast<int32_t>(EnabledPadding - EnabledCompareNext);
	std::memcpy(reinterpret_cast<void*>(*site + EnabledPadding), &value, sizeof(value));
	std::memcpy(reinterpret_cast<void*>(*site + EnabledCompareDisp), &displacement, sizeof(displacement));
	return true;
}

void Install(Program* program) {
	const char* upscaler = std::getenv("KYTY_UPSCALER");
	// (FSR 3 built in, Linux: any GPU, nothing to wait for on the device.)
	bool fsr = Libs::Graphics::FsrNative::UpscalerRequested();
	if (upscaler == nullptr || (std::string_view(upscaler) != "dlss" && !fsr) || program == nullptr ||
	    !Libs::Graphics::DemonsSouls::IsSupportedGame() || program->file_name.filename() != "eboot.bin")
		return;
	// (The device exists by now: without DLSS on it, FSR 3 built in - issue #85: DLSS asked for with FSR 3 frame
	// generation on left an RTX 2060 the game's TAA from 540 lines -, else the game's own TAA rather than a stretch.)
	if (!fsr && !Libs::Graphics::FrameGen::UpscalerReady()) {
		const char* reason = Libs::Graphics::FrameGen::UsesFsr()
		                         ? "beside AMD FSR 3 frame generation it comes through NVIDIA's Streamline, which did not "
		                           "start it (or KYTY_FSR_FG_DLSS=0)"
		                         : "it needs an NVIDIA RTX GPU and NVIDIA's DLSS runtime: Streamline on Windows, "
		                           "libkyty_dlss.so on Linux";
		if (!Libs::Graphics::FsrNative::UpscaleInsteadOfDlss()) {
			std::printf("Demon's Souls super resolution: DLSS is not available here (%s); the game's own TAA kept\n", reason);
			return;
		}
		std::printf("Demon's Souls super resolution: DLSS is not available here (%s); FSR 3 upscaling instead\n", reason);
		fsr = true;
	}
	const auto site = GuestCode::FindUnique(*program, GuestCode::Pattern(CreateCheck));
	if (!site) {
		std::printf("Demon's Souls super resolution: this build has no PS5 Pro path (01.007.000 has); the game's own TAA kept\n");
		return;
	}
	// Installation occurs before module initializers or guest threads run; program memory is writable to the host.
	std::memcpy(reinterpret_cast<void*>(*site), Taken, sizeof(Taken));
	g_active = true;
	// (Its 1920x1080 output: DemonsSoulsHdOutput::Install takes the interface's part, DemonsSoulsRenderSize the sizes.)
	g_hd = DemonsSoulsHdOutput::Requested();
	std::printf("Demon's Souls super resolution: the PS5 Pro path (display mode 5, %s in place of the TAA) is on", fsr ? "FSR 3" : "DLSS");
	if (g_hd)
		std::printf(", its output %ux%u (%s)", DemonsSoulsHdOutput::UpscaledWidth(), DemonsSoulsHdOutput::UpscaledHeight(),
		            DemonsSoulsHdOutput::CustomOutputHeight() != 0 ? "KYTY_OUTPUT_HEIGHT" : "KYTY_HD_OUTPUT");
	std::printf("\n");
}

} // namespace Loader::DemonsSoulsSuperResolution
