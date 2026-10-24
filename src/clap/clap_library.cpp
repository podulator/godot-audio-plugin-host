#include "clap_library.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>

#ifdef __ANDROID__
#include <climits>
#include <pthread.h>
#endif

namespace gaph {

namespace fs = std::filesystem; // A namespace alias: fs::path instead of std::filesystem::path.

// The cache of loaded libraries. std::weak_ptr is the non-owning partner of shared_ptr: it
// remembers the object without keeping it alive. lock() gives back a shared_ptr if the object
// still exists, or an empty one if the last owner has let go. So the cache never keeps a plugin
// loaded by itself; it only lets a second user find the first user's copy.
//
// A function-local static ("Meyers singleton") is created the first time the function runs.
// That avoids the undefined order in which global objects in different files are constructed.
static std::map<std::string, std::weak_ptr<ClapLibrary>> &library_cache() {
	static std::map<std::string, std::weak_ptr<ClapLibrary>> cache;
	return cache;
}

#ifdef __ANDROID__
// Android gives an app only PTHREAD_KEYS_MAX (128) thread-local keys, and about 114 are free
// once Godot and the system have started. Rust code on Android uses one key
// per thread_local and never gives it back (no native TLS on that target), about 7 per plugin
// library; when a plugin cannot get one, Rust aborts the whole game without a message. So a new
// library is only loaded while a safe margin is left: the probe takes up to that many keys and
// gives them straight back. Keys another thread asks for during those microseconds could fail,
// which is why the margin is generous rather than exact.
static constexpr int THREAD_KEYS_NEEDED = 16;

static bool enough_thread_keys() {
	pthread_key_t keys[THREAD_KEYS_NEEDED];
	int n = 0;
	while (n < THREAD_KEYS_NEEDED && pthread_key_create(&keys[n], nullptr) == 0) {
		n++;
	}
	for (int i = 0; i < n; i++) {
		pthread_key_delete(keys[i]);
	}
	return n == THREAD_KEYS_NEEDED;
}
#endif

std::shared_ptr<ClapLibrary> ClapLibrary::open(const std::string &p_path, std::string &r_error) {
	// Canonicalise the path (resolve symlinks, "..", and so on), so two spellings of the same
	// file share one cache entry. The error_code overload reports problems instead of throwing.
	// A bare file name ("libnocoder.so", no folder) is a library the system loader finds by
	// itself: Android's way for the native libraries inside an APK (plugin_paths.h). It must reach
	// dlopen unchanged; made "canonical" it would become a path in the current folder.
	const bool by_name = p_path.find('/') == std::string::npos && p_path.find('\\') == std::string::npos;
	std::error_code ec;
	std::string key = by_name ? p_path : fs::weakly_canonical(fs::path(p_path), ec).string();
	if (ec || key.empty()) {
		key = p_path;
	}
	auto &cache = library_cache();
	if (auto existing = cache[key].lock()) {
		return existing;
	}

#ifdef __ANDROID__
	if (!enough_thread_keys()) {
		r_error = "Android's limit of " + std::to_string(PTHREAD_KEYS_MAX) +
				" thread-local keys per app is nearly used up by the plugins already loaded; loading "
				"another could crash the game. Plugins written in Rust use about 7 each: several "
				"plugins in one CLAP library share them";
		return nullptr;
	}
#endif

	std::string binary = key;
#ifdef __APPLE__
	// A macOS .clap is a bundle; the binary is Contents/MacOS/<name>.
	if (fs::is_directory(binary)) {
		binary = (fs::path(binary) / "Contents" / "MacOS" / fs::path(binary).stem()).string();
	}
#endif

	// `new` and not std::make_shared, because the constructor is private and make_shared cannot
	// reach it. Every early `return nullptr` below drops the only reference, so the destructor
	// runs and cleans up whatever was set up so far.
	std::shared_ptr<ClapLibrary> l(new ClapLibrary());
	l->path = key;
	if (!l->lib.open(binary, r_error)) {
		return nullptr;
	}
	// clap_entry is a data symbol (a struct of function pointers), not a function: dlsym hands
	// back its address, and we read it as the struct.
	l->entry = static_cast<const clap_plugin_entry_t *>(l->lib.symbol("clap_entry"));
	if (!l->entry) {
		r_error = "not a CLAP plugin: no clap_entry symbol";
		return nullptr;
	}
	// CLAP 1.x is ABI-stable: a 1.2 host can load any 1.x plugin. The check rejects 0.x
	// pre-releases, whose structs had a different layout.
	if (!clap_version_is_compatible(l->entry->clap_version)) {
		r_error = "incompatible CLAP version " + std::to_string(l->entry->clap_version.major) + "." +
				std::to_string(l->entry->clap_version.minor);
		return nullptr;
	}
	// The plugin is told its own path; some use it to find resource files installed beside them.
	if (!l->entry->init(key.c_str())) {
		r_error = "clap_entry.init() failed";
		return nullptr;
	}
	l->initialized = true;
	// Factories are looked up by string id, so the format can add new kinds (preset discovery,
	// for instance) without changing the entry struct. The plugin factory is the one every
	// plugin has.
	l->factory = static_cast<const clap_plugin_factory_t *>(l->entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	if (!l->factory) {
		r_error = "binary has no plugin factory";
		return nullptr;
	}
	const uint32_t n = l->factory->get_plugin_count(l->factory);
	for (uint32_t i = 0; i < n; i++) {
		const clap_plugin_descriptor_t *d = l->factory->get_plugin_descriptor(l->factory, i);
		if (d && d->id) {
			l->descriptors.push_back(d);
		}
	}
	cache[key] = l;
	return l;
}

ClapLibrary::~ClapLibrary() {
	// Order matters: deinit() while the code is still loaded, then the DynLib member's own
	// destructor unloads the binary. Members are destroyed after the destructor body runs.
	if (initialized && entry) {
		entry->deinit();
	}
	auto &cache = library_cache();
	auto it = cache.find(path);
	if (it != cache.end() && it->second.expired()) {
		cache.erase(it);
	}
}

const clap_plugin_descriptor_t *ClapLibrary::find(const std::string &p_id) const {
	if (p_id.empty()) {
		return descriptors.empty() ? nullptr : descriptors.front();
	}
	for (const clap_plugin_descriptor_t *d : descriptors) {
		if (p_id == d->id) {
			return d;
		}
	}
	return nullptr;
}

std::vector<std::string> clap_default_search_paths() {
	std::vector<std::string> dirs;
#ifdef _WIN32
	const char sep = ';';
#else
	const char sep = ':';
#endif
	if (const char *env = std::getenv("CLAP_PATH")) {
		std::string s(env);
		size_t start = 0;
		while (start <= s.size()) {
			size_t end = s.find(sep, start);
			if (end == std::string::npos) {
				end = s.size();
			}
			if (end > start) {
				dirs.push_back(s.substr(start, end - start));
			}
			start = end + 1;
		}
	}
#if defined(_WIN32)
	if (const char *cpf = std::getenv("COMMONPROGRAMFILES")) {
		dirs.push_back(std::string(cpf) + "\\CLAP");
	}
	if (const char *lad = std::getenv("LOCALAPPDATA")) {
		dirs.push_back(std::string(lad) + "\\Programs\\Common\\CLAP");
	}
#elif defined(__APPLE__)
	if (const char *home = std::getenv("HOME")) {
		dirs.push_back(std::string(home) + "/Library/Audio/Plug-Ins/CLAP");
	}
	dirs.push_back("/Library/Audio/Plug-Ins/CLAP");
#else
	if (const char *home = std::getenv("HOME")) {
		dirs.push_back(std::string(home) + "/.clap");
	}
	dirs.push_back("/usr/lib/clap");
	dirs.push_back("/usr/local/lib/clap");
#endif
	return dirs;
}

// In the order of p_dirs, so a plugin in an earlier folder (the project's own) comes first and
// wins over a copy further down (a system one). Duplicates are dropped by real path.
std::vector<std::string> clap_find_bundles(const std::vector<std::string> &p_dirs) {
	std::vector<std::string> out;
	std::set<std::string> seen;
	auto add = [&](const fs::path &p) {
		std::error_code ec;
		const std::string real = fs::weakly_canonical(p, ec).string();
		if (seen.insert(real).second) {
			out.push_back(real);
		}
	};
	for (const std::string &dir : p_dirs) {
		std::error_code ec;
		if (!fs::is_directory(dir, ec)) {
			continue;
		}
		// Sorted within each folder, so the order is the same on every run.
		std::vector<fs::path> here;
		fs::recursive_directory_iterator it(dir, fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied, ec);
		for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if (it->path().extension() != ".clap") {
				continue;
			}
#ifdef __APPLE__
			if (it->is_directory(ec)) {
				here.push_back(it->path());
				it.disable_recursion_pending();
			}
#else
			if (it->is_regular_file(ec)) {
				here.push_back(it->path());
			}
#endif
		}
		std::sort(here.begin(), here.end());
		for (const fs::path &p : here) {
			add(p);
		}
	}
	return out;
}

} // namespace gaph
