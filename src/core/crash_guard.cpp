#include "crash_guard.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace gaph {

namespace {

namespace fs = std::filesystem;

// The plugin this thread is inside, or null: set by Scope, read by the crash handler on the
// crashing thread itself (synchronous faults are delivered to the thread that faulted).
thread_local const char *g_inside = nullptr;

std::mutex g_mutex; // the marks, on the main thread (the mutex is for the odd editor thread)
std::string g_dir;
std::map<std::string, int64_t> g_marks; // path -> its modification time when it crashed
bool g_installed = false;
char g_crash_file[1024] = {}; // where the handler appends: fixed before any crash

int64_t mtime_of(const std::string &p_path) {
	std::error_code ec;
	const auto t = fs::last_write_time(fs::path(p_path), ec);
	return ec ? 0 : int64_t(t.time_since_epoch().count());
}

std::string marks_file() { return g_dir.empty() ? std::string() : (fs::path(g_dir) / "crashed_plugins.txt").string(); }

void save_marks() {
	const std::string f = marks_file();
	if (f.empty()) {
		return;
	}
	std::ofstream out(f, std::ios::trunc);
	for (const auto &m : g_marks) {
		out << m.second << '\t' << m.first << '\n';
	}
}

void load_marks() {
	g_marks.clear();
	std::ifstream in(marks_file());
	std::string line;
	while (std::getline(in, line)) {
		const size_t tab = line.find('\t');
		if (tab != std::string::npos) {
			g_marks[line.substr(tab + 1)] = std::strtoll(line.substr(0, tab).c_str(), nullptr, 10);
		}
	}
	// Crashes since: one path per line, appended by the handler. Each becomes a mark.
	std::ifstream crashes(g_crash_file);
	bool any = false;
	while (std::getline(crashes, line)) {
		if (!line.empty()) {
			g_marks[line] = mtime_of(line);
			any = true;
		}
	}
	crashes.close();
	if (any) {
		std::error_code ec;
		fs::remove(g_crash_file, ec);
		save_marks();
	}
}

// The crash: if this thread is inside a plugin, append its path (async-signal-safe calls only).
void record_crash() {
	const char *p = g_inside;
	if (!p || !g_crash_file[0]) {
		return;
	}
	g_inside = nullptr; // once: the abort() that Godot's handler ends with comes back here too
#if defined(_WIN32)
	HANDLE f = CreateFileA(g_crash_file, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f != INVALID_HANDLE_VALUE) {
		DWORD done = 0;
		WriteFile(f, p, DWORD(std::strlen(p)), &done, nullptr);
		WriteFile(f, "\n", 1, &done, nullptr);
		CloseHandle(f);
	}
#else
	const int fd = open(g_crash_file, O_WRONLY | O_APPEND | O_CREAT, 0644);
	if (fd >= 0) {
		ssize_t w = write(fd, p, std::strlen(p));
		w = write(fd, "\n", 1);
		(void)w;
		close(fd);
	}
#endif
}

#if defined(_WIN32)
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

LONG WINAPI on_crash(EXCEPTION_POINTERS *p_info) {
	record_crash();
	return g_previous ? g_previous(p_info) : EXCEPTION_CONTINUE_SEARCH;
}

void install_handler() {
	g_previous = SetUnhandledExceptionFilter(on_crash);
}
#else
const int SIGNALS[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
struct sigaction g_previous[sizeof(SIGNALS) / sizeof(SIGNALS[0])];

void on_crash(int p_sig, siginfo_t *p_info, void *p_context) {
	record_crash();
	// Then whatever was there before (Godot's handler, which prints the backtrace), or the default.
	for (size_t i = 0; i < sizeof(SIGNALS) / sizeof(SIGNALS[0]); i++) {
		if (SIGNALS[i] != p_sig) {
			continue;
		}
		const struct sigaction &prev = g_previous[i];
		if (prev.sa_flags & SA_SIGINFO) {
			if (prev.sa_sigaction) {
				prev.sa_sigaction(p_sig, p_info, p_context);
				return;
			}
		} else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler) {
			prev.sa_handler(p_sig);
			return;
		}
		sigaction(p_sig, &prev, nullptr);
		raise(p_sig);
		return;
	}
}

void install_handler() {
	for (size_t i = 0; i < sizeof(SIGNALS) / sizeof(SIGNALS[0]); i++) {
		struct sigaction sa;
		std::memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = on_crash;
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGNALS[i], &sa, &g_previous[i]);
	}
}
#endif

} // namespace

void CrashGuard::install(const std::string &p_state_dir) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_installed) {
		return;
	}
	g_installed = true;
	g_dir = p_state_dir;
	std::error_code ec;
	fs::create_directories(g_dir, ec);
	std::snprintf(g_crash_file, sizeof(g_crash_file), "%s", (fs::path(g_dir) / "crashed_now.txt").string().c_str());
	load_marks();
	install_handler();
}

CrashGuard::Scope::Scope(const char *p_path) :
		previous(g_inside) {
	g_inside = p_path;
}

CrashGuard::Scope::~Scope() {
	g_inside = previous;
}

bool CrashGuard::is_marked(const std::string &p_path) {
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_marks.find(p_path);
	if (it == g_marks.end()) {
		return false;
	}
	if (it->second != mtime_of(p_path)) { // updated since it crashed: try it again
		g_marks.erase(it);
		save_marks();
		return false;
	}
	return true;
}

std::vector<std::string> CrashGuard::marked() {
	std::lock_guard<std::mutex> lock(g_mutex);
	std::vector<std::string> out;
	for (const auto &m : g_marks) {
		out.push_back(m.first);
	}
	return out;
}

void CrashGuard::clear_mark(const std::string &p_path) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (p_path.empty()) {
		g_marks.clear();
	} else {
		g_marks.erase(p_path);
	}
	save_marks();
}

std::string CrashGuard::state_file() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return marks_file();
}

} // namespace gaph
