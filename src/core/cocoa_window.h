#pragma once

// CocoaWindow: a plain top-level macOS window for a plugin's GUI to embed into. The macOS sibling
// of X11Window and Win32Window (native_window.h), with the same interface.
//
// HOW A PLUGIN EMBEDS ON macOS
// The host gives the plugin an NSView (CLAP "cocoa", VST3 "NSView"): here, the window's content
// view. The plugin adds its own view as a SUBVIEW of it ([parent addSubview:]), so it draws inside
// our window. Events reach it through the app's run loop, which Godot runs on the main thread.
//
// SIZES are logical points, not pixels: on a Retina screen 1 point is 2 pixels, and macOS scales
// for the plugin by itself. That is why the host does not call set_scale on macOS (native_window.h).
//
// CLOSING. AppKit closes a window itself when its close button is clicked. It is told not to free
// it on close (releasedWhenClosed = NO), and poll_close_requested() notices it has gone off screen
// without being minimised. That needs no delegate class: an Objective-C class name is global to
// the process, and both extensions (two dylibs, the same code) would define it, which the
// Objective-C runtime warns about and resolves arbitrarily.
//
// MAIN THREAD ONLY: AppKit may only be used from the main thread, which is where Godot's editor
// calls us.
//
// NO AppKit HERE: cocoa_window.mm, Objective-C++, is the one file that includes it (pimpl).

#if defined(__APPLE__)

#include <cstdint>
#include <memory>
#include <string>

namespace gaph {

class CocoaWindow {
public:
	CocoaWindow();
	~CocoaWindow();
	CocoaWindow(const CocoaWindow &) = delete;
	CocoaWindow &operator=(const CocoaWindow &) = delete;

	// Creates the window (hidden until show()), with a content area of p_width x p_height points.
	// p_owner is unused on macOS: there is no ownership between top-level windows as on Windows.
	bool create(const std::string &p_title, uint32_t p_width, uint32_t p_height, uint64_t p_owner, std::string &r_error);
	void destroy();
	bool is_open() const;

	// The NSView* a plugin embeds into (the window's content view), as an integer.
	uint64_t get_id() const;

	void show(); // also brings it to the front, and un-minimises it
	void resize(uint32_t p_width, uint32_t p_height);
	void flush() {}

	// True once the user has closed the window. Call once per frame.
	bool poll_close_requested();

private:
	struct Impl; // Everything AppKit: defined in cocoa_window.mm only.
	std::unique_ptr<Impl> impl;
};

} // namespace gaph

#endif
