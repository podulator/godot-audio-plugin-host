#pragma once

// At export: copy the project's plugin folders out beside the exported executable.
//
// An exported game cannot load a plugin from res://, which lives inside the .pck, so it looks in
// bin/<format>/ next to its executable instead (plugin_paths.h). This puts the plugins there:
// every res:// folder in audio/plugin_host/<format>/search_paths is copied, recursively, to
// <export folder>/bin/<format>/. Godot leaves .clap and .vst3 files out of the .pck by itself,
// since it has no importer for them.
//
// Linux and Windows exports produce an executable in a folder, which is what that expects. A macOS
// export is a .app (or a .zip, .dmg or .pkg holding one), and a .app must stay one sealed, signed
// folder: nothing may be added beside it or into it afterwards. So on macOS the plugins go in
// through Godot's own exporter instead: EditorExportPlugin::add_macos_plugin_file() asks it to copy
// a file or bundle into the app's Contents/PlugIns/, Apple's folder for exactly this, before the
// app is signed. The list is flat (Contents/PlugIns/<bundle name>), so plugins in subfolders of
// bin/<format> arrive side by side; the game finds them by file name (plugin_paths.h).
//
// Two things Godot's copier does to every folder it is given (4.5 to master, October 2026):
//   * It treats it as a framework: with no Resources/Info.plist inside (plugin bundles keep theirs
//     in Contents/), it writes one there and warns "Info.plist missing or invalid". Harmless for
//     loading; it is an extra file in the bundle.
//   * It does not sign it (it signs .dylib, .framework and extensionless folders). With code
//     signing on, every plugin must already be signed, as plugins from vendors are. Otherwise the
//     app's signature cannot cover it: Godot's built-in signer stops with "Unsigned nested
//     executable file", Apple's codesign with "code object is not signed at all".

#include "plugin_paths.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_export_platform.hpp>
#include <godot_cpp/classes/editor_export_plugin.hpp>
#include <godot_cpp/classes/file_access.hpp>

#include <map>
#include <set>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace gaph_godot {

using namespace godot;

// Copies p_from into p_to, recursively. Returns how many files were copied.
inline int copy_tree(const String &p_from, const String &p_to) {
	DirAccess::make_dir_recursive_absolute(p_to);
	Ref<DirAccess> dir = DirAccess::open(p_from);
	if (dir.is_null()) {
		return 0;
	}
	int copied = 0;
	for (const String &f : dir->get_files()) {
		if (DirAccess::copy_absolute(p_from.path_join(f), p_to.path_join(f)) == OK) {
			copied++;
		}
	}
	for (const String &d : dir->get_directories()) {
		copied += copy_tree(p_from.path_join(d), p_to.path_join(d)); // .vst3 bundles are folders
	}
	return copied;
}

// The plugins under p_dir (a res:// folder), for a macOS export: every FOLDER ending in .<format>,
// searching inside other folders but never inside a plugin bundle itself. On macOS every CLAP and
// VST 3 plugin is a bundle folder; a single .clap FILE is a Linux or Windows build, which a Mac
// could not load (and which Godot's copier would fail on, as it only creates PlugIns/ for a
// folder). Those go into r_skipped.
inline void find_plugin_bundles(const String &p_dir, const String &p_format, PackedStringArray &r_found, PackedStringArray &r_skipped) {
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}
	for (const String &f : dir->get_files()) {
		if (f.get_extension().to_lower() == p_format) {
			r_skipped.push_back(p_dir.path_join(f));
		}
	}
	for (const String &d : dir->get_directories()) {
		if (d.get_extension().to_lower() == p_format) {
			r_found.push_back(p_dir.path_join(d));
		} else {
			find_plugin_bundles(p_dir.path_join(d), p_format, r_found, r_skipped);
		}
	}
}

// EDITOR, from EditorExportPlugin::_export_begin. p_export_path is the file being exported,
// p_features the export's feature tags ("macos" for a macOS export), p_exporter the calling plugin.
inline void export_plugin_folders(const String &p_format, const String &p_export_path, const PackedStringArray &p_features,
		EditorExportPlugin *p_exporter) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const PackedStringArray listed = ps->get_setting(setting_name(p_format, "search_paths"), PackedStringArray());
	if (p_features.has("macos")) {
		PackedStringArray bundles, skipped;
		for (const String &d : listed) {
			if (d.begins_with("res://") && DirAccess::dir_exists_absolute(d)) {
				find_plugin_bundles(d, p_format, bundles, skipped);
			}
		}
		for (const String &f : skipped) {
			UtilityFunctions::print(p_format.to_upper(), " plugins: left out of the macOS export, not a macOS plugin (a single file, not a bundle): ", f);
		}
		for (const String &b : bundles) {
			p_exporter->add_macos_plugin_file(b); // copied when Godot assembles the .app
		}
		if (!bundles.is_empty()) {
			UtilityFunctions::print(p_format.to_upper(), " plugins going into the app's Contents/PlugIns: ", String(", ").join(bundles));
		}
		return;
	}
	const String target = p_export_path.get_base_dir().path_join("bin").path_join(p_format);
	int copied = 0;
	for (const String &d : listed) {
		if (d.begins_with("res://") && DirAccess::dir_exists_absolute(d)) {
			copied += copy_tree(ps->globalize_path(d), target);
		}
	}
	if (copied > 0) {
		UtilityFunctions::print(p_format.to_upper(), " plugins: ", copied, " files copied to ", target);
	}
}

