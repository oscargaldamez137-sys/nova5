// The actual CLI parser, without startup, GPU initialization or launching a guest.
#define NOVA5_ARGS_TEST
#include "../src/main.cpp"
#include <fstream>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#endif

static int failures = 0;
static void Check(bool ok, const char* what) {
	if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
static bool Parse(std::vector<std::string> args, RunOptions& options) {
	std::vector<char*> pointers;
	for (auto& arg: args) pointers.push_back(arg.data());
	bool help = false;
	return ParseArgs(static_cast<int>(pointers.size()), pointers.data(), options, help);
}
int main() {
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	RunOptions defaults;
	Check(defaults.config.present_mode == Config::PresentMode::Fifo && defaults.config.vblank_frequency == 60 &&
	      defaults.config.guest_cpu_mode == Config::GuestCpuMode::Auto && defaults.config.shader_cache_enabled,
	      "unified performance defaults are preserved");
	Check(defaults.config.printf_output_file == "_nova5.txt" && defaults.config.user_name == "Nova5", "branding defaults");
	RunOptions launcher;
	Check(Parse({"Nova5Emulator", "--help", "--screen-width", "1920", "--screen-height", "1080",
	             "--user-name", "Nova5", "--user-id", "1000", "--controller-volume", "50",
	             "--controller-vibration", "100", "--keyboard-player", "0", "--present-mode", "Mailbox",
	             "--fullscreen", "--trophy-duration", "6", "--shader-cache", "--vblank-frequency", "60",
	             "--console-language", "1", "--shader-optimization-type", "Performance", "--shader-log-direction", "Silent",
	             "--shader-log-folder", "_Shaders", "--command-buffer-dump-folder", "_Buffers",
	             "--printf-direction", "Silent", "--printf-output-file", "_nova5.txt", "--amd-cpu"}, launcher),
	      "launcher default argument sequence");
	Check(launcher.config.present_mode == Config::PresentMode::Mailbox && launcher.config.guest_cpu_mode == Config::GuestCpuMode::Full,
	      "launcher overrides survive");
	RunOptions booleans;
	Check(Parse({"test", "--help", "--vulkan-validation", "--shader-validation", "false", "--gpu-assisted-validation",
	             "--readback-linear-images", "--readback-tiled-images", "--sync-raw-image-buffers", "--hide-cursor",
	             "--no-splash", "--perf-overlay", "--profile", "--network-connected", "--skip-notice-screen",
	             "--no-trophy-notifications", "--controller-color", "#1234ab", "--mic", "Micrófono 🎤"}, booleans),
	      "bare flags alongside legacy explicit booleans and UTF-8 strings");
	Check(booleans.config.vulkan_validation_enabled && !booleans.config.shader_validation_enabled &&
	      !booleans.config.splash_enabled && !booleans.config.trophy_enabled && booleans.config.readback_tiled_images &&
	      booleans.config.sync_raw_image_buffers && booleans.config.perf_overlay_enabled &&
	      booleans.config.profiler_direction == Config::ProfilerDirection::Network &&
	      booleans.config.controller_color == Config::ControllerColor{0x12, 0x34, 0xab}, "parsed behavior configuration");
	Config::Initialize();
	Config::Load(booleans.config);
	Check(Config::HideCursorEnabled() && Config::SkipNoticeScreen() && Config::IsNetworkConnected() &&
	      Config::ReadbackTiledImagesEnabled() && Config::SyncRawImageBuffersEnabled() && Config::PerfOverlayEnabled() &&
	      !Config::SplashEnabled() && Config::GetAudioInputDevice() == "Micrófono 🎤", "runtime consumers see options");
	for (const auto& pair: {std::pair{0u,5u}, {6u,6u}, {100u,10u}}) {
		booleans.config.trophy_duration_seconds = pair.first;
		Config::Load(booleans.config);
		Check(Config::GetTrophyDurationSeconds() == pair.second, "Nova5 duration clamp");
	}
	Config::Shutdown();
	for (const auto& args: std::vector<std::vector<std::string>>{
	    {"--controller-volume", "101"}, {"--controller-vibration", "-1"}, {"--controller-color", "#zzffff"},
	    {"--keyboard-player", "5"}, {"--keyboard-player", "1"}, {"--trophy-duration", "12x"}, {"--vr"},
	    {"--tessellation"}, {"--occlusion-queries"}, {"--addcont", "INVALID"}, {"--mic"}}) {
		std::vector<std::string> line {"test", "--help"}; line.insert(line.end(), args.begin(), args.end());
		RunOptions invalid;
		Check(!Parse(line, invalid), "invalid/unsupported options are rejected, not silently ignored");
	}
	RunOptions guest;
	Check(Parse({"test", "--help", "--guest-exec", "/app0/子.elf", "program", "--shader-cache", "argument"}, guest) &&
	      guest.config.guest_arguments == std::vector<std::string>{"program", "--shader-cache", "argument"} &&
	      Common::PathToGenericString(guest.elf) == "/app0/子.elf", "guest-exec consumes remaining arguments verbatim");
	RunOptions dlc;
	Check(Parse({"test", "--help", "--addcont", "ABCDEFGHIJKLMNOP", "--addcont", "1234567890ABCDEF"}, dlc) &&
	      dlc.addcont.size() == 2 && dlc.addcont[0].mount_dir.empty(), "repeatable license-only DLC");
	const auto fixture = std::filesystem::current_path() /
	    ("nova5-cli-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::error_code ec;
	if (std::filesystem::create_directory(fixture, ec)) {
		const auto write_param = [&](const char* folder, const char* json) {
			const auto path = fixture / folder / "sce_sys";
			std::filesystem::create_directories(path);
			std::ofstream(path / "param.json") << json;
		};
		write_param("a valid", R"({"contentId":"UP0001-PPSA01341_00-ABCDEFGHIJKLMNOP","titleName":"Add-on"})");
		write_param("b duplicate", R"({"contentId":"UP0001-PPSA01341_00-ABCDEFGHIJKLMNOP"})");
		write_param("c other game", R"({"contentId":"UP0001-PPSA21564_00-1234567890ABCDEF"})");
		write_param("d application", R"({"contentId":"UP0001-PPSA01341_00-1234567890ABCDEF","applicationCategoryType":0})");
		write_param("e malformed", "{");
		const auto entries = Common::FindAddContent(fixture, "PPSA01341");
		Check(entries.size() == 1 && entries[0].entitlement_label == "ABCDEFGHIJKLMNOP" &&
		      entries[0].mount_dir == fixture / "a valid", "DLC scan filters title/apps/malformed data and deduplicates labels");
		const auto mounted = Common::ParseAddContent("ABCDEFGHIJKLMNOP=" + Common::PathToString(fixture / "a valid"));
		Check(mounted && mounted->mount_dir == fixture / "a valid", "DLC explicit directory with spaces");
		Check(!Common::ParseAddContent("ABCDEFGHIJKLMNOP=" + Common::PathToString(fixture / "missing")), "missing DLC directory rejected");
		// This directory was created exclusively by this harness above; never remove existing data.
		std::filesystem::remove_all(fixture, ec);
		Check(!ec, "test fixture cleanup");
	} else {
		Check(false, "create private DLC fixture");
	}
	std::printf("Nova5 CLI tests: %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures != 0;
}
