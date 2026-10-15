#pragma once

// ScanGuard: survive a plugin that crashes the editor while it is being scanned.
//
// THE PROBLEM
// Building the Add Effect menu loads plugin binaries in the editor's own process (every CLAP
// plugin; VST3 bundles without a moduleinfo.json). If one crashes on load, the editor goes down,
// and would go down again on every start after that.
//
// THE IDEA: a "dirty" marker that survives the crash
// Before loading a plugin, write its path to a small state file; after it has been unloaded
// again, clear it. If the editor dies in between, the path is still there at the next start, so
// that plugin is the culprit: it goes on a blocklist and is skipped, with a warning, while every
// other plugin is scanned as usual. Writing the path rather than a bare flag is what lets the
// rest of the scan go ahead.
//
// A blocked plugin is retried automatically once its file changes (a reinstall or an update),
// judged by its modification time. Deleting its line from the state file also unblocks it.
//
// THE TRAIL: every step is logged, so a crash shows where it happened.
// "found", "loaded" or "failed", "skipped". The file <format>_scan.log next to the state file
// holds the latest scan, flushed line by line, because after a crash the editor's Output panel is
// gone with it. The last "found" without a matching "loaded" names the culprit. The Output panel
// gets a one-line summary and any warnings; with Godot's --verbose, every step as well.
//
// Data written to a file survives the process crashing: it is in the kernel's page cache once
// the file is closed, whatever happens to the process afterwards. Only a power cut could lose it.

#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <godot_cpp/classes/time.hpp>

#include <string>

namespace gaph_godot {

using namespace godot;

class ScanGuard {
public:
	// p_format: "clap" or "vst3", one state file each.
	explicit ScanGuard(const String &p_format) {
		// Godot's per-user config folder (~/.config/godot on Linux), not the project: plugins are
		// installed per machine. GAPH_SCAN_STATE_DIR moves it, for tests.
		String dir = OS::get_singleton()->get_environment("GAPH_SCAN_STATE_DIR");
		if (dir.is_empty()) {
			dir = OS::get_singleton()->get_config_dir().path_join("godot").path_join("godot-audio-plugin-host");
		}
		DirAccess::make_dir_recursive_absolute(dir);
		path = dir.path_join(p_format + String("_scan.cfg"));
		format = p_format.to_upper();
		log_file = FileAccess::open(dir.path_join(p_format + String("_scan.log")), FileAccess::WRITE); // Truncates: latest scan only.
		started_ms = Time::get_singleton()->get_ticks_msec();
		log("scan started (state: " + dir + ")");
		state.instantiate();
		state->load(path); // A missing file just means a first run.

		const String crashed = state->get_value("scan", "in_progress", "");
		if (!crashed.is_empty()) {
			state->set_value("blocked", crashed, int64_t(FileAccess::get_modified_time(crashed)));
			state->set_value("scan", "in_progress", "");
			state->save(path);
			log("BLOCKED  " + crashed + ": the editor crashed while loading it at the last start");
			UtilityFunctions::push_warning(p_format.to_upper(), " plugin scan: the editor crashed while loading '", crashed,
					"' at the last start. It is now skipped; it will be retried when the file changes, or remove it from ", path, ".");
		}
	}

	// Before loading p_plugin. False if it is blocked: skip it.
	bool begin(const std::string &p_plugin) {
		const String plugin = String::utf8(p_plugin.c_str());
		if (state->has_section_key("blocked", plugin)) {
			const int64_t then = state->get_value("blocked", plugin);
			if (int64_t(FileAccess::get_modified_time(plugin)) == then) {
				log("skipped  " + plugin + " (blocked: crashed an earlier scan)");
				UtilityFunctions::push_warning("Skipping '", plugin, "': it crashed the editor during an earlier scan.");
				skipped++;
				return false;
			}
			log("retrying " + plugin + " (blocked before, but the file has changed)");
			state->erase_section_key("blocked", plugin);
		}
		log("found    " + plugin);
		state->set_value("scan", "in_progress", plugin);
		state->save(path);
		return true;
	}

	// After p_plugin has been loaded and its library released again (so a crash on unload
	// counts too). p_detail: what it held ("2 effects (A, B)"), or why it failed if !p_ok.
	void end(const std::string &p_plugin, bool p_ok, const String &p_detail, int p_effects) {
		state->set_value("scan", "in_progress", "");
		state->save(path);
		log(String(p_ok ? "loaded   " : "failed   ") + String::utf8(p_plugin.c_str()) + ": " + p_detail);
		plugins += p_ok ? 1 : 0;
		failed += p_ok ? 0 : 1;
		effects += p_effects;
	}

	// The one line for the Output panel.
	void finish() {
		const uint64_t ms = Time::get_singleton()->get_ticks_msec() - started_ms;
		String line = format + " scan: " + itos(effects) + " effects from " + itos(plugins) + " plugins in " + itos(int64_t(ms)) + " ms";
		if (failed) {
			line += ", " + itos(failed) + " failed to load";
		}
		if (skipped) {
			line += ", " + itos(skipped) + " skipped as blocked";
		}
		log(line);
		UtilityFunctions::print(line, " (details: ", log_path(), ")");
	}

	String log_path() const { return path.get_base_dir().path_join(format.to_lower() + String("_scan.log")); }

private:
	void log(const String &p_line) {
		const String stamped = Time::get_singleton()->get_time_string_from_system() + " " + p_line;
		if (log_file.is_valid()) {
			log_file->store_line(stamped);
			log_file->flush(); // Out of our process now: survives a crash on the very next line.
		}
		if (OS::get_singleton()->is_stdout_verbose()) {
			UtilityFunctions::print(format, " scan: ", p_line);
		}
	}

	String path;
	String format;
	Ref<ConfigFile> state;
	Ref<FileAccess> log_file;
	uint64_t started_ms = 0;
	int plugins = 0, failed = 0, skipped = 0, effects = 0;
};

} // namespace gaph_godot
