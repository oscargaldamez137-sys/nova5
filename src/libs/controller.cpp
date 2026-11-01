#include "libs/controller.h"

#include "SDL.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/padData.h"
#include "local/anti-lag.h"
#include "local/input-trace.h"
#include "loader/demonsSoulsWarp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace Libs::Controller {

LIB_NAME("Pad", "Pad");

constexpr int PAD_ERROR_INVALID_ARG    = -2137915391; /* 0x80920001 */
constexpr int PAD_ERROR_INVALID_HANDLE = -2137915389; /* 0x80920003 */

constexpr uint32_t RUMBLE_DURATION_MS = 0xffff;
constexpr uint32_t RELEASE_FLUSH_MS   = 50;
constexpr uint64_t DROP_REPORT_MS     = 5000;

// The newest unread input state takes an event that changes no button and no touch (GameController::AddState).
// KYTY_PAD_STATE_COALESCE=0: every input event a state of its own, as before.
static bool StateCoalescing() {
	static const bool on = [] {
		const char* value = std::getenv("KYTY_PAD_STATE_COALESCE");
		return value == nullptr || std::string_view(value) != "0";
	}();
	return on;
}

struct PadControllerInformation {
	float    touch_pixel_density;
	uint16_t touch_resolution_x;
	uint16_t touch_resolution_y;
	uint8_t  stick_dead_zone_left;
	uint8_t  stick_dead_zone_right;
	uint8_t  connection_type;
	uint8_t  connected_count;
	bool     connected;
	int      device_class;
	uint8_t  reserve[8];
};

struct PadLightBarParam {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t reserve;
};

struct PadVibrationParam {
	uint8_t large_motor;
	uint8_t small_motor;
};

struct PadTriggerEffectCommand {
	uint32_t mode;
	uint8_t  reserve[4];
	uint8_t  data[48];
};

struct PadTriggerEffectParam {
	uint8_t                 trigger_mask;
	uint8_t                 reserve[7];
	PadTriggerEffectCommand command[2];
};

static_assert(sizeof(PadTriggerEffectCommand) == 56);
static_assert(sizeof(PadTriggerEffectParam) == 120);

struct DualSenseEffects {
	uint8_t enable_bits;
	uint8_t reserve[9];
	uint8_t right_trigger[11];
	uint8_t left_trigger[11];
};

static_assert(sizeof(DualSenseEffects) == 32);

struct ControllerState {
	struct Touch {
		uint8_t  id   = 0;
		bool     down = false;
		uint16_t x    = 0;
		uint16_t y    = 0;
	};

	uint64_t time                                  = 0;
	uint32_t buttons                               = 0;
	int      axes[static_cast<int>(Axis::AxisMax)] = {128, 128, 128, 128, 0, 0};
	Touch    touch[2];
};

class GameController {
public:
	GameController() = default;

	KYTY_CLASS_NO_COPY(GameController);

	void Connect(int id);
	void Disconnect(int id);
	void Button(int id, uint32_t button, bool down);
	void Axis(int id, Axis axis, int value);
	void RightStick(int id, int x, int y);
	void TouchPad(int id, int finger, bool down, float x, float y);
	void ResetInputState();
	void ReleaseHostPads();
	void GetConnectionInfo(bool* flag, int* count);
	// The active pad's name when it took the rumble, else nullptr (no pad, or the pad refused it).
	const char* SetVibration(uint8_t large_motor, uint8_t small_motor);
	void SetLightBar(uint8_t r, uint8_t g, uint8_t b);
	bool SetTriggerEffect(const PadTriggerEffectParam& param);
	int32_t GetTriggerEffectState(bool left);
	void ReadState(ControllerState* state, bool* flag, int* count);
	int  ReadStates(ControllerState* states, int states_num, bool* flag, int* count);

private:
	static constexpr uint32_t STATES_MAX = 64;

	void CheckActive();
	void AddState();
	void CountDroppedState();
	void ReportDroppedStates();

	Common::Mutex    m_mutex;
	std::vector<int> m_connected_ids;
	int              m_active_id       = -1;
	bool             m_connected       = false;
	int              m_connected_count = 0;
	ControllerState  m_state;
	ControllerState  m_states[STATES_MAX];
	bool             m_obtained[STATES_MAX] {};
	uint32_t         m_states_num    = 0;
	uint32_t         m_first_state   = 0;
	uint8_t          m_next_touch_id = 1;
	// Every effect the game set, whether or not the host pad could reproduce it:
	// scePadGetTriggerEffectState answers from it (m_trigger_sent keeps the sent one only).
	PadTriggerEffectParam m_state_effect {};
	// Input states dropped from the history before a read took them (AddState): in all, since the last report, and when
	// that was (SDL ticks; none yet: m_dropped_reported).
	uint64_t         m_dropped_states     = 0;
	uint64_t         m_dropped_unreported = 0;
	uint64_t         m_dropped_report_ms  = 0;
	bool             m_dropped_reported   = false;
};

static GameController* g_controller = nullptr;

// SetScriptedInput's values; `until` (steady clock ns, 0: none) is written last.
struct ScriptedInput {
	std::atomic<int64_t>                until {0};
	std::array<std::atomic<int32_t>, 4> axes {};
	std::atomic<uint32_t>               buttons {0};
};
static ScriptedInput g_scripted;

