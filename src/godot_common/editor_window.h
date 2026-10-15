#pragma once

// EditorWindow: the plugin GUI window behind open_editor() / close_editor(), shared by both
// effect classes. Its own window, editor only, opened by clicking
// the effect on a bus, and it stays open until closed (like a DAW).
//
// Each frame (tick):
//   - the user closed the window, or the plugin closed its GUI  -> close;
//   - the plugin asked for a new size                          -> resize the window;
//   - the instance showing the GUI went away, or the GUI is on the probe while a live instance
//     now exists                                                -> reopen in the same window.
// The window itself never moves or blinks during a reopen: only the plugin's view inside it is
// rebuilt, on the instance HostedEffect picks.

#include "core/hosted_effect.h"
#include "core/native_window.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <string>

namespace gaph_godot {

using namespace godot;

class EditorWindow {
public:
	// MAIN THREAD. Opens the window and the plugin's GUI in it, or raises it if already open.
	bool open(gaph::HostedEffect &p_hosted, const std::string &p_title, std::string &r_error) {
#ifdef GAPH_HAS_PLUGIN_WINDOWS
		if (!Engine::get_singleton()->is_editor_hint()) {
			r_error = "plugin windows are available in the editor only";
			return false;
		}
		if (window.is_open()) {
			window.show();
			return true;
		}
		// The owner: the editor's main window, so ours stays above it (Windows; ignored elsewhere).
		// get_native_handle(WINDOW_HANDLE) is the HWND on Windows.
		const uint64_t owner = uint64_t(DisplayServer::get_singleton()->window_get_native_handle(DisplayServer::WINDOW_HANDLE, 0));
		if (!window.create(p_title, 400, 300, owner, r_error)) {
			return false;
		}
		// Map (show) the window BEFORE the plugin's GUI goes in. Editors that paint only on demand
		// (nice-plug's egui editors, since 11.10.2026) paint once when shown; drawn into a window
		// that is not yet on screen, that frame is lost (X keeps no contents for unmapped
		// windows), and with nothing to trigger another paint the window stayed black. Mapped
		// first, the first frame lands on screen. (Their window library repainting on Expose would
		// also cure it: worth reporting upstream.)
		window.show();
		if (!attach(p_hosted, r_error)) {
			window.destroy();
			return false;
		}
		return true;
#else
		r_error = "plugin windows are not implemented on this platform";
		return false;
#endif
	}

	// MAIN THREAD. Closes the plugin's GUI first, then its window (the reverse order would pull
	// the window out from under a GUI still drawing into it).
	void close(gaph::HostedEffect &p_hosted) {
		p_hosted.gui_close();
#ifdef GAPH_HAS_PLUGIN_WINDOWS
		window.destroy();
#endif
	}

	bool is_open() const {
#ifdef GAPH_HAS_PLUGIN_WINDOWS
		return window.is_open();
#else
		return false;
#endif
	}

	// MAIN THREAD, every frame, AFTER HostedEffect::idle() (which may have destroyed the GUI's
	// instance). Returns true if the window was closed.
	bool tick(gaph::HostedEffect &p_hosted) {
#ifdef GAPH_HAS_PLUGIN_WINDOWS
		if (!window.is_open()) {
			return false;
		}
		gaph::PluginProcessor *owner = p_hosted.get_gui_owner();
		if (window.poll_close_requested() || (owner && owner->gui_take_closed())) {
			close(p_hosted);
			return true;
		}
		if (p_hosted.take_gui_lost() || p_hosted.gui_wants_live_instance()) {
			std::string err;
			if (!attach(p_hosted, err)) {
				UtilityFunctions::push_warning("Plugin window: could not reattach the GUI: ", String::utf8(err.c_str()));
				close(p_hosted);
				return true;
			}
			owner = p_hosted.get_gui_owner();
		}
		uint32_t w, h;
		if (owner && owner->gui_take_resize(w, h)) {
			window.resize(w, h);
		}
#endif
		return false;
	}

private:
#ifdef GAPH_HAS_PLUGIN_WINDOWS
	// The plugin draws at the editor's own display scale (Editor Settings > Interface > Editor >
	// Display Scale), so its window matches the size of the editor's UI.
	// Read each time a GUI is attached, so a changed setting applies to the next window.
	// GAPH_GUI_SCALE overrides it, for testing scaling without changing the editor's setting
	// (which only applies after an editor restart).
	static double editor_scale() {
		const String forced = OS::get_singleton()->get_environment("GAPH_GUI_SCALE");
		if (!forced.is_empty() && forced.to_float() > 0) {
			return forced.to_float();
		}
		EditorInterface *ei = EditorInterface::get_singleton();
		const double s = ei ? double(ei->get_editor_scale()) : 1.0;
		return s > 0 ? s : 1.0;
	}

	bool attach(gaph::HostedEffect &p_hosted, std::string &r_error) {
		uint32_t w = 0, h = 0;
		// macOS: no scale from the host; the OS scales for Retina (native_window.h).
		const double scale = gaph::plugin_window_api().host_sets_scale ? editor_scale() : 1.0;
		if (!p_hosted.gui_open(window.get_id(), scale, w, h, r_error)) {
			return false;
		}
		window.resize(w, h);
		return true;
	}

	gaph::NativeWindow window;
#endif
};

} // namespace gaph_godot
