#pragma once

// CrashGuard: a plugin that crashed the program is not loaded again (Mat, 19.11.2026: "simple crash
// protection: mark, and don't load").
//
// WHO IS TO BLAME
// Several plugins can be loaded at once, and most crashes happen on the audio thread, so "which
// plugins were loaded" is not enough. Every call into a plugin's code (its process(), loading it,
// its state, its GUI, its main-thread work) runs inside a Scope, which records "this thread is inside
// this plugin" (one thread_local pointer: free on the audio thread). A crash handler, installed once per library, looks the crashing thread up: if it was
// inside a plugin, that plugin's path is appended to a file with async-signal-safe calls only, and
// the previous handler (Godot's, which prints the backtrace) runs as before. A crash elsewhere
// marks nothing.
//
// NEXT START
// The appended paths become marks (with the file's modification time). A marked plugin is refused
// with a load error saying why; it is retried once its file changes (an update), or when the mark
// is cleared (clear_mark, from scripts: clear_crashed_plugin()). One state file for CLAP and VST3,
// next to the scan guard's (GAPH_SCAN_STATE_DIR moves it, for tests).

#include <string>
#include <vector>

namespace gaph {

class CrashGuard {
public:
	// MAIN THREAD, once per library at start: the state folder, and the crash handler.
	static void install(const std::string &p_state_dir);

	// While alive, this thread is inside the plugin at p_path (a pointer that outlives the scope:
	// the processor's own copy). Nested scopes keep the innermost.
	class Scope {
	public:
		explicit Scope(const char *p_path);
		~Scope();
		Scope(const Scope &) = delete;
		Scope &operator=(const Scope &) = delete;

	private:
		const char *previous = nullptr;
	};

	// MAIN THREAD. Whether p_path crashed the program before (and is unchanged since), and the marks.
	static bool is_marked(const std::string &p_path);
	static std::vector<std::string> marked();
	static void clear_mark(const std::string &p_path); // empty: every mark
	static std::string state_file();
};

} // namespace gaph