static int64_t SteadyNanoseconds() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void pad_fill_data(PadData* data, const ControllerState& state, bool connected,
                          int connected_count) {
	EXIT_IF(data == nullptr);

	std::memset(data, 0, sizeof(*data));

	data->buttons           = state.buttons;
	data->left_stick_x      = state.axes[static_cast<int>(Axis::LeftX)];
	data->left_stick_y      = state.axes[static_cast<int>(Axis::LeftY)];
	data->right_stick_x     = state.axes[static_cast<int>(Axis::RightX)];
	data->right_stick_y     = state.axes[static_cast<int>(Axis::RightY)];
	data->analog_buttons_l2 = state.axes[static_cast<int>(Axis::TriggerLeft)];
	data->analog_buttons_r2 = state.axes[static_cast<int>(Axis::TriggerRight)];
	data->orientation_w     = 1.0f;
	for (const auto& touch: state.touch) {
		if (touch.down) {
			auto& output = data->touch_data.touch[data->touch_data.touch_num++];
			output.x     = touch.x;
			output.y     = touch.y;
			output.id    = touch.id;
		}
	}
	if (const auto until = g_scripted.until.load(std::memory_order_acquire); until != 0 && SteadyNanoseconds() < until) {
		uint8_t* axes[] = {&data->left_stick_x, &data->left_stick_y, &data->right_stick_x, &data->right_stick_y};
		for (size_t i = 0; i < 4; i++) {
			if (const auto value = g_scripted.axes[i].load(std::memory_order_relaxed); value >= 0) {
				*axes[i] = static_cast<uint8_t>(std::min(value, 255));
			}
		}
		data->buttons |= g_scripted.buttons.load(std::memory_order_relaxed);
	}
	data->connected              = connected;
	data->timestamp              = state.time;
	data->connected_count        = static_cast<uint8_t>(std::min(connected_count, 255));
	data->device_unique_data_len = 0;
}

// Log what the guest receives, including a press and release returned in one batch.
// Only the first few reads and changes are printed; disabled unless explicitly requested.
static void TracePadRead(const char* api, const PadData* data, int count) {
	if (!InputTrace::Enabled()) return;
	static std::mutex mutex;
	static uint32_t last_any = 0, last_final = 0;
	static bool last_connected = false;
	static unsigned reads = 0, lines = 0;
	std::lock_guard lock(mutex);
	static uint64_t read_calls = 0, state_calls = 0, next_report = 0;
	(std::strcmp(api, "scePadReadState") == 0 ? state_calls : read_calls)++;
	const auto now = SDL_GetTicks64();
	if (now >= next_report) {
		next_report = now + 5000;
		std::printf("[INPUT] pad-poll ms=%llu scePadRead=%llu scePadReadState=%llu\n",
		            static_cast<unsigned long long>(now), static_cast<unsigned long long>(read_calls),
		            static_cast<unsigned long long>(state_calls));
		std::fflush(stdout);
	}
	uint32_t any = 0;
	for (int i = 0; i < count; ++i) any |= data[i].buttons;
	const auto& final = data[count - 1];
	if ((reads++ < 8 || any != last_any || final.buttons != last_final || final.connected != last_connected) && lines < 256) {
		++lines;
		std::printf("[INPUT] %s count=%d connected=%d buttons-any=0x%x buttons-last=0x%x timestamp=%llu\n",
		            api, count, final.connected, any, final.buttons,
		            static_cast<unsigned long long>(final.timestamp));
		std::fflush(stdout);
	}
	last_any = any;
	last_final = final.buttons;
	last_connected = final.connected;
}

static bool trigger_effect_zones(uint8_t* effect, const uint8_t* strengths, uint8_t type,
                                 uint8_t frequency = 0) {
	uint16_t active = 0;
	uint32_t packed = 0;
	for (int i = 0; i < 10; i++) {
		if (strengths[i] > 8) {
			return false;
		}
		if (strengths[i] != 0) {
			active |= static_cast<uint16_t>(1u << i);
			packed |= static_cast<uint32_t>(strengths[i] - 1u) << (3 * i);
		}
	}

	effect[0] = active != 0 && (type != 0x26 || frequency != 0) ? type : 0x05;
	effect[1] = static_cast<uint8_t>(active);
	effect[2] = static_cast<uint8_t>(active >> 8u);
	effect[3] = static_cast<uint8_t>(packed);
	effect[4] = static_cast<uint8_t>(packed >> 8u);
	effect[5] = static_cast<uint8_t>(packed >> 16u);
	effect[6] = static_cast<uint8_t>(packed >> 24u);
	effect[9] = frequency;
	return true;
}

