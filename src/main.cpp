#include "common/archive.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/dateTime.h"
#include "common/debug.h"
#include "common/file.h"
#include "common/magicEnum.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "emulator.h"
#include "kytyGitVersion.h"
#include "local-platform.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>

using namespace Common;
using namespace Emulator;

static std::string GetBuildString() {
	Date date = Date::FromMacros(std::string(__DATE__));

#if KYTY_BUILD == KYTY_BUILD_DEBUG
	std::string type = "Debug";
#elif KYTY_BUILD == KYTY_BUILD_RELEASE
	std::string type = "Release";
#else
	std::string type = "????";
#endif
#if defined(KYTY_STEAM_DECK)
	type += " (Steam Deck)";
#endif

	std::string compiler =
	    Debug::GetCompiler() + "-" + Debug::GetLinker() + "-" + Debug::GetBitness();

	std::string str =
	    fmt::format("{}, {}, ver = {}, git = {}, date = {}", type.c_str(), compiler.c_str(),
	                KYTY_VERSION, KYTY_GIT_VERSION, date.ToString().c_str());

	return str;
}

static void PrintUsage() {
	::printf("%s\n", GetBuildString().c_str());
	::printf("Nova5Emulator --game <dir|elf|zar> [options]\n\n");
	::printf("Options:\n");
	::printf("  --game <dir|elf|zar>                 Game directory, ELF, or ZArchive (.zar) to load.\n");
	::printf("  --game-patch <json>                  ETAHen cheat file.\n");
	::printf("  --screen-width <num>                 Window width. Default: 1280.\n");
	::printf("  --screen-height <num>                Window height. Default: 720.\n");
	::printf(
	    "  --user-name <name>                   Local user name (1-16 bytes). Default: Nova5.\n");
	::printf("  --user-id <num>                      Local user ID. Default: %d.\n",
	         Config::DEFAULT_USER_ID);
	::printf(
	    "  --present-mode <value>               Fifo, Mailbox, or Immediate. Default: Fifo.\n");
	::printf(
	    "  --gpu <index>                        Vulkan physical device index. Default: auto.\n");
	::printf("  --fullscreen                         Run in borderless desktop fullscreen.\n");
	::printf("  --hdr                                Enable HDR10 output when supported.\n");
	::printf("  --vblank-frequency <num>             Virtual vblank frequency. Default: 60.\n");
	::printf("  --console-language <0-29>            Console language. Default: 1 (English US).\n");
	::printf("  --vulkan-validation <true|false>     Enable Vulkan validation.\n");
	::printf("  --gpu-assisted-validation <t|f>      Bounds-check shader accesses on the GPU.\n"
	         "                                       Implies --vulkan-validation; very slow.\n");
	::printf("  --shader-validation <true|false>     Enable shader validation.\n");
	::printf("  --shader-optimization-type <value>   None, Size, or Performance.\n");
	::printf("  --shader-log-direction <value>       Silent, Console, or File.\n");
	::printf("  --shader-log-folder <path>           Shader log output folder.\n");
	::printf("  --command-buffer-dump <true|false>   Enable command buffer dumps.\n");
	::printf("  --command-buffer-dump-folder <path>  Command buffer dump folder.\n");
	::printf("  --graphics-debug-dump <true|false>   Enable graphics debug dumps.\n");
	::printf("  --printf-direction <value>           Silent, Console, or File.\n");
	::printf("  --printf-output-file <path>          Guest printf output file.\n");
	::printf("  --profiler-direction <value>         None or Network.\n");
	::printf("  --spirv-debug-printf <true|false>    Enable SPIR-V debug printf.\n");
	::printf(
	    "  --readback-linear-images <true|false> Read back writable linear images on submit.\n");
	::printf("  --playgo-hack                       Use the supplied PlayGo stub fallback.\n");
	::printf("  --amd-cpu                           Missing CPU extensions + precise VRSQRTPS.\n");
	::printf("  --no-amd-cpu                        Disable load-time CPU fixes (A/B only).\n");
	::printf("  Default: fix missing SSE4a/RDPID/CLWB per CPUID; keep native VRSQRTPS.\n");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	::printf("  --redzone                            Protect the guest SysV red zone.\n");
#endif
	::printf("  --keymap <Control=Input>             DualSense mapping; may be repeated.\n");
	::printf("  --rd                                 Enable RenderDoc capture.\n");
	::printf("  --addcont <LABEL[=DIR]>              Additional content; repeatable.\n"
	         "  --addcont-dir <dir>                  Scan additional content for this title.\n"
	         "  --guest-exec <path> [args...]         Guest executable; remaining arguments go to guest.\n"
	         "  --controller-color <#RRGGBB>         Override controller LED.\n"
	         "  --controller-volume <0-100>          Controller speaker volume; default 50.\n"
	         "  --controller-vibration <0-100>       Vibration intensity; default 100.\n"
	         "  --keyboard-player <0>                Share player 1 (players 1-4 pending multiplayer port).\n"
	         "  --mic <device name>                  Capture microphone; omitted: silence.\n"
	         "  --hide-cursor                        Hide the cursor over the window.\n"
	         "  --no-splash                          Disable startup progress window.\n"
	         "  --network-connected                 Report host network connectivity (not PSN).\n"
	         "  --profile                           Enable Tracy network profiling.\n"
	         "  --perf-overlay                      Performance HUD; toggle with F2, cycle with F8.\n"
	         "  --shader-cache                      Persistent shaders (already on in unified core).\n"
	         "  --readback-tiled-images              Read back small writable tiled images.\n"
	         "  --sync-raw-image-buffers              Synchronize raw buffer reads with image writes.\n"
	         "  --skip-notice-screen                 Guest notice-screen skip flag.\n"
	         "  --no-trophy-notifications            Disable trophy notifications.\n"
	         "  --trophy-duration <seconds>          Trophy toast duration; clamped to 5-10.\n"
	         "  --vr, --tessellation, --occlusion-queries: subsystem ports pending; rejected.\n"
	         "  Boolean options accept either a bare flag or an explicit true/false.\n");
}

