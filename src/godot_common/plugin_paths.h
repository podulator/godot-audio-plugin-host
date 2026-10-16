#pragma once

// Where plugins are looked for, and how a scene refers to one so it still works elsewhere.
// Shared by both extensions; `p_format` is "clap" or "vst3" throughout.
//
// THE PROBLEM
// A scene that stores "/usr/lib/clap/DragonflyHallReverb.clap" only works where the plugin sits
// at exactly that path. On a player's machine it won't, and plugins shipped with a game live
// inside the game's own folder, wherever that was installed.
//
// THE ANSWER
// A list of search folders, in order:
//   1. In an exported game: bin/<format>/ NEXT TO THE EXECUTABLE. Not res://, because res:// is
//      inside the .pck and the OS cannot load a library from there; the export plugin copies the
//      project's plugin folders out beside the executable instead. On macOS, where the
//      executable is <game>.app/Contents/MacOS/<game>, also the app's Contents/PlugIns/, which is
//      where a macOS export puts them (export_copy.h).
//   2. The project setting audio/plugin_host/<format>/search_paths (default res://bin/<format>).
//   3. The platform's system folders: in the editor (and a game run from it), if
//      audio/plugin_host/<format>/use_system_paths is on (the default), so any installed plugin can
//      be auditioned; in an exported game only if use_system_paths_in_exports is on (default OFF).
//      A shipped game then plays exactly the plugins it ships, never whatever versions a player
//      happens to have installed; a plugin used in the editor but not shipped passes audio through,
//      and the export says so (export_copy.h).
// A scene stores a plugin RELATIVE to the folder it was found in ("DragonflyHallReverb.clap"),
// and loading tries each folder in turn. Absolute paths still work where they exist, and one
// that doesn't falls back to its file name, so scenes saved before this keep working.

#include "param_properties.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace gaph_godot {

using namespace godot;

inline String setting_name(const String &p_format, const String &p_key) {
	return String("audio/plugin_host/") + p_format + String("/") + p_key;
}

// True in an exported game: the template feature is set there and never in the editor, nor
// when a project is run from the editor.
inline bool is_exported_game() {
	return OS::get_singleton()->has_feature("template");
}

// MAIN THREAD, at extension start. Adds the settings to Project Settings, with hints, so they
// appear in the editor's dialog (under Audio > Plugin Host, with "Advanced Settings" on).
inline void register_path_settings(const String &p_format) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String paths = setting_name(p_format, "search_paths");
	if (!ps->has_setting(paths)) {
		ps->set_setting(paths, PackedStringArray({ String("res://bin/") + p_format }));
	}
	Dictionary info;
	info["name"] = paths;
	info["type"] = Variant::PACKED_STRING_ARRAY;
	info["hint"] = PROPERTY_HINT_TYPE_STRING;
	info["hint_string"] = String::num_int64(Variant::STRING) + "/" + String::num_int64(PROPERTY_HINT_DIR) + ":";
	ps->add_property_info(info);
	ps->set_initial_value(paths, PackedStringArray({ String("res://bin/") + p_format }));
	ps->set_as_basic(paths, true);

	const String system = setting_name(p_format, "use_system_paths");
	if (!ps->has_setting(system)) {
		ps->set_setting(system, true);
	}
	ps->set_initial_value(system, true);
	ps->set_as_basic(system, true);

	const String in_exports = setting_name(p_format, "use_system_paths_in_exports");
	if (!ps->has_setting(in_exports)) {
		ps->set_setting(in_exports, false);
	}
	ps->set_initial_value(in_exports, false);
	ps->set_as_basic(in_exports, true);
}

// Whether the system plugin folders are searched here (see the top of this file).
inline bool use_system_paths(const String &p_format) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (is_exported_game()) {
		return bool(ps->get_setting(setting_name(p_format, "use_system_paths_in_exports"), false));
	}
	return bool(ps->get_setting(setting_name(p_format, "use_system_paths"), true));
}

// Canonical, so a path from a scan (also canonical) is recognised as inside the folder even
// through a symlink such as /usr/lib64 -> /usr/lib.
inline std::string canonical_dir(const std::string &p_dir) {
	std::error_code ec;
	std::string c = std::filesystem::weakly_canonical(std::filesystem::path(p_dir), ec).string();
	return ec || c.empty() ? p_dir : c;
}