static bool trigger_effect_to_dualsense(const PadTriggerEffectCommand& command, uint8_t* effect) {
	std::memset(effect, 0, 11);
	effect[0] = 0x05;

	uint8_t strengths[10] = {};
	switch (command.mode) {
		case 0: return true;
		case 1:
			if (command.data[0] > 9 || command.data[1] > 8) {
				return false;
			}
			std::fill(strengths + command.data[0], strengths + 10, command.data[1]);
			return trigger_effect_zones(effect, strengths, 0x21);
		case 2: {
			const auto start    = command.data[0];
			const auto end      = command.data[1];
			const auto strength = command.data[2];
			if (start < 2 || start > 7 || end <= start || end > 8 || strength > 8) {
				return false;
			}
			if (strength == 0) {
				return true;
			}
			const uint16_t zones = static_cast<uint16_t>((1u << start) | (1u << end));
			effect[0]            = 0x25;
			effect[1]            = static_cast<uint8_t>(zones);
			effect[2]            = static_cast<uint8_t>(zones >> 8u);
			effect[3]            = strength - 1u;
			return true;
		}
		case 3:
			if (command.data[0] > 9 || command.data[1] > 8) {
				return false;
			}
			std::fill(strengths + command.data[0], strengths + 10, command.data[1]);
			return trigger_effect_zones(effect, strengths, 0x26, command.data[2]);
		case 4: return trigger_effect_zones(effect, command.data, 0x21);
		case 5: {
			const int start          = command.data[0];
			const int end            = command.data[1];
			const int start_strength = command.data[2];
			const int end_strength   = command.data[3];
			if (start > 8 || end <= start || end > 9 || start_strength < 1 || start_strength > 8 ||
			    end_strength < 1 || end_strength > 8) {
				return false;
			}
			for (int i = start; i < 10; i++) {
				const int delta  = (end_strength - start_strength) * (i - start);
				const int length = end - start;
				strengths[i]     = static_cast<uint8_t>(
				    i >= end ? end_strength
				             : start_strength +
				                   (delta + (delta < 0 ? -length / 2 : length / 2)) / length);
			}
			return trigger_effect_zones(effect, strengths, 0x21);
		}
		case 6: return trigger_effect_zones(effect, command.data + 1, 0x26, command.data[0]);
		default: return false;
	}
}

void Initialize() {
	EXIT_IF(g_controller != nullptr);

	g_controller = new GameController;
	g_controller->Connect(HOST_INPUT_CONTROLLER_ID);
	if (InputTrace::Enabled()) std::printf("[INPUT] virtual keyboard/mouse pad connected; shares player 1\n");
}

void Shutdown() {
	EmergencyShutdown();
	delete g_controller;
	g_controller = nullptr;
}

void EmergencyShutdown() {
	if (g_controller != nullptr) {
		g_controller->ReleaseHostPads();
	}
}

void GameController::Connect(int id) {
	Common::LockGuard lock(m_mutex);

	if (std::find(m_connected_ids.begin(), m_connected_ids.end(), id) != m_connected_ids.end()) {
		return;
	}

	m_connected_ids.push_back(id);

	CheckActive();
}

void GameController::Disconnect(int id) {
	Common::LockGuard lock(m_mutex);

	// SDL2 reports every removed joystick as a removed controller, also one that never was one (a wheel, a virtual or
	// unmapped device: "we don't know if this was a game controller"), and a controller that could not be opened was
	// never connected here (issue #49: the game ended when such a device went away).
	const auto it = std::find(m_connected_ids.begin(), m_connected_ids.end(), id);
	if (it == m_connected_ids.end()) {
		return;
	}

	m_connected_ids.erase(it);

	CheckActive();
}

void GameController::CheckActive() {
	int  new_active_id = -1;
	bool new_connected = false;

	if (!m_connected_ids.empty()) {
		new_active_id = m_connected_ids[0];
		for (const auto id: m_connected_ids) {
			if (id != HOST_INPUT_CONTROLLER_ID) {
				new_active_id = id;
				break;
			}
		}
		new_connected = true;
	}

	if (m_connected == new_connected && m_active_id == new_active_id) {
		return;
	}
	if (!m_connected && new_connected) {
		m_connected_count++;
	}
	m_active_id     = new_active_id;
	if (new_active_id != HOST_INPUT_CONTROLLER_ID && new_active_id != -1) {
		if (const auto& color = Config::GetControllerColor()) {
			if (auto* pad = SDL_GameControllerFromInstanceID(new_active_id))
				SDL_GameControllerSetLED(pad, (*color)[0], (*color)[1], (*color)[2]);
		}
	}
	m_connected     = new_connected;
	m_state         = {};
	m_states_num    = 0;
	m_first_state   = 0;
	m_next_touch_id = 1;
}

// The history scePadRead returns, oldest unread first: a state per input event. Demon's Souls reads 64 each tick and
// takes the presses and releases of every one, the last as the pad's state (its pad update, eboot 0xb933a0), so a press
// is lost only when it leaves the STATES_MAX states unread. An event that changes no button and no touch (a stick or
// trigger step, a finger moving) updates the newest state instead while no read has taken it (StateCoalescing): every
// press and release keeps a state of its own, and a burst of axis events between two reads (a frame hitch: 5-43 of
// 100 ms or more a session in the logs, one of 18.5 s) no longer pushes an unread press out.
void GameController::AddState() {
	if (m_states_num != 0 && StateCoalescing()) {
		const auto  newest = (m_first_state + m_states_num - 1) % STATES_MAX;
		const auto& last   = m_states[newest];
		if (!m_obtained[newest] && last.buttons == m_state.buttons &&
		    std::equal(std::begin(last.touch), std::end(last.touch), std::begin(m_state.touch),
		               [](const ControllerState::Touch& a, const ControllerState::Touch& b) {
			               return a.down == b.down && a.id == b.id;
		               })) {
			m_states[newest] = m_state;
			return;
		}
	}

	if (m_states_num >= STATES_MAX) {
		if (!m_obtained[m_first_state]) {
			CountDroppedState();
		}
		m_states_num  = STATES_MAX - 1;
		m_first_state = (m_first_state + 1) % STATES_MAX;
	}

	const auto index  = (m_first_state + m_states_num) % STATES_MAX;
	m_states[index]   = m_state;
	m_obtained[index] = false;
	m_states_num++;
}

