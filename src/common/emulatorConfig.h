#ifndef KYTY_COMMON_EMULATOR_CONFIG_H_
#define KYTY_COMMON_EMULATOR_CONFIG_H_

#include "common/common.h"

#include <cstddef>
#include <array>
#include <optional>
#include <filesystem>
#include <string>
#include <vector>

namespace Config {

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr const char* name       = "Config";
	static constexpr auto        initialize = Config::Initialize;
	static constexpr auto        shutdown   = Config::Shutdown;
};

enum class ShaderOptimizationType { None, Size, Performance };

enum class ShaderLogDirection { Silent, Console, File };

enum class ProfilerDirection { None, Network };

enum class OutputDirection { Silent, Console, File };

enum class PresentMode { Fifo, Mailbox, Immediate };

// Auto fixes missing ISA extensions; Full also applies Nova5's precise VRSQRTPS.
enum class GuestCpuMode { Auto, Full, Off };

using Keymap = std::vector<std::string>;
using ControllerColor = std::array<uint8_t, 3>;

constexpr uint32_t DEFAULT_CONSOLE_LANGUAGE = 1;
constexpr uint32_t MAX_CONSOLE_LANGUAGE     = 29;
constexpr std::size_t MAX_USER_NAME_LENGTH = 16;
constexpr int32_t DEFAULT_USER_ID           = 1000;

constexpr bool IsConfiguredUserIdValid(int32_t user_id) {
	constexpr int32_t USER_ID_EVERYONE = 0xfe;
	constexpr int32_t USER_ID_SYSTEM   = 0xff;
	return user_id >= 0 && user_id != USER_ID_EVERYONE && user_id != USER_ID_SYSTEM;
}

struct ConfigOptions {
	uint32_t               screen_width                = 1280;
	uint32_t               screen_height               = 720;
	std::string            user_name                   = "Nova5";
	std::string            audio_input_device;
	std::optional<ControllerColor> controller_color;
	uint32_t               controller_speaker_volume = 50;
	uint32_t               controller_vibration_intensity = 100;
	uint32_t               keyboard_player = 0;
	bool                   hide_cursor_enabled = false;
	bool                   splash_enabled = true;
	bool                   network_connected = false;
	bool                   perf_overlay_enabled = false;
	bool                   skip_notice_screen = false;
	// The unified core already persists shader/pipeline caches by default.
	bool                   shader_cache_enabled = true;
	bool                   readback_tiled_images = false;
	bool                   sync_raw_image_buffers = false;
	bool                   trophy_enabled = true;
	uint32_t               trophy_duration_seconds = 6;
	std::vector<std::string> guest_arguments {""};
	int32_t                user_id                     = DEFAULT_USER_ID;
	PresentMode            present_mode                = PresentMode::Fifo;
	int32_t                gpu_index                   = -1;
	bool                   fullscreen_enabled          = false;
	bool                   hdr_enabled                 = false;
	uint32_t               vblank_frequency            = 60;
	uint32_t               console_language            = DEFAULT_CONSOLE_LANGUAGE;
	bool                   vulkan_validation_enabled   = false;
	bool                   shader_validation_enabled   = false;
	ShaderOptimizationType shader_optimization_type    = ShaderOptimizationType::None;
	ShaderLogDirection     shader_log_direction        = ShaderLogDirection::Silent;
	std::filesystem::path  shader_log_folder           = "_Shaders";
	bool                   command_buffer_dump_enabled = false;
	std::filesystem::path  command_buffer_dump_folder  = "_Buffers";
	bool                   graphics_debug_dump_enabled = false;
	OutputDirection        printf_direction            = OutputDirection::Silent;
	std::filesystem::path  printf_output_file          = "_nova5.txt";
	ProfilerDirection      profiler_direction          = ProfilerDirection::None;
	bool                   spirv_debug_printf_enabled  = false;
	bool                   gpu_assisted_validation_enabled = false;
	bool                   renderdoc_enabled           = false;
	bool                   readback_linear_images      = false;
	bool                   playgo_hack_enabled         = false;
	GuestCpuMode           guest_cpu_mode              = GuestCpuMode::Auto;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	bool red_zone_protection_enabled = false;
#endif
	Keymap keymap;
};

void Load(const ConfigOptions& cfg);

uint32_t GetScreenWidth();
uint32_t GetScreenHeight();
const std::string& GetUserName();
int32_t  GetUserId();
PresentMode GetPresentMode();
int32_t GetGpuIndex();
bool     FullscreenEnabled();
bool     HdrEnabled();
uint32_t GetVblankFrequency();
uint32_t GetConsoleLanguage();
bool     VulkanValidationEnabled();

bool                   ShaderValidationEnabled();
ShaderOptimizationType GetShaderOptimizationType();
ShaderLogDirection     GetShaderLogDirection();
std::filesystem::path  GetShaderLogFolder();

bool                  CommandBufferDumpEnabled();
std::filesystem::path GetCommandBufferDumpFolder();

bool GraphicsDebugDumpEnabled();
bool SyncValidationEnabled();
bool VulkanObjectNamesEnabled();

OutputDirection       GetPrintfDirection();
std::filesystem::path GetPrintfOutputFile();

ProfilerDirection GetProfilerDirection();

bool SpirvDebugPrintfEnabled();

bool GpuAssistedValidationEnabled();

bool RenderDocEnabled();
bool ReadbackLinearImagesEnabled();
bool PlayGoHackEnabled();
GuestCpuMode GetGuestCpuMode();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled();
#endif

const Keymap& GetKeymap();
const std::string& GetAudioInputDevice();
const std::optional<ControllerColor>& GetControllerColor();
uint32_t GetControllerSpeakerVolume();
uint32_t GetControllerVibrationIntensity();
uint32_t GetKeyboardPlayer();
bool HideCursorEnabled();
bool SplashEnabled();
bool IsNetworkConnected();
bool PerfOverlayEnabled();
bool SkipNoticeScreen();
bool ShaderCacheEnabled();
bool ReadbackTiledImagesEnabled();
bool SyncRawImageBuffersEnabled();
bool TrophyEnabled();
uint32_t GetTrophyDurationSeconds();
const std::vector<std::string>& GetGuestArguments();

} // namespace Config

#endif /* KYTY_COMMON_EMULATOR_CONFIG_H_ */
