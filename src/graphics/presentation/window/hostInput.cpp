#include "graphics/presentation/window/hostInput.h"

#include "SDL_error.h"
#include "SDL_events.h"
#include "SDL_keyboard.h"
#include "SDL_keycode.h"
#include "SDL_mouse.h"
#include "SDL_timer.h"
#include "SDL_video.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "libs/controller.h"
#include "local/input-trace.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace Libs::Graphics {

namespace {

struct ControlInfo {
	std::string_view name;
	uint32_t         button   = 0;
	Controller::Axis axis     = Controller::Axis::AxisMax;
	bool             positive = false;
	float            touch_x  = 0.0f;
};

static constexpr std::array CONTROL_INFO = {
    ControlInfo {"L3", Controller::PAD_BUTTON_L3},
    ControlInfo {"R3", Controller::PAD_BUTTON_R3},
    ControlInfo {"Options", Controller::PAD_BUTTON_OPTIONS},
    ControlInfo {"Up", Controller::PAD_BUTTON_UP},
    ControlInfo {"Right", Controller::PAD_BUTTON_RIGHT},
    ControlInfo {"Down", Controller::PAD_BUTTON_DOWN},
    ControlInfo {"Left", Controller::PAD_BUTTON_LEFT},
    ControlInfo {"L2", Controller::PAD_BUTTON_L2},
    ControlInfo {"R2", Controller::PAD_BUTTON_R2},
    ControlInfo {"L1", Controller::PAD_BUTTON_L1},
    ControlInfo {"R1", Controller::PAD_BUTTON_R1},
    ControlInfo {"Triangle", Controller::PAD_BUTTON_TRIANGLE},
    ControlInfo {"Circle", Controller::PAD_BUTTON_CIRCLE},
    ControlInfo {"Cross", Controller::PAD_BUTTON_CROSS},
    ControlInfo {"Square", Controller::PAD_BUTTON_SQUARE},
    ControlInfo {"TouchPad", Controller::PAD_BUTTON_TOUCH_PAD, Controller::Axis::AxisMax, false,
                 0.25f},
    ControlInfo {"TouchPadRight", Controller::PAD_BUTTON_TOUCH_PAD, Controller::Axis::AxisMax,
                 false, 0.75f},
    ControlInfo {"LeftStickLeft", 0, Controller::Axis::LeftX, false},
    ControlInfo {"LeftStickRight", 0, Controller::Axis::LeftX, true},
    ControlInfo {"LeftStickUp", 0, Controller::Axis::LeftY, false},
    ControlInfo {"LeftStickDown", 0, Controller::Axis::LeftY, true},
    ControlInfo {"RightStickLeft", 0, Controller::Axis::RightX, false},
    ControlInfo {"RightStickRight", 0, Controller::Axis::RightX, true},
    ControlInfo {"RightStickUp", 0, Controller::Axis::RightY, false},
    ControlInfo {"RightStickDown", 0, Controller::Axis::RightY, true},
};

constexpr std::size_t INVALID_CONTROL = CONTROL_INFO.size();

struct Binding {
	SDL_Keycode key          = SDLK_UNKNOWN;
	uint8_t     mouse_button = 0;
	std::size_t control      = INVALID_CONTROL;
};

constexpr int              MOUSE_POLL_INTERVAL_MS = 33;
constexpr std::string_view MOUSE_SENSITIVITY      = "MouseSensitivity=";
// The wheel's notches as mouse "buttons" (Mouse:WheelUp, Mouse:WheelDown; past SDL's buttons).
constexpr uint8_t MOUSE_WHEEL_UP   = 0xf0;
constexpr uint8_t MOUSE_WHEEL_DOWN = 0xf1;

struct MouseJoystickState {
	bool     enabled   = false;
	bool     output    = false;
	uint64_t next_poll = 0;
};

MouseJoystickState g_mouse;

std::size_t ControlFromName(std::string_view name) {
	const auto info = std::find_if(CONTROL_INFO.begin(), CONTROL_INFO.end(),
	                               [name](const auto& item) { return item.name == name; });
	return static_cast<std::size_t>(std::distance(CONTROL_INFO.begin(), info));
}

SDL_Keycode NormalizeKey(SDL_Keycode key) {
	switch (key) {
		case SDLK_RSHIFT: return SDLK_LSHIFT;
		case SDLK_RCTRL: return SDLK_LCTRL;
		case SDLK_RALT: return SDLK_LALT;
		case SDLK_RGUI: return SDLK_LGUI;
		default: return key;
	}
}

// A key by its SDL name ("Left Shift"; case does not matter), or by that name without its spaces ("LeftShift") or a
// usual short one (Spacebar, Esc, Enter, Shift, Ctrl, Alt, LShift, RCtrl...): issue #91's list used those.
SDL_Keycode KeyFromName(std::string_view name) {
	if (const auto key = SDL_GetKeyFromName(std::string(name).c_str()); key != SDLK_UNKNOWN) {
		return key;
	}
	std::string compact;
	for (const char c: name) {
		if (c != ' ' && c != '_' && c != '-') {
			compact += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}
	static constexpr std::array aliases = {
	    std::pair {std::string_view("spacebar"), SDLK_SPACE},     std::pair {std::string_view("esc"), SDLK_ESCAPE},
	    std::pair {std::string_view("enter"), SDLK_RETURN},       std::pair {std::string_view("shift"), SDLK_LSHIFT},
	    std::pair {std::string_view("leftshift"), SDLK_LSHIFT},   std::pair {std::string_view("lshift"), SDLK_LSHIFT},
	    std::pair {std::string_view("rightshift"), SDLK_RSHIFT},  std::pair {std::string_view("rshift"), SDLK_RSHIFT},
	    std::pair {std::string_view("ctrl"), SDLK_LCTRL},         std::pair {std::string_view("control"), SDLK_LCTRL},
	    std::pair {std::string_view("leftctrl"), SDLK_LCTRL},     std::pair {std::string_view("lctrl"), SDLK_LCTRL},
	    std::pair {std::string_view("leftcontrol"), SDLK_LCTRL},  std::pair {std::string_view("rightctrl"), SDLK_RCTRL},
	    std::pair {std::string_view("rctrl"), SDLK_RCTRL},        std::pair {std::string_view("rightcontrol"), SDLK_RCTRL},
	    std::pair {std::string_view("alt"), SDLK_LALT},           std::pair {std::string_view("leftalt"), SDLK_LALT},
	    std::pair {std::string_view("lalt"), SDLK_LALT},          std::pair {std::string_view("rightalt"), SDLK_RALT},
	    std::pair {std::string_view("ralt"), SDLK_RALT},
	};
	const auto alias = std::find_if(aliases.begin(), aliases.end(),
	                                [&compact](const auto& item) { return item.first == compact; });
	return alias != aliases.end() ? alias->second : SDL_GetKeyFromName(compact.c_str());
}

uint8_t MouseButtonFromName(std::string_view name) {
	static constexpr std::array buttons = {
	    std::pair {std::string_view("Mouse:Left"), uint8_t {SDL_BUTTON_LEFT}},
	    std::pair {std::string_view("Mouse:Middle"), uint8_t {SDL_BUTTON_MIDDLE}},
	    std::pair {std::string_view("Mouse:Right"), uint8_t {SDL_BUTTON_RIGHT}},
	    std::pair {std::string_view("Mouse:X1"), uint8_t {SDL_BUTTON_X1}},
	    std::pair {std::string_view("Mouse:X2"), uint8_t {SDL_BUTTON_X2}},
	    std::pair {std::string_view("Mouse:WheelUp"), MOUSE_WHEEL_UP},
	    std::pair {std::string_view("Mouse:WheelDown"), MOUSE_WHEEL_DOWN},
	};

	const auto button = std::find_if(buttons.begin(), buttons.end(),
	                                 [name](const auto& item) { return item.first == name; });
	return button != buttons.end() ? button->second : 0;
}

// A control takes one key and one mouse input (Up=Up with Up=Mouse:WheelUp: both), each input one control.
bool Conflicts(const Binding& first, const Binding& second) {
	return (first.control == second.control && (first.key != SDLK_UNKNOWN) == (second.key != SDLK_UNKNOWN)) ||
	       (first.key != SDLK_UNKNOWN && first.key == second.key) ||
	       (first.mouse_button != 0 && first.mouse_button == second.mouse_button);
}

class InputMap {
public:
	InputMap() {
		for (const auto& value: Config::GetKeymap()) {
			const std::string_view entry = value;
			if (entry.starts_with(MOUSE_SENSITIVITY)) {
				const float sensitivity =
				    std::strtof(value.c_str() + MOUSE_SENSITIVITY.size(), nullptr);
				m_mouse_sensitivity =
				    std::clamp(std::isfinite(sensitivity) ? sensitivity : 1.0f, 0.1f, 5.0f);
				continue;
			}
			const auto split = entry.find('=');

			Binding binding;
			if (split != std::string_view::npos) {
				binding.control       = ControlFromName(entry.substr(0, split));
				const auto host_input = entry.substr(split + 1);
				binding.mouse_button  = MouseButtonFromName(host_input);
				if (binding.mouse_button == 0) {
					binding.key = NormalizeKey(KeyFromName(host_input));
				}
			}

			// The emulator's own keys: F7 the mouse camera, F8 its overlay, F11 full screen. (Space pauses unless a
			// mapping takes it, the Pause key then; the system's dialogs see Esc first.)
			const bool reserved = binding.key == SDLK_F7 || binding.key == SDLK_F8 || binding.key == SDLK_F11;
			if (binding.control == INVALID_CONTROL || reserved ||
			    (binding.key == SDLK_UNKNOWN && binding.mouse_button == 0)) {
				// (Skipped, not the end of the start-up: the other mappings still work.)
				std::printf("Input mapping skipped: %s (%s)\n", value.c_str(),
				            binding.control == INVALID_CONTROL ? "no such control"
				            : reserved                         ? "one of the emulator's own keys"
				                                               : "no such key or mouse button");
				continue;
			}
			Add(binding);
		}
	}
	[[nodiscard]] bool  Custom() const { return m_size != 0; }
	[[nodiscard]] float MouseSensitivity() const { return m_mouse_sensitivity; }

	[[nodiscard]] std::size_t FindKey(int key_code) const {
		key_code = NormalizeKey(static_cast<SDL_Keycode>(key_code));
		const auto binding =
		    std::find_if(m_bindings.begin(), m_bindings.begin() + m_size,
		                 [key_code](const auto& item) { return item.key == key_code; });
		return binding != m_bindings.begin() + m_size ? binding->control : INVALID_CONTROL;
	}

	[[nodiscard]] std::size_t FindMouseButton(uint8_t mouse_button) const {
		const auto binding = std::find_if(
		    m_bindings.begin(), m_bindings.begin() + m_size,
		    [mouse_button](const auto& item) { return item.mouse_button == mouse_button; });
		return binding != m_bindings.begin() + m_size ? binding->control : INVALID_CONTROL;
	}

private:
	void Add(const Binding& binding) {
		for (std::size_t index = 0; index < m_size;) {
			if (Conflicts(m_bindings[index], binding)) {
				m_bindings[index] = m_bindings[--m_size];
			} else {
				index++;
			}
		}
		EXIT_IF(m_size >= m_bindings.size());
		m_bindings[m_size++] = binding;
	}

	std::array<Binding, CONTROL_INFO.size() * 2> m_bindings {};
	std::size_t                              m_size              = 0;
	float                                    m_mouse_sensitivity = 1.0f;
};

const InputMap& GetInputMap() {
	static const InputMap map;
	return map;
}

void SetButton(uint32_t button, bool down) {
	if (button == Controller::PAD_BUTTON_L2) {
		Controller::SetAxis(Controller::HOST_INPUT_CONTROLLER_ID, Controller::Axis::TriggerLeft,
		                    down ? 255 : 0);
	} else if (button == Controller::PAD_BUTTON_R2) {
		Controller::SetAxis(Controller::HOST_INPUT_CONTROLLER_ID, Controller::Axis::TriggerRight,
		                    down ? 255 : 0);
	} else if (button != 0) {
		Controller::SetButton(Controller::HOST_INPUT_CONTROLLER_ID, button, down);
	}
}

void SetTouchPad(float x, bool down) {
	Controller::SetTouchPad(Controller::HOST_INPUT_CONTROLLER_ID, 0, down, x, 0.5f);
}

uint32_t DefaultKeyboardButton(int key_code) {
	switch (NormalizeKey(static_cast<SDL_Keycode>(key_code))) {
		case SDLK_UP: return Controller::PAD_BUTTON_UP;
		case SDLK_LEFT: return Controller::PAD_BUTTON_LEFT;
		case SDLK_DOWN: return Controller::PAD_BUTTON_DOWN;
		case SDLK_RIGHT: return Controller::PAD_BUTTON_RIGHT;
		case SDLK_j: return Controller::PAD_BUTTON_CROSS;
		case SDLK_i: return Controller::PAD_BUTTON_TRIANGLE;
		case SDLK_k: return Controller::PAD_BUTTON_SQUARE;
		case SDLK_l: return Controller::PAD_BUTTON_CIRCLE;
		case SDLK_q: return Controller::PAD_BUTTON_L1;
		case SDLK_e: return Controller::PAD_BUTTON_R1;
		case SDLK_LSHIFT: return Controller::PAD_BUTTON_L3;
		case SDLK_LCTRL: return Controller::PAD_BUTTON_R3;
		case SDLK_RETURN:
		case SDLK_RETURN2: return Controller::PAD_BUTTON_OPTIONS;
		default: return 0;
	}
}

struct StickKeys {
	bool left  = false;
	bool right = false;
	bool up    = false;
	bool down  = false;
};

void SetStickAxis(Controller::Axis axis, bool negative, bool positive) {
	int value = 128;
	if (negative && !positive) {
		value = 0;
	} else if (positive && !negative) {
		value = 255;
	}
	Controller::SetAxis(Controller::HOST_INPUT_CONTROLLER_ID, axis, value);
}

// Who holds a control: its key, its mouse input (a button or a wheel notch). It is up once neither does.
constexpr uint8_t FROM_KEY = 1, FROM_MOUSE = 2;

void SetControl(std::size_t control, bool down, uint8_t source) {
	if (control == INVALID_CONTROL) {
		return;
	}
	static std::array<uint8_t, CONTROL_INFO.size()> held {};
	held[control] = static_cast<uint8_t>(down ? held[control] | source : held[control] & ~source);
	down          = held[control] != 0;

	const auto& info = CONTROL_INFO[control];
	if (info.button == Controller::PAD_BUTTON_TOUCH_PAD) {
		SetTouchPad(info.touch_x, down);
		return;
	}
	if (info.button != 0) {
		SetButton(info.button, down);
		return;
	}

	struct AxisKeys {
		bool negative = false;
		bool positive = false;
	};
	static std::array<AxisKeys, 4> axes;

	const auto axis = static_cast<std::size_t>(info.axis);
	EXIT_IF(axis >= axes.size());
	if (info.positive) {
		axes[axis].positive = down;
	} else {
		axes[axis].negative = down;
	}
	SetStickAxis(info.axis, axes[axis].negative, axes[axis].positive);
}

void DefaultKeyboardInput(int key_code, bool down) {
	static StickKeys left;
	static StickKeys right;

	switch (NormalizeKey(static_cast<SDL_Keycode>(key_code))) {
		case SDLK_BACKSPACE: SetTouchPad(0.25f, down); return;
		case SDLK_TAB: SetTouchPad(0.75f, down); return;
		case SDLK_a:
			left.left = down;
			SetStickAxis(Controller::Axis::LeftX, left.left, left.right);
			return;
		case SDLK_d:
			left.right = down;
			SetStickAxis(Controller::Axis::LeftX, left.left, left.right);
			return;
		case SDLK_w:
			left.up = down;
			SetStickAxis(Controller::Axis::LeftY, left.up, left.down);
			return;
		case SDLK_s:
			left.down = down;
			SetStickAxis(Controller::Axis::LeftY, left.up, left.down);
			return;
		case SDLK_f:
			right.left = down;
			SetStickAxis(Controller::Axis::RightX, right.left, right.right);
			return;
		case SDLK_h:
			right.right = down;
			SetStickAxis(Controller::Axis::RightX, right.left, right.right);
			return;
		case SDLK_t:
			right.up = down;
			SetStickAxis(Controller::Axis::RightY, right.up, right.down);
			return;
		case SDLK_g:
			right.down = down;
			SetStickAxis(Controller::Axis::RightY, right.up, right.down);
			return;
		default: SetButton(DefaultKeyboardButton(key_code), down); return;
	}
}

void MouseToJoystick(int delta_x, int delta_y) {
	const double distance = std::hypot(delta_x, delta_y);
	const double scale =
	    std::clamp(distance * GetInputMap().MouseSensitivity() + 16.0, 64.0, 128.0) / distance;
	const auto map_axis = [scale](int delta) {
		return std::clamp(128 + static_cast<int>(std::lround(delta * scale)), 0, 255);
	};
	Controller::SetRightStick(Controller::HOST_INPUT_CONTROLLER_ID, map_axis(delta_x),
	                          map_axis(delta_y));
}

void CenterMouseStick() {
	if (!g_mouse.output) {
		return;
	}
	Controller::SetRightStick(Controller::HOST_INPUT_CONTROLLER_ID, 128, 128);
	g_mouse.output = false;
}

bool SetRelativeMouseMode(bool enabled) {
	if (SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE) == 0) {
		return true;
	}
	LOGF("Mouse-to-joystick relative mode failed: %s\n", SDL_GetError());
	return false;
}
} // namespace

