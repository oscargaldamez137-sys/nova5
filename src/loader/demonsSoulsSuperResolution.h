#pragma once

#include <cstdint>
#include <string_view>

namespace Loader {
struct Program;
// KYTY_UPSCALER=dlss (run-windows.ps1 -Upscaler, the launcher's "Upscaler"): Demon's Souls 01.007.000's own PS5 Pro
// path. On a PS5 Pro (sceKernelIsTrinityMode) the game's performance mode is its display mode 5: the 3D rendered at
// 2688x1512, then Sony's super resolution (PSSR, libScePsml) in place of its TAA makes the 3840x2160 image the rest of
// the frame (bloom, depth of field, motion blur, the HUD) is drawn on; elsewhere mode 3: 2560x1440, the TAA at that
// size, the 4K output a spatial upscale in the final composite. Here the renderer makes its SuperResolution object
// (the one sceKernelIsTrinityMode check before it, nothing else of the PS5 Pro: its shaders, memory and settings stay
// the PS5's) and the performance mode's render config asks for mode 5 (kernel/fileSystem.cpp: loader/gameArgs.h);
// libs/libPsml.cpp hands the dispatch to the emulator's upscaler (graphics/guest_gpu/superResolution.h). Only where
// DLSS is ready on the device (frame-gen.h UpscalerReady, made before the game is loaded); elsewhere the game's TAA.
// KYTY_RENDER_HEIGHT then sizes mode 5 (loader/demonsSoulsRenderSize.h): 1080 and up, or 720 with SetLeastHeight (the
// game's super resolution is off below its least height, SuperResolution::IsEnabled).
// With KYTY_HD_OUTPUT=1 too, the same path makes a 1920x1080 output instead of 3840x2160 (the upscaler's image, the
// processing after it and the interface at 1080p, loader/demonsSoulsHdOutput.h, demonsSoulsRenderSize.h): DLSS from
// a lower render height to 1080p, where the game's own HD path has no upscaler (its TAA, bloom and depth of field run
// at the render size, the final composite stretches them). Its least render height is then the render height itself
// (SetLeastHeight).
namespace DemonsSoulsSuperResolution {
// Before DemonsSoulsHdOutput::Install, which takes only the interface's part of the HD path when this is on.
void Install(Program* program);
// Install turned the game's super resolution path on.
[[nodiscard]] bool Active();
// ... with the HD output (KYTY_HD_OUTPUT): a 1920x1080 output.
[[nodiscard]] bool HdOutput();
// The least view height the game's super resolution runs at (its 1080 in SuperResolution::IsEnabled), for a render
// height below 1080 with the HD output: a constant of its own in the padding after the function, the comparison
// pointed at it (the game's 1080.0 is shared). False when the function was not found.
bool SetLeastHeight(Program* program, uint32_t height);

// SuperResolution::IsEnabled (01.007.000 0x900a53700): mode == 5 (lea rax, [rip + x]; mov rax, [rax]; cmp dword
// [rax + 0x3c], 5; jne), then the view's height (vmovss xmm0, [rax + 4]) vucomiss [rip + 1080.0]; setae al; ret;
// xor eax, eax; ret; and 8 bytes of int3 padding.
inline constexpr std::string_view EnabledCheck =
    "48 8d 05 ?? ?? ?? ?? 48 8b 00 83 78 3c 05 75 ?? 48 8b 46 50 c5 fa 10 40 04 c5 f8 2e 05 ?? ?? ?? ?? 0f 93 c0 c3 31 c0 "
    "c3 cc cc cc cc";
inline constexpr uint32_t EnabledCompareDisp = 0x1d, EnabledCompareNext = 0x21, EnabledPadding = 0x28;
} // namespace DemonsSoulsSuperResolution
} // namespace Loader