static bool NextArg(int argc, char* argv[], int& index, std::string& out) {
	if (index + 1 >= argc) {
		return false;
	}

	index++;
	out = argv[index];
	return true;
}

static bool ParseBool(const std::string& value, bool& out) {
	if (Common::EqualNoCase(value, "true") || value == "1" || Common::EqualNoCase(value, "yes") ||
	    Common::EqualNoCase(value, "on")) {
		out = true;
		return true;
	}

	if (Common::EqualNoCase(value, "false") || value == "0" || Common::EqualNoCase(value, "no") ||
	    Common::EqualNoCase(value, "off")) {
		out = false;
		return true;
	}

	return false;
}

template <typename E>
static bool ParseEnum(const std::string& value, E& out) {
	auto enum_value = magic_enum::enum_cast<E>(value.c_str());
	if (!enum_value.has_value()) {
		return false;
	}

	out = enum_value.value();
	return true;
}

static bool ParseConsoleLanguage(const std::string& value, uint32_t& out) {
	uint32_t language = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), language);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    language > Config::MAX_CONSOLE_LANGUAGE) {
		return false;
	}
	out = language;
	return true;
}

static bool ParseUserId(const std::string& value, int32_t& out) {
	int32_t user_id   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), user_id);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    !Config::IsConfiguredUserIdValid(user_id)) {
		return false;
	}
	out = user_id;
	return true;
}