// A state dropped before a read took it: in the log (stdout) at most every DROP_REPORT_MS, the count since the last line.
void GameController::CountDroppedState() {
	m_dropped_states++;
	m_dropped_unreported++;
	if (const auto now = SDL_GetTicks64(); !m_dropped_reported || now - m_dropped_report_ms >= DROP_REPORT_MS) {
		m_dropped_reported  = true;
		m_dropped_report_ms = now;
		ReportDroppedStates();
	}
}

void GameController::ReportDroppedStates() {
	if (m_dropped_unreported == 0) {
		return;
	}
	std::printf("Pad: %llu input states dropped unread (the %u-state history was full; %llu in all)\n",
	            static_cast<unsigned long long>(m_dropped_unreported), STATES_MAX,
	            static_cast<unsigned long long>(m_dropped_states));
	std::fflush(stdout);
	m_dropped_unreported = 0;
}

void GameController::Button(int id, uint32_t button, bool down) {
	Common::LockGuard lock(m_mutex);

	// The keyboard shares the player-1 pad with the active gamepad.
	if (m_active_id == id || id == HOST_INPUT_CONTROLLER_ID) {
		m_state.time = LibKernel::KernelGetProcessTime();

		m_state.buttons = down ? m_state.buttons | button : m_state.buttons & ~button;

		AddState();
	}
}

void GameController::Axis(int id, Controller::Axis axis, int value) {
	Common::LockGuard lock(m_mutex);

	if (m_active_id == id || id == HOST_INPUT_CONTROLLER_ID) {
		m_state.time = LibKernel::KernelGetProcessTime();

		int axis_id = static_cast<int>(axis);

		EXIT_IF(axis_id < 0 || axis_id >= static_cast<int>(Controller::Axis::AxisMax));

		m_state.axes[axis_id] = value;

		uint32_t trigger = 0;
		if (axis == Controller::Axis::TriggerLeft) {
			trigger = PAD_BUTTON_L2;
		} else if (axis == Controller::Axis::TriggerRight) {
			trigger = PAD_BUTTON_R2;
		}
		if (trigger != 0) {
			m_state.buttons = value > 0 ? m_state.buttons | trigger : m_state.buttons & ~trigger;
		}

		AddState();
	}
}

void GameController::RightStick(int id, int x, int y) {
	Common::LockGuard lock(m_mutex);

	if (m_active_id == id || id == HOST_INPUT_CONTROLLER_ID) {
		m_state.time                                 = LibKernel::KernelGetProcessTime();
		m_state.axes[static_cast<int>(Axis::RightX)] = x;
		m_state.axes[static_cast<int>(Axis::RightY)] = y;
		AddState();
	}
}

void GameController::TouchPad(int id, int finger, bool down, float x, float y) {
	if (finger < 0 || finger >= 2) {
		return;
	}

	Common::LockGuard lock(m_mutex);
	if (m_active_id == id || id == HOST_INPUT_CONTROLLER_ID) {
		auto& touch  = m_state.touch[finger];
		m_state.time = LibKernel::KernelGetProcessTime();
		if (down && !touch.down) {
			touch.id        = m_next_touch_id;
			m_next_touch_id = m_next_touch_id == 127 ? 1 : m_next_touch_id + 1;
		}
		touch.down = down;
		touch.x    = static_cast<uint16_t>(std::clamp(x, 0.0f, 1.0f) * 1920.0f);
		touch.y    = static_cast<uint16_t>(std::clamp(y, 0.0f, 1.0f) * 943.0f);
		if (id == HOST_INPUT_CONTROLLER_ID) {
			m_state.buttons = down ? m_state.buttons | PAD_BUTTON_TOUCH_PAD
			                       : m_state.buttons & ~PAD_BUTTON_TOUCH_PAD;
		}
		AddState();
	}
}

void GameController::ResetInputState() {
	Common::LockGuard lock(m_mutex);
	m_state         = {};
	m_state.time    = LibKernel::KernelGetProcessTime();
	m_states_num    = 0;
	m_first_state   = 0;
	m_next_touch_id = 1;
	AddState();
}

