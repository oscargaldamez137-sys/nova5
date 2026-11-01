#ifndef KYTY_GRAPHICS_PRESENTATION_WINDOW_HOST_INPUT_H_
#define KYTY_GRAPHICS_PRESENTATION_WINDOW_HOST_INPUT_H_

#include <cstdint>

union SDL_Event;

namespace Libs::Graphics {

void               HostInputInit();
void               HostInputKey(int key_code, bool down);
// The keymap (--keymap) gives that key to a control of the pad.
[[nodiscard]] bool HostInputMapsKey(int key_code);
// Whether the keyboard's default keys count (false in the Steam Deck build unless KYTY_DECK_KEYBOARD=1: there Steam's
// desktop layout sends keys for the Deck's own buttons).
[[nodiscard]] bool HostInputDefaultKeysUsed();
void               HostInputMouseButton(uint8_t mouse_button, bool down);
// Wheel notches (up > 0, down < 0) on the controls mapped to Mouse:WheelUp / Mouse:WheelDown: a short press each.
void               HostInputMouseWheel(int notches);
void               HostInputToggleMouseToJoystick();
[[nodiscard]] bool HostInputWaitEvent(SDL_Event* event);

} // namespace Libs::Graphics

#endif /* KYTY_GRAPHICS_PRESENTATION_WINDOW_HOST_INPUT_H_ */
