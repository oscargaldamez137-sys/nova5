// Pad input as the game sees it (src/libs/controller.cpp): the state history scePadRead gives.
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "libs/controller.h"
#include "libs/padData.h"
#include "graphics/presentation/window/hostInput.h"
#include "SDL_keycode.h"

#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Controller;

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "PadInputTests: failed: %s\n", text);
    std::abort();
  }
}

// The unread states, oldest first (Demon's Souls reads 64 a tick).
int Read(PadData *data) { return PadRead(1, data, 64); }

void CheckKeyboardWithoutGamepad() {
  PadData data[64] {};
  Libs::Graphics::HostInputInit();
  Check(PadReadState(1, data) == 0 && data[0].connected, "keyboard pad is connected without a physical gamepad");
  (void)Read(data);
  Libs::Graphics::HostInputKey(SDLK_j, true);
  Check(PadReadState(1, data) == 0 && (data[0].buttons & PAD_BUTTON_CROSS), "J reaches scePadReadState as Cross");
  Libs::Graphics::HostInputKey(SDLK_j, false);
  const int count = Read(data);
  bool pressed = false, released = false;
  for (int i = 0; i < count; ++i) {
    Check(data[i].connected, "keyboard history remains connected");
    if (data[i].buttons & PAD_BUTTON_CROSS) pressed = true;
    else if (pressed) released = true;
  }
  Check(pressed && released, "J tap reaches scePadRead with both edges");
}

// A tap, then far more stick events before the next read than the history holds: the read still has the press and
// the release, and its newest state the sticks' last values (KYTY_PAD_STATE_COALESCE).
void CheckTapAcrossStickBurst() {
  static PadData data[64];
  (void)Read(data);
  SetButton(HOST_INPUT_CONTROLLER_ID, PAD_BUTTON_CROSS, true);
  for (int i = 0; i < 200; i++) {
    SetAxis(HOST_INPUT_CONTROLLER_ID, Axis::LeftX, 100 + i % 50);
  }
  SetButton(HOST_INPUT_CONTROLLER_ID, PAD_BUTTON_CROSS, false);
  for (int i = 0; i < 200; i++) {
    SetAxis(HOST_INPUT_CONTROLLER_ID, Axis::LeftX, 150 - i % 50);
  }
  const int count = Read(data);
  bool pressed = false;
  bool released_after = false;
  for (int i = 0; i < count; i++) {
    if ((data[i].buttons & PAD_BUTTON_CROSS) != 0) {
      pressed = true;
    } else if (pressed) {
      released_after = true;
    }
  }
  Check(pressed && released_after, "a tap between two reads keeps its press and its release");
  Check(count > 0 && data[count - 1].left_stick_x == 150 - 199 % 50,
        "the newest state has the sticks' last values");
}

} // namespace

int main() {
  Common::InitializeThreads();
  Common::Subsystems subsystems;
  subsystems.Initialize<Config::Lifecycle>();
  Config::ConfigOptions options;
  options.printf_direction = Config::OutputDirection::Silent;
  Config::Load(options);
  subsystems.Initialize<Log::Lifecycle>();

  Libs::Controller::Initialize();
  CheckKeyboardWithoutGamepad();
  CheckTapAcrossStickBurst();
  Libs::Controller::Shutdown();
  subsystems.Destroy();

  std::printf("PadInputTests: all cases passed\n");
  return 0;
}
