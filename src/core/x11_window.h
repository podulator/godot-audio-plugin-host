#pragma once

// X11Window: a plain top-level X11 window for a plugin's GUI to embed into.
//
// WHY X11, AND WHY ITS OWN WINDOW
// Plugin GUIs on Linux embed into an X11 window and nothing else (CLAP's "x11" API, VST3's
// "X11EmbedWindowID"), and nih-plug's GUIs cannot float on their own. So the host makes a window,
// hands its id to the plugin, and the plugin draws inside it. On a Wayland desktop this runs
// through XWayland, which is fine: the window is a separate X11 client of its own, whether
// Godot itself runs as Wayland or X11.
//
// WHY libX11 IS LOADED AT RUNTIME
// Linking it would make both extensions require libX11 just to load, in every exported game and
// on every CI runner. Plugin windows are editor-only, so libX11 is opened with dlopen the first
// time a window is needed, and its functions are looked up by name. decltype(&XOpenDisplay) is
// the exact type of XOpenDisplay, so each looked-up pointer is checked against the real
// signature from <X11/Xlib.h> at compile time; only the headers are needed to build.
//
// MAIN THREAD ONLY. Xlib connections are not thread-safe unless XInitThreads() is called first,
// and this one is only ever used from Godot's main thread.
//
// NO X11 HEADERS HERE. <X11/Xlib.h> defines Time, None, Bool, Status, True and False, as types
// and macros, which collide with Godot's own names in any file that includes both. So this
// header shows only an opaque Impl (the "pimpl" pattern) and keeps every X11 type inside
// x11_window.cpp, the one file that includes Xlib.

#if defined(__linux__) || defined(__FreeBSD__)
#define GAPH_HAS_X11 1

#include <cstdint>
#include <memory>
#include <string>

namespace gaph {

class X11Window {
public:
	X11Window();
	~X11Window();
	X11Window(const X11Window &) = delete;
	X11Window &operator=(const X11Window &) = delete;

	// Opens the window (unmapped until show()). False, with r_error set, if there is no X server
	// (no DISPLAY), or libX11 is missing.
	// p_owner is unused on X11: Godot's own window may be a Wayland one, which an X11 window
	// cannot be made transient for.
	bool create(const std::string &p_title, uint32_t p_width, uint32_t p_height, uint64_t p_owner, std::string &r_error);
	void destroy();
	bool is_open() const { return window != 0; }

	// The id a plugin embeds into: an X11 Window is a 32-bit XID, passed around as an integer.
	uint64_t get_id() const { return window; }

	void show();
	void resize(uint32_t p_width, uint32_t p_height);
	// Sends everything queued to the X server. The plugin talks to X over its own connection, so
	// the window must really exist on the server before the plugin is told about it.
	void flush();

	// Drains pending events; returns true if the user asked to close the window (its close
	// button, or the window manager's equivalent). Call once per frame.
	bool poll_close_requested();

private:
	struct Impl; // Everything X11: defined in x11_window.cpp only.
	std::unique_ptr<Impl> impl;
	unsigned long window = 0;
};

} // namespace gaph

#endif