static bool ParseArgs(int argc, char* argv[], RunOptions& options, bool& show_help) {
	show_help = false;

	for (int i = 1; i < argc; i++) {
		std::string arg = std::string(argv[i]);
		std::string value;

		if (arg == "--help" || arg == "-h") {
			show_help = true;
			continue;
		}

		// Nova5's launcher emits bare flags; run-windows also supports explicit booleans.
		struct Flag { const char* name; bool Config::ConfigOptions::*field; bool value; };
		static constexpr Flag flags[] {
		    {"--hide-cursor", &Config::ConfigOptions::hide_cursor_enabled, true},
		    {"--no-splash", &Config::ConfigOptions::splash_enabled, false},
		    {"--network-connected", &Config::ConfigOptions::network_connected, true},
		    {"--perf-overlay", &Config::ConfigOptions::perf_overlay_enabled, true},
		    {"--shader-cache", &Config::ConfigOptions::shader_cache_enabled, true},
		    {"--skip-notice-screen", &Config::ConfigOptions::skip_notice_screen, true},
		    {"--readback-linear-images", &Config::ConfigOptions::readback_linear_images, true},
		    {"--readback-tiled-images", &Config::ConfigOptions::readback_tiled_images, true},
		    {"--sync-raw-image-buffers", &Config::ConfigOptions::sync_raw_image_buffers, true},
		    {"--no-trophy-notifications", &Config::ConfigOptions::trophy_enabled, false},
		    {"--vulkan-validation", &Config::ConfigOptions::vulkan_validation_enabled, true},
		    {"--gpu-assisted-validation", &Config::ConfigOptions::gpu_assisted_validation_enabled, true},
		    {"--shader-validation", &Config::ConfigOptions::shader_validation_enabled, true},
		    {"--command-buffer-dump", &Config::ConfigOptions::command_buffer_dump_enabled, true},
		    {"--graphics-debug-dump", &Config::ConfigOptions::graphics_debug_dump_enabled, true},
		    {"--spirv-debug-printf", &Config::ConfigOptions::spirv_debug_printf_enabled, true},
		};
		bool handled = false;
		for (const auto& flag: flags) {
			if (arg != flag.name) continue;
			bool enabled = true;
			if (i + 1 < argc && ParseBool(argv[i + 1], enabled)) ++i;
			options.config.*(flag.field) = enabled ? flag.value : !flag.value;
			handled = true;
			break;
		}
		if (handled) continue;
		if (arg == "--profile") {
			options.config.profiler_direction = Config::ProfilerDirection::Network;
			continue;
		}
		// Nova5.ini can hold these on from the previous engine: the game still starts, without them (said in the log).
		if (arg == "--tessellation" || arg == "--occlusion-queries") {
			::printf("Nova5: %s is not available in this engine yet; the game runs without it.\n", arg.c_str());
			continue;
		}
		if (arg == "--vr") {
			::printf("Nova5 unified: %s requires a subsystem not yet ported; refusing to ignore it.\n", arg.c_str());
			return false;
		}

		if (arg == "--rd") {
			options.config.renderdoc_enabled = true;
			continue;
		}

		if (arg == "--fullscreen") {
			options.config.fullscreen_enabled = true;
			continue;
		}

		if (arg == "--hdr") {
			options.config.hdr_enabled = true;
			continue;
		}

		if (arg == "--playgo-hack") {
			options.config.playgo_hack_enabled = true;
			continue;
		}
		if (arg == "--amd-cpu" || arg == "--no-amd-cpu") {
			options.config.guest_cpu_mode = arg == "--amd-cpu" ? Config::GuestCpuMode::Full : Config::GuestCpuMode::Off;
			continue;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (arg == "--redzone") {
			options.config.red_zone_protection_enabled = true;
			continue;
		}
#endif

		if (!Common::StartsWith(arg, "--")) {
			::printf("game input must be provided with --game\n");
			return false;
		}

		if (!NextArg(argc, argv, i, value)) {
			::printf("missing value for %s\n", arg.c_str());
			return false;
		}

		if (arg == "--guest-exec") {
			options.elf = Common::PathFromUtf8(Common::FixFilenameSlash(value));
			options.config.guest_arguments.clear();
			while (++i < argc) options.config.guest_arguments.emplace_back(argv[i]);
			break;
		} else if (arg == "--addcont-dir") {
			if (!options.addcont_dir.empty()) return false;
			options.addcont_dir = Common::PathFromUtf8(Common::FixFilenameSlash(value));
		} else if (arg == "--addcont") {
			auto entry = Common::ParseAddContent(value);
			if (!entry) { ::printf("invalid additional content: %s\n", value.c_str()); return false; }
			options.addcont.push_back(std::move(*entry));
		} else if (arg == "--mic") {
			options.config.audio_input_device = value;
		} else if (arg == "--controller-color") {
			uint32_t rgb = 0;
			if (value.size() != 7 || value[0] != '#') return false;
			auto [end, error] = std::from_chars(value.data() + 1, value.data() + 7, rgb, 16);
			if (error != std::errc{} || end != value.data() + 7) return false;
			options.config.controller_color = Config::ControllerColor {uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb)};
		} else if (arg == "--controller-volume" || arg == "--controller-vibration" ||
		           arg == "--keyboard-player" || arg == "--trophy-duration") {
			uint32_t number = 0;
			auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
			const auto max = arg == "--keyboard-player" ? 4u : arg == "--trophy-duration" ? UINT32_MAX : 100u;
			if (error != std::errc{} || end != value.data() + value.size() || number > max) {
				::printf("invalid value for %s: %s\n", arg.c_str(), value.c_str()); return false;
			}
			if (arg == "--controller-volume") options.config.controller_speaker_volume = number;
			if (arg == "--controller-vibration") options.config.controller_vibration_intensity = number;
			if (arg == "--keyboard-player") options.config.keyboard_player = number;
			if (arg == "--keyboard-player" && number != 0) {
				::printf("Nova5 unified: dedicated keyboard players require the multiplayer port; use 0 to share player 1.\n");
				return false;
			}
			if (arg == "--trophy-duration") options.config.trophy_duration_seconds = number;
		} else if (arg == "--game") {
			if (!options.app0_dir.empty()) {
				::printf("--game can only be specified once\n");
				return false;
			}

			value = Common::FixFilenameSlash(value);
			if (Common::File::IsDirectoryExisting(value)) {
				options.app0_dir = value;
				options.elf      = "/app0/eboot.bin";
			} else if (Common::IsSupportedArchive(value) && Common::File::IsFileExisting(value)) {
				// A game folder packed into a ZArchive: mounted read-only, read without extracting it.
				const auto root = Common::MakeArchivePath(value);
				if (!Common::File::IsFileExisting(root / "eboot.bin")) {
					::printf("Not a game archive (no eboot.bin at its root, or not a ZArchive): %s\n", value.c_str());
					return false;
				}
				options.app0_dir = root;
				options.elf      = "/app0/eboot.bin";
			} else if (Common::File::IsFileExisting(value)) {
				options.app0_dir = Common::DirectoryWithoutFilename(value);
				if (options.app0_dir.empty()) {
					options.app0_dir = ".";
				}
				options.elf = "/app0/" + Common::FilenameWithoutDirectory(value);
			} else {
				::printf("--game must point to an existing directory, ELF, or .zar: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--game-patch") {
			if (!options.game_patch.empty()) {
				::printf("--game-patch can only be specified once\n");
				return false;
			}
			value = Common::FixFilenameSlash(value);
			if (!Common::File::IsFileExisting(value)) {
				::printf("--game-patch must point to an existing file: %s\n", value.c_str());
				return false;
			}
			options.game_patch = value;
		} else if (arg == "--screen-width") {
			options.config.screen_width = static_cast<uint32_t>(Common::ToInt32(value));
		} else if (arg == "--screen-height") {
			options.config.screen_height = static_cast<uint32_t>(Common::ToInt32(value));
		} else if (arg == "--user-name") {
			if (value.empty() || value.size() > Config::MAX_USER_NAME_LENGTH) {
				::printf("invalid user name: must contain 1-%zu bytes\n",
				         Config::MAX_USER_NAME_LENGTH);
				return false;
			}
			options.config.user_name = value;
		} else if (arg == "--user-id") {
			if (!ParseUserId(value, options.config.user_id)) {
				::printf("invalid user ID: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--present-mode") {
			if (!ParseEnum(value, options.config.present_mode)) {
				::printf("invalid present mode: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--gpu") {
			options.config.gpu_index = Common::ToInt32(value);
		} else if (arg == "--vblank-frequency") {
			const int32_t vblank_frequency = Common::ToInt32(value);
			options.config.vblank_frequency =
			    static_cast<uint32_t>(vblank_frequency < 0 ? 0 : vblank_frequency);
		} else if (arg == "--console-language") {
			if (!ParseConsoleLanguage(value, options.config.console_language)) {
				::printf("invalid console language: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--vulkan-validation") {
			if (!ParseBool(value, options.config.vulkan_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--gpu-assisted-validation") {
			if (!ParseBool(value, options.config.gpu_assisted_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--shader-validation") {
			if (!ParseBool(value, options.config.shader_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--shader-optimization-type") {
			if (!ParseEnum(value, options.config.shader_optimization_type)) {
				::printf("invalid shader optimization type: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-log-direction") {
			if (!ParseEnum(value, options.config.shader_log_direction)) {
				::printf("invalid shader log direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-log-folder") {
			options.config.shader_log_folder = value;
		} else if (arg == "--command-buffer-dump") {
			if (!ParseBool(value, options.config.command_buffer_dump_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--command-buffer-dump-folder") {
			options.config.command_buffer_dump_folder = value;
		} else if (arg == "--graphics-debug-dump") {
			if (!ParseBool(value, options.config.graphics_debug_dump_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--printf-direction") {
			if (!ParseEnum(value, options.config.printf_direction)) {
				::printf("invalid printf direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--printf-output-file") {
			options.config.printf_output_file = value;
		} else if (arg == "--profiler-direction") {
			if (!ParseEnum(value, options.config.profiler_direction)) {
				::printf("invalid profiler direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--spirv-debug-printf") {
			if (!ParseBool(value, options.config.spirv_debug_printf_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--readback-linear-images") {
			if (!ParseBool(value, options.config.readback_linear_images)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--keymap") {
			const auto split = value.find('=');
			if (split == std::string::npos || split == 0 || split + 1 == value.size()) {
				// (Skipped as hostInput.cpp skips a mapping it cannot use: the game still starts.)
				::printf("Input mapping skipped: %s (not <control>=<key>)\n", value.c_str());
			} else {
				options.config.keymap.push_back(value);
			}
		} else {
			::printf("unknown option: %s\n", arg.c_str());
			return false;
		}
	}

	if (options.config.gpu_assisted_validation_enabled) {
		options.config.vulkan_validation_enabled = true;
	}

	return show_help || (!options.app0_dir.empty() && !options.elf.empty());
}

#ifndef NOVA5_ARGS_TEST
static int Main(int argc, char* argv[]) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	LocalPlatform::ReleaseRedirectedConsole();
	LocalPlatform::AsyncStdout();
	Common::SetExitDrain(LocalPlatform::DrainStdout);
	// (Registered first, run last: the game's window closes with quick_exit.)
	std::atexit(LocalPlatform::DrainStdout);
	std::at_quick_exit(LocalPlatform::DrainStdout);
#endif
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	// AMD on Linux (RADV): the transfer-only queue family, which RADV shows only when asked to, carries the readbacks of
	// GPU work already done (KYTY_READBACK_QUEUE); without it each waited behind the whole graphics queue. Walking at 1-1
	// on an RX 9070 XT: 44.2 -> 56.3 fps and the 0.1% lows 20 -> 27-29 (dreamerk24m); an RX 9060 XT +19% at the lowest
	// preset (the2ndday; issue #80). Asked for before the driver loads (Mesa 26 reads RADV_EXPERIMENTAL, older Mesa
	// RADV_PERFTEST; other drivers neither), unless the variable is set already. KYTY_RADV_TRANSFER_QUEUE=0: not asked.
	if (const char* ask = std::getenv("KYTY_RADV_TRANSFER_QUEUE"); ask == nullptr || ask[0] != '0') {
		setenv("RADV_EXPERIMENTAL", "transfer_queue", 0);
		setenv("RADV_PERFTEST", "transfer_queue", 0);
	}
#endif
	VirtualMemory::Init();
	InitializeThreads();
	// KYTY_ALLOC_TRACE=<sizes> (diagnostics, e.g. "112,64,16-256" or "1048576-33554432;2888": ',' or ';' between, sizes
	// of 1 MiB and more as up to 16 ranges): the stacks of heap blocks of those sizes from the start (live `alloctrace
	// dump <path>` writes the live ones); the live `alloctrace start` misses what loading made.
	if (const char* sizes = std::getenv("KYTY_ALLOC_TRACE"); sizes != nullptr && *sizes != '\0')
		std::printf("Allocation trace from start-up (%s): %s\n", sizes,
		            LocalPlatform::StartAllocTrace(sizes) ? "on" : "unavailable");

	RunOptions options;
	bool       show_help = false;

	if (argc < 2) {
		PrintUsage();
		return 0;
	}

	if (!ParseArgs(argc, argv, options, show_help)) {
		PrintUsage();
		return 1;
	}

	if (show_help) {
		PrintUsage();
		return 0;
	}

	LocalPlatform::StartHeapTrimmer();
	Run(options);

	return 0;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
int wmain(int argc, wchar_t* argv[]) {
	std::vector<std::string> arguments;
	arguments.reserve(argc);
	for (int i = 0; i < argc; ++i) {
		const std::wstring_view wide(argv[i]);
		arguments.push_back(Common::Utf16ToUtf8(std::u16string(wide.begin(), wide.end())));
	}
	std::vector<char*> pointers;
	for (auto& argument: arguments) pointers.push_back(argument.data());
	return Main(argc, pointers.data());
}
#else
int main(int argc, char* argv[]) { return Main(argc, argv); }
#endif
#endif // NOVA5_ARGS_TEST
