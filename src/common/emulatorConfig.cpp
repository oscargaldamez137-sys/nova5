#include "common/emulatorConfig.h"

#include "common/assert.h"

#include <algorithm>
#include <cstdlib>
#include <memory>

namespace Config {

static std::unique_ptr<ConfigOptions> g_config;

void Initialize() {
	EXIT_IF(g_config != nullptr);

	g_config = std::make_unique<ConfigOptions>();
}

void Shutdown() {
	g_config.reset();
}

void Load(const ConfigOptions& cfg) {
	EXIT_IF(g_config == nullptr);
	EXIT_IF(cfg.user_name.empty() || cfg.user_name.size() > MAX_USER_NAME_LENGTH);
	EXIT_IF(!IsConfiguredUserIdValid(cfg.user_id));
	EXIT_IF(cfg.controller_speaker_volume > 100 || cfg.controller_vibration_intensity > 100);
	EXIT_IF(cfg.keyboard_player != 0); // Dedicated players need the multiplayer subsystem.

	*g_config = cfg;
}

uint32_t GetScreenWidth() {
	return g_config->screen_width;
}

uint32_t GetScreenHeight() {
	return g_config->screen_height;
}

const std::string& GetUserName() {
	return g_config->user_name;
}

int32_t GetUserId() {
	return g_config->user_id;
}

bool IsGuestUserId(int32_t user_id) {
	// Nova5's four player IDs; match the save-data guest mapping too.
	const int64_t delta = static_cast<int64_t>(user_id) - GetUserId();
	return delta > 0 && delta <= 3 * 0x100 && delta % 0x100 == 0;
}

PresentMode GetPresentMode() {
	return g_config->present_mode;
}

int32_t GetGpuIndex() {
	return g_config->gpu_index;
}

bool FullscreenEnabled() {
	return g_config->fullscreen_enabled;
}

bool HdrEnabled() {
	return g_config->hdr_enabled;
}

uint32_t GetVblankFrequency() {
	return std::clamp(g_config->vblank_frequency, 30u, 360u);
}

uint32_t GetConsoleLanguage() {
	return g_config->console_language;
}

bool VulkanValidationEnabled() {
	return g_config->vulkan_validation_enabled;
}

bool ShaderValidationEnabled() {
	return g_config->shader_validation_enabled;
}

ShaderOptimizationType GetShaderOptimizationType() {
	return g_config->shader_optimization_type;
}

ShaderLogDirection GetShaderLogDirection() {
	return g_config->shader_log_direction;
}

std::filesystem::path GetShaderLogFolder() {
	return g_config->shader_log_folder;
}

bool CommandBufferDumpEnabled() {
	return g_config->command_buffer_dump_enabled;
}

std::filesystem::path GetCommandBufferDumpFolder() {
	return g_config->command_buffer_dump_folder;
}

bool GraphicsDebugDumpEnabled() {
	return g_config->graphics_debug_dump_enabled;
}

// KYTY_VULKAN_SYNCVAL=1 (diagnostic): the validation layer with only its synchronization checks, errors logged, not
// fatal (a missing barrier between commands of one queue: hazards on the buffers and images the layer sees, not on
// device-address accesses).
bool SyncValidationEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_VULKAN_SYNCVAL");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

// Vulkan object names (guest ranges of buffers, shader hashes): for a graphics dump and for synchronization validation.
bool VulkanObjectNamesEnabled() {
	return g_config->graphics_debug_dump_enabled || SyncValidationEnabled();
}

OutputDirection GetPrintfDirection() {
	return g_config->printf_direction;
}

std::filesystem::path GetPrintfOutputFile() {
	return g_config->printf_output_file;
}

ProfilerDirection GetProfilerDirection() {
	return g_config->profiler_direction;
}

bool SpirvDebugPrintfEnabled() {
	return g_config->spirv_debug_printf_enabled;
}

bool GpuAssistedValidationEnabled() {
	return g_config->gpu_assisted_validation_enabled && g_config->vulkan_validation_enabled;
}

bool RenderDocEnabled() {
	return g_config->renderdoc_enabled;
}

bool ReadbackLinearImagesEnabled() {
	return g_config->readback_linear_images;
}

bool PlayGoHackEnabled() {
	return g_config->playgo_hack_enabled;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled() {
	return g_config->red_zone_protection_enabled;
}
#endif

GuestCpuMode GetGuestCpuMode() { return g_config->guest_cpu_mode; }

const Keymap& GetKeymap() {
	return g_config->keymap;
}

const std::string& GetAudioInputDevice() { return g_config->audio_input_device; }
const std::optional<ControllerColor>& GetControllerColor() { return g_config->controller_color; }
uint32_t GetControllerSpeakerVolume() { return g_config->controller_speaker_volume; }
uint32_t GetControllerVibrationIntensity() { return g_config->controller_vibration_intensity; }
uint32_t GetKeyboardPlayer() { return g_config->keyboard_player; }
bool HideCursorEnabled() { return g_config->hide_cursor_enabled; }
bool SplashEnabled() { return g_config->splash_enabled; }
bool IsNetworkConnected() { return g_config->network_connected; }
bool PerfOverlayEnabled() { return g_config->perf_overlay_enabled; }
bool SkipNoticeScreen() { return g_config->skip_notice_screen; }
bool ShaderCacheEnabled() { return g_config->shader_cache_enabled; }
bool ReadbackTiledImagesEnabled() { return g_config->readback_tiled_images; }
bool SyncRawImageBuffersEnabled() { return g_config->sync_raw_image_buffers; }
bool TrophyEnabled() { return g_config->trophy_enabled; }
uint32_t GetTrophyDurationSeconds() { return std::clamp(g_config->trophy_duration_seconds, 5u, 10u); }
const std::vector<std::string>& GetGuestArguments() { return g_config->guest_arguments; }

} // namespace Config