void GameController::ReleaseHostPads() {
	Common::LockGuard lock(m_mutex);

	std::vector<SDL_GameController*> pads;
	for (const auto id: m_connected_ids) {
		if (id == HOST_INPUT_CONTROLLER_ID) {
			continue;
		}
		if (auto* pad = SDL_GameControllerFromInstanceID(static_cast<SDL_JoystickID>(id));
		    pad != nullptr) {
			if (SDL_GameControllerGetType(pad) == SDL_CONTROLLER_TYPE_PS5) {
				DualSenseEffects effect {};
				effect.enable_bits     = 0x0c;
				effect.right_trigger[0] = 0x05;
				effect.left_trigger[0]  = 0x05;
				(void)SDL_GameControllerSendEffect(pad, &effect, sizeof(effect));
			}
			(void)SDL_GameControllerRumble(pad, 0, 0, 0);
			(void)SDL_GameControllerSetLED(pad, 0, 0, 0);
			pads.push_back(pad);
		}
	}

	if (!pads.empty()) {
		SDL_Delay(RELEASE_FLUSH_MS);
		for (auto* pad: pads) {
			SDL_GameControllerClose(pad);
		}
	}

	m_connected_ids.clear();
	ReportDroppedStates();
}

const char* GameController::SetVibration(uint8_t large_motor, uint8_t small_motor) {
	Common::LockGuard lock(m_mutex);

	if (m_active_id == HOST_INPUT_CONTROLLER_ID) {
		return nullptr;
	}

	auto* pad = SDL_GameControllerFromInstanceID(static_cast<SDL_JoystickID>(m_active_id));
	if (pad == nullptr) {
		return nullptr;
	}

	const auto intensity = Config::GetControllerVibrationIntensity();
	const auto large = static_cast<uint16_t>(large_motor * 0x101U * intensity / 100u);
	const auto small = static_cast<uint16_t>(small_motor * 0x101U * intensity / 100u);
	if (SDL_GameControllerRumble(pad, large, small, RUMBLE_DURATION_MS) != 0) {
		LOGF("\t rumble failed: %s\n", SDL_GetError());
		return nullptr;
	}
	const char* name = SDL_GameControllerName(pad);
	return name != nullptr ? name : "a pad";
}

void GameController::SetLightBar(uint8_t r, uint8_t g, uint8_t b) {
	Common::LockGuard lock(m_mutex);
	if (const auto& color = Config::GetControllerColor()) {
		r = (*color)[0]; g = (*color)[1]; b = (*color)[2];
	}
	if (auto* pad = SDL_GameControllerFromInstanceID(static_cast<SDL_JoystickID>(m_active_id));
	    pad != nullptr) {
		(void)SDL_GameControllerSetLED(pad, r, g, b);
	}
}

bool GameController::SetTriggerEffect(const PadTriggerEffectParam& param) {
	if ((param.trigger_mask & ~0x03u) != 0) {
		return false;
	}

	DualSenseEffects effect {};
	if ((param.trigger_mask & 0x01u) != 0) {
		effect.enable_bits |= 0x08;
		if (!trigger_effect_to_dualsense(param.command[0], effect.left_trigger)) {
			return false;
		}
	}
	if ((param.trigger_mask & 0x02u) != 0) {
		effect.enable_bits |= 0x04;
		if (!trigger_effect_to_dualsense(param.command[1], effect.right_trigger)) {
			return false;
		}
	}
	if (effect.enable_bits == 0) {
		return true;
	}

	Common::LockGuard lock(m_mutex);
	// Remember it for scePadGetTriggerEffectState even when no DualSense takes it: a game that polls
	// the state has to see the effect it asked for, not a zero.
	m_state_effect = param;
	auto* pad = SDL_GameControllerFromInstanceID(static_cast<SDL_JoystickID>(m_active_id));
	if (pad != nullptr && SDL_GameControllerGetType(pad) == SDL_CONTROLLER_TYPE_PS5) {
		(void)SDL_GameControllerSendEffect(pad, &effect, sizeof(effect));
	}
	return true;
}

// A trigger's travel, 0..255. An L2/R2 that arrives as a button only (input remapping, pads or
// tools that report the trigger digitally) counts as full travel.
static int TriggerTravel(const ControllerState& state, Axis axis) {
	const auto button = axis == Axis::TriggerLeft ? PAD_BUTTON_L2 : PAD_BUTTON_R2;
	const int  value  = state.axes[static_cast<int>(axis)];
	// An analog press also sets the button, so only a button without any travel is digital.
	return (state.buttons & button) != 0 && value == 0 ? 255 : value;
}