// The folders to search, in order (see the top of this file). p_system: the format's standard
// folders (clap_default_search_paths(), or VST3's).
inline std::vector<std::string> search_dirs(const String &p_format, const std::vector<std::string> &p_system) {
	std::vector<std::string> dirs;
	if (is_exported_game()) {
		const String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
		dirs.push_back(canonical_dir(to_std(exe_dir.path_join("bin").path_join(p_format))));
		if (OS::get_singleton()->has_feature("macos")) {
			dirs.push_back(canonical_dir(to_std(exe_dir.get_base_dir().path_join("PlugIns"))));
		}
	}
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const PackedStringArray listed = ps->get_setting(setting_name(p_format, "search_paths"), PackedStringArray());
	for (const String &d : listed) {
		if (d.begins_with("res://") || d.begins_with("user://")) {
			if (d.begins_with("res://") && is_exported_game()) {
				continue; // Inside the .pck: nothing loadable there. Covered by bin/ beside the exe.
			}
			dirs.push_back(canonical_dir(to_std(ps->globalize_path(d))));
		} else if (!d.is_empty()) {
			dirs.push_back(canonical_dir(to_std(d)));
		}
	}
	if (use_system_paths(p_format)) {
		for (const std::string &d : p_system) {
			dirs.push_back(canonical_dir(d));
		}
	}
	return dirs;
}

inline bool path_exists(const std::string &p_path) {
	std::error_code ec;
	return !p_path.empty() && std::filesystem::exists(std::filesystem::path(p_path), ec);
}

// What to store in a scene for a plugin found at p_real: relative to the first search folder
// that contains it, or the absolute path if none does.
inline String portable_path(const std::string &p_real, const std::vector<std::string> &p_dirs) {
	const std::string real = canonical_dir(p_real);
	for (const std::string &d : p_dirs) {
		if (real.size() > d.size() + 1 && real.compare(0, d.size(), d) == 0 && real[d.size()] == '/') {
			return to_godot(real.substr(d.size() + 1));
		}
	}
	return to_godot(p_real);
}

// The real path for a stored reference, or "" if it is nowhere to be found.
//   absolute and present -> as is;  res:// or user:// -> globalised;
//   relative             -> the first search folder that has it;
//   absolute but missing -> its file name, looked up the same way (scenes from another machine).
// True in an exported Android game. Android only runs native code that came inside the APK (from
// Android 10 on, code in any folder the app can write to is refused), so plugins are not files
// to look for there: the export put each one in the APK as a native library (export_copy.h), and
// it is loaded BY NAME, the way Android loads an app's own libraries.
inline bool is_android_game() {
	return is_exported_game() && OS::get_singleton()->has_feature("android");
}

// The name a plugin has inside an Android APK: lib<name>.so, from a stored "<name>.clap" (a
// folder in front, as from a subfolder of bin/clap, is dropped: the APK's library folder is flat).
// A name already of that form is kept.
inline std::string android_library_name(const String &p_stored) {
	const String file = p_stored.get_file();
	if (file.begins_with("lib") && file.ends_with(".so")) {
		return to_std(file);
	}
	return to_std(String("lib") + file.get_basename() + ".so");
}

inline std::string resolve_plugin_path(const String &p_stored, const std::vector<std::string> &p_dirs) {
	if (p_stored.is_empty()) {
		return "";
	}
	if (is_android_game()) {
		return android_library_name(p_stored); // a bare name: the system loader finds it in the APK
	}
	String stored = p_stored;
	if (stored.begins_with("res://") || stored.begins_with("user://")) {
		stored = ProjectSettings::get_singleton()->globalize_path(stored);
	}
	const std::string s = to_std(stored);
	if (stored.is_absolute_path()) {
		if (path_exists(s)) {
			return s;
		}
		stored = stored.get_file();
	}
	for (const std::string &d : p_dirs) {
		const std::string candidate = d + "/" + to_std(stored);
		if (path_exists(candidate)) {
			return candidate;
		}
	}
	// "vendor/Some.clap" from a subfolder of bin/clap: a macOS export puts every plugin straight
	// into Contents/PlugIns, so try the file name alone too.
	if (stored.contains("/")) {
		for (const std::string &d : p_dirs) {
			const std::string candidate = d + "/" + to_std(stored.get_file());
			if (path_exists(candidate)) {
				return candidate;
			}
		}
	}
	return "";
}

} // namespace gaph_godot
