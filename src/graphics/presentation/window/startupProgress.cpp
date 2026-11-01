#include "graphics/presentation/window/startupProgress.h"

#include "common/threads.h"
#include "startup-progress.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "SDL.h"
#include "SDL_syswm.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

namespace Libs::Graphics {

namespace {

SDL_Window* g_window = nullptr;
std::string g_title;

#if defined(_WIN32)
// The client area dark, the text centred above a bar (fraction < 0: no bar).
void PaintClient(HWND hwnd, const char* text, double fraction) {
	RECT client {};
	if (GetClientRect(hwnd, &client) == 0 || client.right <= 0 || client.bottom <= 0) return;
	HDC dc = GetDC(hwnd);
	if (dc == nullptr) return;
	const auto fill = [&](const RECT& rect, COLORREF color) {
		HBRUSH brush = CreateSolidBrush(color);
		FillRect(dc, &rect, brush);
		DeleteObject(brush);
	};
	const int width = client.right, height = client.bottom;
	fill(client, RGB(12, 12, 12));
	const int line_height = std::max(16, height / 32);
	HFONT     font = CreateFontW(-line_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
	                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
	                             L"Segoe UI");
	HGDIOBJ   previous = SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(225, 225, 225));
	wchar_t wide[256] {};
	(void)MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 256);
	RECT line {0, height / 2 - line_height * 2, width, height / 2 - line_height / 2};
	DrawTextW(dc, wide, -1, &line, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
	if (fraction >= 0.0) {
		const int bar_width = width / 2, bar_height = std::max(6, height / 100);
		RECT      bar {(width - bar_width) / 2, height / 2, 0, 0};
		bar.right  = bar.left + bar_width;
		bar.bottom = bar.top + bar_height;
		fill(bar, RGB(52, 52, 52));
		RECT done  = bar;
		done.right = bar.left + static_cast<LONG>(bar_width * std::clamp(fraction, 0.0, 1.0));
		fill(done, RGB(117, 186, 0));
	}
	SelectObject(dc, previous);
	DeleteObject(font);
	ReleaseDC(hwnd, dc);
}
#endif

void Show(const char* text, double fraction) {
	SDL_SetWindowTitle(g_window, text);
#if defined(_WIN32)
	SDL_SysWMinfo info {};
	SDL_VERSION(&info.version);
	if (SDL_GetWindowWMInfo(g_window, &info) == SDL_TRUE && info.subsystem == SDL_SYSWM_WINDOWS)
		PaintClient(info.info.win.window, text, fraction);
#else
	(void)fraction;
#endif
	// Messages keep being handled: the window neither freezes nor turns "not responding".
	SDL_PumpEvents();
}

void Paint(const char* text, uint64_t done, uint64_t total) {
	if (g_window == nullptr || !Common::Thread::IsMainThread()) return;
	// At most 20 times a second, and whenever the text changes.
	static std::string                           shown;
	static std::chrono::steady_clock::time_point painted {};
	const auto                                   now = std::chrono::steady_clock::now();
	if (shown == text && now - painted < std::chrono::milliseconds(50)) return;
	shown   = text;
	painted = now;
	char line[256];
	if (total != 0) {
		std::snprintf(line, sizeof(line), "%s  %llu / %llu", text, static_cast<unsigned long long>(done),
		              static_cast<unsigned long long>(total));
	} else {
		std::snprintf(line, sizeof(line), "%s...", text);
	}
	Show(line, total != 0 ? static_cast<double>(done) / static_cast<double>(total) : -1.0);
}

} // namespace

void StartupProgressShow(SDL_Window* window) {
	g_window = window;
	if (const char* title = SDL_GetWindowTitle(window); title != nullptr) g_title = title;
	StartupProgress::g_painter.store(Paint, std::memory_order_release);
}

void StartupProgressHide() {
	StartupProgress::g_painter.store(nullptr, std::memory_order_release);
	if (g_window == nullptr) return;
	Show("Starting the game...", -1.0);
	SDL_SetWindowTitle(g_window, g_title.c_str());
	g_window = nullptr;
}

} // namespace Libs::Graphics