void HostInputInit() {
	GetInputMap();
}

// The Steam Deck build leaves the keyboard's default pad keys out (KYTY_DECK_KEYBOARD=1 keeps them): started outside
// Steam, Steam's desktop layout sends keys for the Deck's sticks and buttons on top of the controller itself (the arrow
// keys pressed the D-pad: the equipment changed while walking). A keymap's keys still count.
static bool DefaultKeysUsed() {
#if defined(KYTY_STEAM_DECK)
	static const bool used = [] {
		const char* value = std::getenv("KYTY_DECK_KEYBOARD");
		return value != nullptr && std::string_view(value) == "1";
	}();
	return used;
#else
	return true;
#endif
}

void HostInputKey(int key_code, bool down) {
	const auto& map = GetInputMap();
	if (InputTrace::Enabled()) {
		const auto control = map.Custom() ? map.FindKey(key_code) : INVALID_CONTROL;
		std::printf("[INPUT] host-key key=%d down=%d map=%s control=%s default-buttons=0x%x focus=%d\n",
		            key_code, down, map.Custom() ? "custom" : (DefaultKeysUsed() ? "default" : "disabled"),
		            control != INVALID_CONTROL ? CONTROL_INFO[control].name.data() : "-",
		            !map.Custom() && DefaultKeysUsed() ? DefaultKeyboardButton(key_code) : 0,
		            SDL_GetKeyboardFocus() != nullptr);
		std::fflush(stdout);
	}
	if (map.Custom()) {
		SetControl(map.FindKey(key_code), down, FROM_KEY);
	} else if (DefaultKeysUsed()) {
		DefaultKeyboardInput(key_code, down);
	}
}

