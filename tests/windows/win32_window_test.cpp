// Win32Window on its own, as a plugin uses it, without Godot: run under Wine on Linux
// (tests/windows/run.sh), or natively on Windows. A child window stands in for a plugin's GUI.
#include "core/win32_window.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

static int failures = 0;
#define CHECK(cond, what)                                    \
	do {                                                     \
		std::printf("%s  %s\n", (cond) ? "PASS" : "FAIL", what); \
		failures += (cond) ? 0 : 1;                          \
	} while (0)

static void pump() { // What Godot's main loop does every frame.
	MSG msg;
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}

int main() {
	std::string err;
	{
		gaph::Win32Window w;
		CHECK(w.create("Test plugin window", 640, 480, 0, err), "creates");
		HWND hwnd = reinterpret_cast<HWND>(uintptr_t(w.get_id()));
		CHECK(IsWindow(hwnd), "get_id() is a real HWND");
		RECT rc;
		GetClientRect(hwnd, &rc);
		CHECK(rc.right == 640 && rc.bottom == 480, "the client area is the size asked for");

		// The plugin's side: a child window in ours, as CLAP "win32" and VST3 "HWND" plugins make.
		HWND child = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 640, 480, hwnd, nullptr, nullptr, nullptr);
		CHECK(child && GetParent(child) == hwnd, "a plugin's child window sits inside it");

		w.show();
		pump();
		w.resize(900, 720); // a plugin asking for its scaled size
		GetClientRect(hwnd, &rc);
		CHECK(rc.right == 900 && rc.bottom == 720, "resize() sets the client area exactly");

		CHECK(!w.poll_close_requested(), "no close request before one is made");
		PostMessageW(hwnd, WM_CLOSE, 0, 0); // the close button, as Windows sends it
		pump();
		CHECK(w.poll_close_requested(), "the close button is seen");
		CHECK(IsWindow(hwnd) && IsWindow(child), "... and nothing is destroyed until the host says so");
		w.destroy();
		CHECK(!IsWindow(hwnd) && !w.is_open(), "destroy() removes it (and the plugin's child with it)");

		CHECK(w.create("Again", 300, 200, 0, err), "a second window after the first (class re-registered)");
		w.destroy();
	}
	std::printf("WIN32 WINDOW %s\n", failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
}
