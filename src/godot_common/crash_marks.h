#pragma once

// The crash guard (core/crash_guard.h) from Godot: installed when a library starts, with its state
// next to the scan guard's, and the marks for scripts (get_crashed_plugins, clear_crashed_plugin).

#include "core/crash_guard.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace gaph_godot {

using namespace godot;

inline String crash_state_dir() {
	String dir = OS::get_singleton()->get_environment("GAPH_SCAN_STATE_DIR");
	if (dir.is_empty()) {
		dir = OS::get_singleton()->get_config_dir().path_join("godot").path_join("godot-audio-plugin-host");
	}
	return dir;
}

inline void install_crash_guard() {
	gaph::CrashGuard::install(crash_state_dir().utf8().get_data());
}

inline PackedStringArray crashed_plugins() {
	PackedStringArray out;
	for (const std::string &p : gaph::CrashGuard::marked()) {
		out.push_back(String::utf8(p.c_str()));
	}
	return out;
}

inline void clear_crashed_plugin(const String &p_path) {
	gaph::CrashGuard::clear_mark(p_path.utf8().get_data());
}

} // namespace gaph_godot