bool HostInputMapsKey(int key_code) {
	const auto& map = GetInputMap();
	return map.Custom() && map.FindKey(key_code) != INVALID_CONTROL;
}

bool HostInputDefaultKeysUsed() {
	return DefaultKeysUsed();
}

void HostInputMouseButton(uint8_t mouse_button, bool down) {
	const auto& map = GetInputMap();
	if (map.Custom() && mouse_button != 0) {
		SetControl(map.FindMouseButton(mouse_button), down, FROM_MOUSE);
	}
}

// The control a wheel notch presses (Mouse:WheelUp / Mouse:WheelDown): the wheel has no release, so each notch is a
// press of WHEEL_PRESS_MS, then WHEEL_GAP_MS up before the next (the game reads its pad once a frame: a shorter press
// could fall between two reads, and back to back two notches would be one press).
constexpr uint64_t WHEEL_PRESS_MS = 80, WHEEL_GAP_MS = 50;
constexpr int      WHEEL_QUEUED   = 3; // notches kept at most

struct WheelPulse {
	std::size_t control = INVALID_CONTROL;
	int         pending = 0;     // notches not pressed yet
	bool        down    = false; // pressed now
	uint64_t    until   = 0;     // the next change
};
static WheelPulse g_wheel;

// The wheel's control pressed or released when due; the ms until its next change (-1: none).
static int UpdateWheel(uint64_t now) {
	for (;;) {
		if (g_wheel.control == INVALID_CONTROL) return -1;
		if (now < g_wheel.until) return static_cast<int>(g_wheel.until - now);
		if (g_wheel.down) {
			SetControl(g_wheel.control, false, FROM_MOUSE);
			g_wheel.down  = false;
			g_wheel.until = now + WHEEL_GAP_MS;
		} else if (g_wheel.pending > 0) {
			SetControl(g_wheel.control, true, FROM_MOUSE);
			g_wheel.pending--;
			g_wheel.down  = true;
			g_wheel.until = now + WHEEL_PRESS_MS;
		} else {
			g_wheel = {};
		}
	}
}