// --- Plugins used but not shipped -------------------------------------------------------------
//
// EDITOR, from EditorExportPlugin::_export_file, once per exported resource. A plugin auditioned
// from the system folders is not copied by the export (only res:// folders are), and an exported
// game does not search the system folders unless use_system_paths_in_exports is on
// (plugin_paths.h). So such an effect would pass audio through in the game, silently. This reads
// each exported text resource (.tres, .tscn: how Godot saves bus layouts and scenes by default),
// finds the format's effects in it, and warns about each plugin no res:// plugin folder holds,
// naming the bus where the resource is a bus layout. Binary resources (.res, .scn) are not read.

inline String unquote_tres(const String &p_value) {
	String v = p_value.strip_edges();
	if (v.begins_with("&")) {
		v = v.substr(1); // a StringName, &"Reverb"
	}
	if (v.length() >= 2 && v.begins_with("\"") && v.ends_with("\"")) {
		v = v.substr(1, v.length() - 2).c_unescape();
	}
	return v;
}

// True if p_stored is found in one of the project's res:// plugin folders, the way an exported
// game will look for it (its own bin/<format> is a copy of exactly those folders).
inline bool shipped_with_project(const String &p_format, const String &p_stored) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	std::vector<std::string> dirs;
	for (const String &d : PackedStringArray(ps->get_setting(setting_name(p_format, "search_paths"), PackedStringArray()))) {
		if (d.begins_with("res://")) {
			dirs.push_back(canonical_dir(to_std(ps->globalize_path(d))));
		}
	}
	const std::string found = canonical_dir(resolve_plugin_path(p_stored, dirs));
	for (const std::string &d : dirs) {
		if (!found.empty() && found.size() > d.size() && found.compare(0, d.size(), d) == 0 && found[d.size()] == '/') {
			return true;
		}
	}
	return false;
}


// p_effect_class: "ClapPluginEffect" or "Vst3PluginEffect". Reports through the export dialog's
// message list (EditorExportPlatform::add_message) when there is a platform, else the Output panel.
inline void warn_unshipped_plugins(const String &p_format, const String &p_effect_class, const String &p_path,
		EditorExportPlugin *p_exporter) {
	const String ext = p_path.get_extension().to_lower();
	if (ext != "tres" && ext != "tscn") {
		return;
	}
	if (bool(ProjectSettings::get_singleton()->get_setting(setting_name(p_format, "use_system_paths_in_exports"), false))) {
		return; // The game will search the player's folders: nothing silent about a missing plugin.
	}
	const String text = FileAccess::get_file_as_string(p_path);
	if (!text.contains(p_effect_class)) {
		return; // The quick case: most resources hold no such effect.
	}
	// One pass over the file: which sections are our effects, their plugin_path, and (in a bus
	// layout) which bus each sub-resource sits on.
	std::map<std::string, String> plugin_of; // sub-resource id ("" for the file's own resource) -> plugin_path
	std::map<std::string, String> bus_of; // sub-resource id -> bus name
	std::map<int, String> bus_names;
	std::string section_id;
	bool in_effect = false;
	for (const String &raw : text.split("\n")) {
		const String line = raw.strip_edges();
		if (line.begins_with("[")) {
			const bool is_ours = line.contains(String("type=\"") + p_effect_class + "\"");
			if (line.begins_with("[sub_resource")) {
				const int at = line.find("id=\"");
				section_id = at < 0 ? "" : to_std(line.substr(at + 4).get_slice("\"", 0));
				in_effect = is_ours;
			} else if (line.begins_with("[gd_resource")) {
				section_id = "";
				in_effect = is_ours; // a saved effect on its own (.tres): its body is [resource]
			} else if (!line.begins_with("[resource]")) {
				in_effect = false; // [ext_resource], [node], [connection]...
			}
			continue;
		}
		const String key = line.get_slice("=", 0).strip_edges();
		const String value = line.contains("=") ? line.substr(line.find("=") + 1) : String();
		if (in_effect && key == "plugin_path") {
			plugin_of[section_id] = unquote_tres(value);
		} else if (key.begins_with("bus/") && key.ends_with("/name")) {
			bus_names[key.get_slice("/", 1).to_int()] = unquote_tres(value);
		} else if (key.begins_with("bus/") && key.contains("/effect/") && key.ends_with("/effect") && value.contains("SubResource(")) {
			bus_of[to_std(value.get_slice("\"", 1))] = String::num_int64(key.get_slice("/", 1).to_int());
		}
	}
	Ref<EditorExportPlatform> platform = p_exporter ? p_exporter->get_export_platform() : Ref<EditorExportPlatform>();
	std::set<std::string> told;
	for (const auto &[id, stored] : plugin_of) {
		if (stored.is_empty() || shipped_with_project(p_format, stored) || !told.insert(id + to_std(stored)).second) {
			continue;
		}
		String where = p_path;
		if (bus_of.count(id)) {
			const int bus = bus_of[id].to_int();
			where = String("the ") + (bus_names.count(bus) ? bus_names[bus] : String::num_int64(bus)) + " bus (" + p_path + ")";
		}
		const String msg = String("An effect on ") + where + " uses " + stored.get_file() + ", which is in none of the project's " +
				p_format.to_upper() + " plugin folders (audio/plugin_host/" + p_format + "/search_paths), so the export does not include it and " +
				"the game will pass that audio through unchanged. Copy the plugin into res://bin/" + p_format + " to ship it.";
		if (platform.is_valid()) {
			// Shown in the export dialog's list (and printed to the log by Godot too).
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_WARNING, p_format.to_upper() + " plugins", msg);
		} else {
			UtilityFunctions::push_warning(msg);
		}
	}
}

} // namespace gaph_godot