// A real DualSense reports which effect runs and where the trigger is against it. Without that
// report (SDL does not expose it, and other pads have no trigger motors) the state follows from the
// game's effect and the trigger's travel: 0..255 maps to the effect's ten positions 0..9. Games wait
// for the weapon or feedback state before acting on L2/R2 - the state stayed 0 before, so those
// actions only ever half happened (a bow's draw, for one).
static int32_t TriggerEffectState(const PadTriggerEffectCommand& command, int value) {
	const int  pos     = std::clamp(value, 0, 255) * 10 / 256;
	const bool pressed = value >= 8; // below this a trigger counts as released (analog rest noise)

	switch (command.mode) {
		case 1: // feedback: resistance from position data[0], strength data[1]
			return pressed && command.data[1] != 0 && pos >= command.data[0]
			           ? PAD_TRIGGER_STATE_IS_PUSHING
			           : PAD_TRIGGER_STATE_NO_FORCE;
		case 2: // weapon: resistance from data[0], released at data[1]
			if (pos >= command.data[1]) {
				return PAD_TRIGGER_STATE_WEAPON_FULLY_PRESSED;
			}
			return pos >= command.data[0] ? PAD_TRIGGER_STATE_WEAPON_ALMOST_PRESSED
			                              : PAD_TRIGGER_STATE_WEAPON_NOT_PRESSED;
		case 3: // vibration from position data[0], amplitude data[1], frequency data[2]
			return pressed && command.data[1] != 0 && command.data[2] != 0 && pos >= command.data[0]
			           ? PAD_TRIGGER_STATE_VIBRATION_IS_FIRING
			           : PAD_TRIGGER_STATE_VIBRATION_NOT_FIRING;
		case 4: // multiple-position feedback: strength per position
			return pressed && command.data[pos] != 0 ? PAD_TRIGGER_STATE_IS_PUSHING
			                                         : PAD_TRIGGER_STATE_NO_FORCE;
		case 5: // slope feedback from position data[0]
			return pressed && pos >= command.data[0] ? PAD_TRIGGER_STATE_IS_PUSHING
			                                         : PAD_TRIGGER_STATE_NO_FORCE;
		case 6: // multiple-position vibration: frequency data[0], amplitude per position
			return pressed && command.data[0] != 0 && command.data[1 + pos] != 0
			           ? PAD_TRIGGER_STATE_VIBRATION_IS_FIRING
			           : PAD_TRIGGER_STATE_VIBRATION_NOT_FIRING;
		default: return 0;
	}
}

int32_t GameController::GetTriggerEffectState(bool left) {
	Common::LockGuard lock(m_mutex);
	const auto        index = left ? 0 : 1;
	if ((m_state_effect.trigger_mask & (1u << index)) == 0) {
		return 0;
	}
	const auto axis = left ? Axis::TriggerLeft : Axis::TriggerRight;
	return TriggerEffectState(m_state_effect.command[index], TriggerTravel(m_state, axis));
}

void GameController::GetConnectionInfo(bool* flag, int* count) {
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);

	Common::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;
}

void GameController::ReadState(ControllerState* state, bool* flag, int* count) {
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);
	EXIT_IF(state == nullptr);

	Common::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;
	*state = m_state;
}

int GameController::ReadStates(ControllerState* states, int states_num, bool* flag, int* count) {
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);
	EXIT_IF(states == nullptr);
	EXIT_IF(states_num < 1 || states_num > STATES_MAX);

	Common::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;

	int ret_num = 0;

	if (m_connected) {
		if (m_states_num != 0) {
			for (uint32_t i = 0; i < m_states_num; i++) {
				if (ret_num >= states_num) {
					break;
				}
				auto index = (m_first_state + i) % STATES_MAX;
				if (!m_obtained[index]) {
					m_obtained[index] = true;

					states[ret_num++] = m_states[index];
				}
			}
		}
	}

	return ret_num;
}

void Connect(int id) {
	g_controller->Connect(id);
}

void Disconnect(int id) {
	g_controller->Disconnect(id);
}

void SetButton(int id, uint32_t button, bool down) {
	g_controller->Button(id, button, down);
}

void SetAxis(int id, Axis axis, int value) {
	g_controller->Axis(id, axis, value);
}

void SetRightStick(int id, int x, int y) {
	g_controller->RightStick(id, x, y);
}

void SetTouchPad(int id, int finger, bool down, float x, float y) {
	g_controller->TouchPad(id, finger, down, x, y);
}

void SetScriptedInput(int left_x, int left_y, int right_x, int right_y, uint32_t buttons, double seconds) {
	g_scripted.until.store(0, std::memory_order_release);
	const int values[] = {left_x, left_y, right_x, right_y};
	for (size_t i = 0; i < 4; i++) g_scripted.axes[i].store(values[i], std::memory_order_relaxed);
	g_scripted.buttons.store(buttons, std::memory_order_relaxed);
	if (seconds > 0) {
		g_scripted.until.store(SteadyNanoseconds() + static_cast<int64_t>(seconds * 1e9), std::memory_order_release);
	}
}

void ResetInputState() {
	g_controller->ResetInputState();
}

int KYTY_SYSV_ABI PadInit() {
	PRINT_NAME();

	return OK;
}

static bool PadOpenArgsAreValid(int user_id, int type, int index) {
	constexpr int user_id_system     = 0xff;
	constexpr int port_type_standard = 0;
	constexpr int port_type_special  = 2;
	constexpr int port_type_remote   = 16;
	const bool    personal_port =
	    user_id == Config::GetUserId() && (type == port_type_standard || type == port_type_special);
	const bool system_remote_control = user_id == user_id_system && type == port_type_remote;
	return index == 0 && (personal_port || system_remote_control);
}

int KYTY_SYSV_ABI PadOpen(int user_id, int type, int index, const void* param) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %d\n"
	     "\t index   = %d\n"
	     "\t param   = 0x%016" PRIx64 "\n",
	     user_id, type, index, reinterpret_cast<uint64_t>(param));

	constexpr int pad_error_invalid_arg = -2137915391; /* 0x80920001 */

	if (!PadOpenArgsAreValid(user_id, type, index)) {
		return pad_error_invalid_arg;
	}

	int handle = 1;

	return handle;
}