void HostInputMouseWheel(int notches) {
	const auto& map = GetInputMap();
	if (!map.Custom() || notches == 0) return;
	const auto control = map.FindMouseButton(notches > 0 ? MOUSE_WHEEL_UP : MOUSE_WHEEL_DOWN);
	if (control == INVALID_CONTROL) return;
	if (g_wheel.control != control) {
		if (g_wheel.down) SetControl(g_wheel.control, false, FROM_MOUSE);
		g_wheel = {control, 0, false, 0};
	}
	g_wheel.pending = std::min(WHEEL_QUEUED, g_wheel.pending + std::abs(notches));
	(void)UpdateWheel(SDL_GetTicks64());
}

void HostInputToggleMouseToJoystick() {
	if (g_mouse.enabled) {
		SetRelativeMouseMode(false);
		CenterMouseStick();
		g_mouse = {};
		LOGF("Mouse to right stick: disabled\n");
		return;
	}

	if (!SetRelativeMouseMode(true)) {
		return;
	}
	int ignored_x = 0;
	int ignored_y = 0;
	SDL_GetRelativeMouseState(&ignored_x, &ignored_y);
	g_mouse.enabled   = true;
	g_mouse.next_poll = SDL_GetTicks64() + MOUSE_POLL_INTERVAL_MS;
	LOGF("Mouse to right stick: enabled (F7 to release)\n");
}

