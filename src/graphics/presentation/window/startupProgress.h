#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_STARTUPPROGRESS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_STARTUPPROGRESS_H_

struct SDL_Window;

namespace Libs::Graphics {

// Shows the StartupProgress reports (src/local/startup-progress.h) of the window's thread in the
// window until Vulkan presents to it: on Windows a line of text over a bar, drawn with GDI, and the
// window title everywhere. Hide leaves a last line until the game's first frame.
void StartupProgressShow(SDL_Window* window);
void StartupProgressHide();

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_STARTUPPROGRESS_H_
