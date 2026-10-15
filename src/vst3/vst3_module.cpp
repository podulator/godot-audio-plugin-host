#include "vst3_module.h"

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/moduleinfo/moduleinfoparser.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <set>

namespace gaph {

namespace fs = std::filesystem;

VST3::Hosting::Module::Ptr vst3_open_module(const std::string &p_path, std::string &r_error) {
	static std::map<std::string, std::weak_ptr<VST3::Hosting::Module>> cache; // See clap_library.cpp.
	std::error_code ec;
	std::string key = fs::weakly_canonical(fs::path(p_path), ec).string();
	if (ec || key.empty()) {
		key = p_path;
	}
	if (auto existing = cache[key].lock()) {
		return existing;
	}
	VST3::Hosting::Module::Ptr module = VST3::Hosting::Module::create(key, r_error);
	if (module) {
		cache[key] = module;
	}
	return module;
}

std::vector<std::string> vst3_default_search_paths() {
	std::vector<std::string> dirs;
#ifdef _WIN32
	const char sep = ';';
#else
	const char sep = ':';
#endif
	if (const char *env = std::getenv("GAPH_VST3_PATH")) {
		std::string s(env);
		for (size_t start = 0; start <= s.size();) {
			size_t end = s.find(sep, start);
			end = end == std::string::npos ? s.size() : end;
			if (end > start) {
				dirs.push_back(s.substr(start, end - start));
			}
			start = end + 1;
		}
	}
#if defined(_WIN32)
	if (const char *cpf = std::getenv("COMMONPROGRAMFILES")) {
		dirs.push_back(std::string(cpf) + "\\VST3");
	}
	if (const char *lad = std::getenv("LOCALAPPDATA")) {
		dirs.push_back(std::string(lad) + "\\Programs\\Common\\VST3");
	}
#elif defined(__APPLE__)
	if (const char *home = std::getenv("HOME")) {
		dirs.push_back(std::string(home) + "/Library/Audio/Plug-Ins/VST3");
	}
	dirs.push_back("/Library/Audio/Plug-Ins/VST3");
#else
	if (const char *home = std::getenv("HOME")) {
		dirs.push_back(std::string(home) + "/.vst3");
	}
	dirs.push_back("/usr/lib/vst3");
	dirs.push_back("/usr/local/lib/vst3");
#endif
	return dirs;
}

std::vector<std::string> vst3_find_bundles(const std::vector<std::string> &p_dirs) {
	std::vector<std::string> out;
	std::set<std::string> seen;
	for (const std::string &dir : p_dirs) {
		std::error_code ec;
		if (!fs::is_directory(dir, ec)) {
			continue;
		}
		fs::recursive_directory_iterator it(dir, fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied, ec);
		for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if (it->path().extension() == ".vst3") {
				const std::string real = fs::weakly_canonical(it->path(), ec).string();
				if (seen.insert(real).second) {
					out.push_back(real);
				}
				it.disable_recursion_pending(); // Don't look inside the bundle itself.
			}
		}
	}
	return out;
}

// Instruments share the effect category (kVstAudioEffectClass, "Audio Module Class"); only the
// subcategories tell them apart, e.g. "Instrument|Synth" vs "Fx|Reverb". An instrument on a bus
// would replace the bus audio with its own output, silent with no notes to play, so we skip
// anything marked Instrument unless it is also marked Fx (a vocoder can be both).
static bool is_effect(const std::string &p_category, const std::vector<std::string> &p_subs) {
	if (p_category != kVstAudioEffectClass) {
		return false;
	}
	bool instrument = false, fx = false;
	for (const std::string &sub : p_subs) {
		instrument = instrument || sub == "Instrument";
		fx = fx || sub == "Fx";
	}
	return !instrument || fx;
}

std::vector<VST3::Hosting::ClassInfo> vst3_effect_classes(const VST3::Hosting::Module::Ptr &p_module) {
	std::vector<VST3::Hosting::ClassInfo> out;
	for (const auto &c : p_module->getFactory().classInfos()) {
		if (is_effect(c.category(), c.subCategories())) {
			out.push_back(c);
		}
	}
	return out;
}

std::vector<Vst3EffectEntry> vst3_scan_bundle(const std::string &p_path, bool &r_from_moduleinfo, std::string &r_error) {
	std::vector<Vst3EffectEntry> out;
	r_from_moduleinfo = false;

	auto info_path = VST3::Hosting::Module::getModuleInfoPath(p_path);
	if (info_path) {
		std::ifstream file(*info_path, std::ios::binary);
		std::stringstream text;
		text << file.rdbuf();
		std::stringstream parse_errors;
		auto info = Steinberg::ModuleInfoLib::parseJson(text.str(), &parse_errors);
		if (info) {
			r_from_moduleinfo = true;
			for (const auto &c : info->classes) {
				if (is_effect(c.category, c.subCategories)) {
					out.push_back(Vst3EffectEntry{ c.cid, c.name, c.vendor, c.version, c.subCategories });
				}
			}
			return out;
		}
		// A broken moduleinfo.json is the plugin's mistake; fall back to asking the binary.
	}

	VST3::Hosting::Module::Ptr mod = vst3_open_module(p_path, r_error);
	if (!mod) {
		return out;
	}
	for (const auto &c : vst3_effect_classes(mod)) {
		out.push_back(Vst3EffectEntry{ c.ID().toString(), c.name(), c.vendor(), c.version(), c.subCategories() });
	}
	return out;
}

Steinberg::FUnknown *vst3_host_context() {
	// Built the first time this runs, on the main thread, and deliberately never destroyed:
	// plugins keep their own reference to it (addRef) and may still use it while their module
	// unloads at exit. A raw static pointer says that honestly. A static IPtr would release our
	// reference at exit while a plugin still held one, leaving an object nothing points to,
	// which LeakSanitizer then (correctly) reports as leaked.
	static Steinberg::Vst::HostApplication *host = new Steinberg::Vst::HostApplication();
	// IHostApplication derives from FUnknown, so this is an ordinary upcast.
	return static_cast<Steinberg::FUnknown *>(host);
}

} // namespace gaph