int KYTY_SYSV_ABI PadGetHandle(int user_id, int type, int index) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %d\n"
	     "\t index   = %d\n",
	     user_id, type, index);

	constexpr int pad_error_device_no_handle = -2137915384; /* 0x80920008 */

	if (!PadOpenArgsAreValid(user_id, type, index)) {
		return pad_error_device_no_handle;
	}

	return 1;
}

int KYTY_SYSV_ABI PadSetMotionSensorState(int handle, bool enable) {
	PRINT_NAME();

	if (handle != 1) {
		return PAD_ERROR_INVALID_HANDLE;
	}

	LOGF("\t enable = %s\n", (enable ? "true" : "false"));

	return OK;
}

int KYTY_SYSV_ABI PadSetAngularVelocityDeadbandState(int handle, bool enable) {
	PRINT_NAME();

	if (handle != 1) {
		return PAD_ERROR_INVALID_HANDLE;
	}

	LOGF("\t enable = %s\n", (enable ? "true" : "false"));

	return OK;
}

int KYTY_SYSV_ABI PadResetOrientation(int handle) {
	PRINT_NAME();

	if (handle != 1) {
		return PAD_ERROR_INVALID_HANDLE;
	}

	return OK;
}

int KYTY_SYSV_ABI PadGetControllerInformation(int handle, PadControllerInformation* info) {
	PRINT_NAME();

	int  connected_count = 0;
	bool connected       = false;

	g_controller->GetConnectionInfo(&connected, &connected_count);

	if (handle != 1) {
		return PAD_ERROR_INVALID_HANDLE;
	}
	if (info == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	std::memset(info, 0, sizeof(*info));

	info->touch_pixel_density   = 44.86f;
	info->touch_resolution_x    = 1920;
	info->touch_resolution_y    = 943;
	info->stick_dead_zone_left  = controller_get_axis(-32768, 32767, 8000) - 128;
	info->stick_dead_zone_right = controller_get_axis(-32768, 32767, 8000) - 128;
	info->connection_type       = 0;
	info->connected_count       = static_cast<uint8_t>(std::min(connected_count, 255));
	info->connected             = connected;
	info->device_class          = 0;

	return OK;
}

int KYTY_SYSV_ABI PadReadState(int handle, PadData* data) {
	PRINT_NAME();
	Loader::DemonsSoulsWarp::RunQueuedCommand(); // (live `gamecmd`: on the game's own thread)

	if (handle != 1) {
		return PAD_ERROR_INVALID_HANDLE;
	}
	if (data == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	int             connected_count = 0;
	bool            connected       = false;
	ControllerState state;

	Graphics::AntiLag::Input(); // (KYTY_LOW_LATENCY on AMD: a new frame's input; the driver may wait here)
	g_controller->ReadState(&state, &connected, &connected_count);

	pad_fill_data(data, state, connected, connected_count);
	TracePadRead("scePadReadState", data, 1);

	return OK;
}

int KYTY_SYSV_ABI PadRead(int handle, PadData* data, int num) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(num < 1 || num > 64);
	if (handle != 1) {
		return PAD_ERROR_INVALID_HANDLE;
	}
	if (data == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	std::memset(data, 0, sizeof(PadData) * static_cast<size_t>(num));

	int             connected_count = 0;
	bool            connected       = false;
	ControllerState states[64]      = {};

	Graphics::AntiLag::Input(); // (KYTY_LOW_LATENCY on AMD: a new frame's input; the driver may wait here)
	int ret_num = g_controller->ReadStates(states, num, &connected, &connected_count);

	if (!connected || ret_num == 0) {
		if (connected) {
			g_controller->ReadState(&states[0], &connected, &connected_count);
		}
		ret_num = 1;
	}

	for (int i = 0; i < ret_num; i++) {
		pad_fill_data(&data[i], states[i], connected, connected_count);
	}
	TracePadRead("scePadRead", data, ret_num);

	return ret_num;
}

// Demon's Souls sends its vibration only as haptic audio for the DualSense's actuators (the vibration port), never through
// PadSetVibration, so an Xbox controller never vibrated (issue #236). Each channel's peak (left the large motor, right
// the small one) is held with a decay, as the DualSense on Bluetooth's motors are (dualSenseHaptics.cpp
// RumbleFromHaptics: x0.6 per 21.3 ms, so a single hit ends in ~250 ms and hits in consecutive blocks join), and the
// motors are set once per 1024 frames (21.3 ms at 48 kHz). KYTY_PAD_RUMBLE_GAIN=<0..16> scales the drive (1 by
// default: an Xbox pad's motors at the DualSense's reference 2 were too strong even at 1, issue #236; 0 leaves the
// motors alone). KYTY_PAD_RUMBLE_ONSET=<0..1>: the level a block
// needs to drive them (0.025, as the DualSense's: a quiet effect the game keeps playing, an enchanted weapon's hum at
// 0.005-0.01, rumbled the whole buff long, issue #239; hits reach 0.25).
void RumbleFromVibrationAudio(const void* data, uint32_t frames, uint32_t channels, bool is_float, const float* gains) {
	static const float gain = [] {
		const char* value = std::getenv("KYTY_PAD_RUMBLE_GAIN");
#if defined(KYTY_STEAM_DECK)
		constexpr float fallback = 2.0f; // (the Deck's own motors are weak)
#else
		constexpr float fallback = 1.0f;
#endif
		return value != nullptr && *value != 0 ? std::clamp(std::strtof(value, nullptr), 0.0f, 16.0f) : fallback;
	}();
	static const float onset = [] {
		const char* value = std::getenv("KYTY_PAD_RUMBLE_ONSET");
		return value != nullptr && *value != 0 ? std::clamp(std::strtof(value, nullptr), 0.0f, 1.0f) : 0.025f;
	}();
	if (gain == 0.0f || data == nullptr || frames == 0 || channels == 0 || g_controller == nullptr) {
		return;
	}
	constexpr float    DECAY = 0.6f, FLOOR = 0.01f;
	constexpr uint32_t STEP  = 1024;
	static std::mutex  mutex;
	static float       peak[2] {}, held[2] {};
	static uint32_t    pending = 0;
	std::lock_guard    lock(mutex);
	for (uint32_t i = 0; i < frames; i++) {
		for (uint32_t c = 0; c < channels; c++) {
			const size_t at    = static_cast<size_t>(i) * channels + c;
			float        value = is_float ? static_cast<const float*>(data)[at]
			                              : static_cast<float>(static_cast<const int16_t*>(data)[at]) / 32768.0f;
			if (gains != nullptr) value *= gains[c];
			auto& motor = peak[std::min<uint32_t>(c, 1)];
			motor       = std::max(motor, std::fabs(value));
		}
	}
	if (channels == 1) peak[1] = peak[0];
	pending += frames;
	if (pending < STEP) {
		return;
	}
	pending = 0;
	const bool was_live = held[0] != 0.0f || held[1] != 0.0f;
	for (int m = 0; m < 2; m++) {
		held[m] = std::max(peak[m] >= onset ? peak[m] : 0.0f, held[m] * DECAY);
		peak[m] = 0.0f;
	}
	if (held[0] < FLOOR && held[1] < FLOOR) {
		held[0] = held[1] = 0.0f;
		// (Stopped explicitly once: a hit that ends must not leave the motors at its last level.)
		if (was_live) g_controller->SetVibration(0, 0);
		return;
	}
	const auto to_motor = [](float value) { return static_cast<uint8_t>(std::clamp(value * gain, 0.0f, 1.0f) * 255.0f); };
	const char* pad = g_controller->SetVibration(to_motor(held[0]), to_motor(held[1]));
	// (Once a pad took it, and once the first time none did: whether the game's vibration reaches a pad.)
	static bool noted = false, no_pad = false;
	if (pad != nullptr ? !noted : !no_pad) {
		(pad != nullptr ? noted : no_pad) = true;
		std::printf("Pad rumble: the game's vibration %s%s (no DualSense haptics; KYTY_PAD_RUMBLE_GAIN %.1f)\n",
		            pad != nullptr ? "drives the motors of " : "has no pad that takes rumble",
		            pad != nullptr ? pad : "", static_cast<double>(gain));
		std::fflush(stdout);
	}
}

int KYTY_SYSV_ABI PadSetVibration(int handle, const PadVibrationParam* param) {
	PRINT_NAME();

	if (handle != 1) {
		return PAD_ERROR_INVALID_HANDLE;
	}
	if (param == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	LOGF("\t large_motor = %d\n"
	     "\t small_motor = %d\n",
	     static_cast<int>(param->large_motor), static_cast<int>(param->small_motor));

	g_controller->SetVibration(param->large_motor, param->small_motor);

	return OK;
}

int KYTY_SYSV_ABI PadResetLightBar(int handle) {
	PRINT_NAME();

	if (handle != 1) {
		return PAD_ERROR_INVALID_HANDLE;
	}

	return OK;
}

int KYTY_SYSV_ABI PadSetLightBar(int handle, const PadLightBarParam* param) {
	PRINT_NAME();

	if (handle != 1) {
		return PAD_ERROR_INVALID_HANDLE;
	}
	if (param == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	g_controller->SetLightBar(param->r, param->g, param->b);

	return OK;
}

int KYTY_SYSV_ABI PadSetTriggerEffect(int handle, const PadTriggerEffectParam* param) {
	PRINT_NAME();

	if (handle != 1) {
		return PAD_ERROR_INVALID_HANDLE;
	}
	if (param == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	return g_controller->SetTriggerEffect(*param) ? OK : PAD_ERROR_INVALID_ARG;
}

int KYTY_SYSV_ABI PadGetTriggerEffectState(int handle, PadTriggerEffectStateInformation* info) {
	PRINT_NAME();

	if (handle != 1) {
		return PAD_ERROR_INVALID_HANDLE;
	}
	if (info == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	info->state[0] = g_controller->GetTriggerEffectState(true);
	info->state[1] = g_controller->GetTriggerEffectState(false);

	// The first changes, so a log shows whether the game polls this and what it sees (printf: the
	// console's LOGF is silent in the release configuration).
	static int                    reports = 0;
	static std::array<int32_t, 2> last {-1, -1};
	if (reports < 32 && (info->state[0] != last[0] || info->state[1] != last[1])) {
		std::printf("Pad: trigger state %d,%d\n", info->state[0], info->state[1]);
		last[0] = info->state[0];
		last[1] = info->state[1];
		reports++;
	}
	return OK;
}

} // namespace Libs::Controller