int PollMouse(uint64_t now_ms) {
	if (now_ms < g_mouse.next_poll) {
		return static_cast<int>(g_mouse.next_poll - now_ms);
	}
	g_mouse.next_poll = now_ms + MOUSE_POLL_INTERVAL_MS;

	int delta_x = 0;
	int delta_y = 0;
	SDL_GetRelativeMouseState(&delta_x, &delta_y);
	if (delta_x == 0 && delta_y == 0) {
		CenterMouseStick();
		return MOUSE_POLL_INTERVAL_MS;
	}

	MouseToJoystick(delta_x, delta_y);
	g_mouse.output = true;
	return MOUSE_POLL_INTERVAL_MS;
}

// The cursor over the window hides 3 s after the mouse last moved and shows again when it moves (issue #129). (The mouse
// as a stick hides it anyway: relative mode.)
constexpr uint64_t CURSOR_HIDE_MS  = 3000;
constexpr int      CURSOR_MIN_MOVE = 4; // pixels
static uint64_t    g_cursor_moved  = 0;
static bool        g_cursor_hidden = false;
static bool        g_cursor_known  = false;
static int         g_cursor_x = 0, g_cursor_y = 0; // where the mouse last counted as moved

// KYTY_HIDE_CURSOR=1: the cursor never shows over the window; 0: as above. The Steam Deck build's default is 1: Steam
// Input's own mouse (the gyro, the trackpads) moved it into view while the right stick turned the camera.
static bool CursorAlwaysHidden() {
	if (Config::HideCursorEnabled()) return true;
	static const bool hidden = [] {
		const char* value = std::getenv("KYTY_HIDE_CURSOR");
#if defined(KYTY_STEAM_DECK)
		return value == nullptr || std::string_view(value) != "0";
#else
		return value != nullptr && std::string_view(value) == "1";
#endif
	}();
	return hidden;
}

