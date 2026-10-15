#pragma once

// Win32Window: a plain top-level Windows window for a plugin's GUI to embed into. The Windows
// sibling of X11Window (native_window.h), with the same interface.
//
// HOW A PLUGIN EMBEDS ON WINDOWS
// The host gives the plugin an HWND (CLAP "win32", VST3 "HWND"), and the plugin creates its own
// window as a CHILD of it (WS_CHILD, or SetParent). Children are clipped to the parent and move
// with it, so the plugin looks like the window's content.
//
// WHO PUMPS THE MESSAGES
// A Windows window does nothing until its thread takes its messages off the queue and dispatches
// them. Ours, and the plugin's child, are created on Godot's main thread, and Godot's main loop
// already does that for every window on the thread (PeekMessageW(nullptr, ...), then
// TranslateMessage and DispatchMessageW: display_server_windows.cpp), so both get their paint,
// mouse and key messages with no loop of ours. Plugins' own timers (SetTimer) ride on it too.
//
// SIZES are physical pixels. Godot makes the process "system DPI aware", so Windows neither
// stretches our window nor the plugin's: what the plugin asks for is what appears.
//
// OWNED, NOT A CHILD. The window is owned by Godot's main window (p_owner): it stays above the
// editor, minimises with it, and has no taskbar button of its own, as plugin windows in DAWs
// behave. It is still a top-level window with its own title bar.
//
// MAIN THREAD ONLY. A window belongs to the thread that created it, and only that thread gets its
// messages.
//
// NO <windows.h> HERE. It defines min, max, near, far, ERROR and many more as macros that break
// other headers, Godot's included. Every Win32 type stays in win32_window.cpp (pimpl, as with X11).

#if defined(_WIN32)

#include <cstdint>
#include <memory>
#include <string>

namespace gaph {

class Win32Window {
public:
	Win32Window();
	~Win32Window();
	Win32Window(const Win32Window &) = delete;
	Win32Window &operator=(const Win32Window &) = delete;

	// Creates the window (hidden until show()). p_width/p_height are the CLIENT area, the part the
	// plugin draws into; the title bar and borders are added around it. p_owner: an HWND to stay
	// above (Godot's main window), or 0.
	bool create(const std::string &p_title, uint32_t p_width, uint32_t p_height, uint64_t p_owner, std::string &r_error);
	void destroy();
	bool is_open() const;

	// The HWND a plugin embeds into, as an integer.
	uint64_t get_id() const;

	void show(); // also raises it, and restores it if minimised
	void resize(uint32_t p_width, uint32_t p_height);
	void flush() {} // Nothing to send: Windows has no connection to a server, unlike X11.

	// True once the user has asked to close the window (its close button, Alt+F4); the window
	// stays until destroy(). Call once per frame.
	bool poll_close_requested();

private:
	struct Impl; // Everything Win32: defined in win32_window.cpp only.
	std::unique_ptr<Impl> impl;
};

} // namespace gaph

#endif
