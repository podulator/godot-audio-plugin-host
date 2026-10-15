#pragma once

// NativeWindow: the platform's own top-level window, for a plugin's GUI to embed into. Each
// platform has a class with the same interface (create, show, resize, close detection, and the
// handle a plugin embeds into), and NativeWindow names the one for this build:
//
//   Linux    X11Window    (x11_window.h)    an X11 Window id; CLAP "x11", VST3 "X11EmbedWindowID"
//   Windows  Win32Window  (win32_window.h)  an HWND;          CLAP "win32", VST3 "HWND"
//   macOS    CocoaWindow  (cocoa_window.h)  an NSView*;       CLAP "cocoa", VST3 "NSView"
//
// plugin_window_api() is the rest of what differs, for the CLAP and VST3 code: which window type
// to ask the plugin for, and whether the host tells the plugin its scale. On Linux and Windows the
// plugin's sizes are physical pixels and the host passes the scale on (CLAP set_scale, VST3
// setContentScaleFactor). On macOS they are logical points: the OS scales for Retina itself, and
// CLAP's gui.h says not to call set_scale there at all.

#include "x11_window.h"
#if defined(_WIN32)
#include "win32_window.h"
#elif defined(__APPLE__)
#include "cocoa_window.h"
#endif

namespace gaph {

#if defined(GAPH_HAS_X11)
#define GAPH_HAS_PLUGIN_WINDOWS 1
using NativeWindow = X11Window;
#elif defined(_WIN32)
#define GAPH_HAS_PLUGIN_WINDOWS 1
using NativeWindow = Win32Window;
#elif defined(__APPLE__)
#define GAPH_HAS_PLUGIN_WINDOWS 1
using NativeWindow = CocoaWindow;
#endif

struct PluginWindowApi {
	const char *clap; // CLAP_WINDOW_API_* (clap/ext/gui.h)
	const char *vst3; // kPlatformType* (pluginterfaces/gui/iplugview.h)
	bool host_sets_scale;
};

inline PluginWindowApi plugin_window_api() {
#if defined(_WIN32)
	return { "win32", "HWND", true };
#elif defined(__APPLE__)
	return { "cocoa", "NSView", false };
#else
	return { "x11", "X11EmbedWindowID", true };
#endif
}

} // namespace gaph