static void NoteCursorEvent(const SDL_Event& event) {
	if (event.type != SDL_MOUSEMOTION) return;
	// (Motion of a few pixels is not counted: a mouse resting on the desk, or its software, can report tiny moves on its
	// own, which kept the cursor shown for good, issue #188.)
	const int dx = event.motion.x - g_cursor_x, dy = event.motion.y - g_cursor_y;
	if (g_cursor_known && dx * dx + dy * dy < CURSOR_MIN_MOVE * CURSOR_MIN_MOVE) return;
	g_cursor_known = true;
	g_cursor_x     = event.motion.x;
	g_cursor_y     = event.motion.y;
	g_cursor_moved = SDL_GetTicks64();
	if (g_cursor_hidden && !CursorAlwaysHidden()) {
		SDL_ShowCursor(SDL_ENABLE);
		g_cursor_hidden = false;
	}
}

bool HostInputWaitEvent(SDL_Event* event) {
	// (A wheel notch's press or release due within the wait: the wait ends then.)
	const int  wheel_ms = UpdateWheel(SDL_GetTicks64());
	const auto capped   = [wheel_ms](int timeout_ms) {
		if (InputTrace::Enabled()) timeout_ms = timeout_ms < 0 ? 1000 : std::min(timeout_ms, 1000);
		return wheel_ms >= 0 ? std::min(timeout_ms, wheel_ms) : timeout_ms;
	};
	if (!g_mouse.enabled || SDL_GetKeyboardFocus() == nullptr) {
		CenterMouseStick();
		const uint64_t now = SDL_GetTicks64();
		if (g_cursor_moved == 0) g_cursor_moved = now;
		if (!g_cursor_hidden && (CursorAlwaysHidden() || now >= g_cursor_moved + CURSOR_HIDE_MS)) {
			SDL_ShowCursor(SDL_DISABLE);
			g_cursor_hidden = true;
		}
		if (g_cursor_hidden && wheel_ms < 0 && !InputTrace::Enabled()) {
			if (SDL_WaitEvent(event) == 0) {
				EXIT("%s\n", SDL_GetError());
			}
			NoteCursorEvent(*event);
			return true;
		}
		SDL_ClearError();
		const int cursor_ms = g_cursor_hidden ? wheel_ms : static_cast<int>(g_cursor_moved + CURSOR_HIDE_MS - now);
		if (SDL_WaitEventTimeout(event, capped(cursor_ms)) != 0) {
			NoteCursorEvent(*event);
			return true;
		}
		if (SDL_GetError()[0] != '\0') {
			EXIT("%s\n", SDL_GetError());
		}
		return false;
	}

	const int timeout_ms = capped(PollMouse(SDL_GetTicks64()));
	SDL_ClearError();
	if (SDL_WaitEventTimeout(event, timeout_ms) != 0) {
		return true;
	}
	if (SDL_GetError()[0] != '\0') {
		EXIT("%s\n", SDL_GetError());
	}
	return false;
}

} // namespace Libs::Graphics
