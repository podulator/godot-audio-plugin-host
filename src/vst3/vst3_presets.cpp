// VST3 presets (Vst3Processor::list_presets, load_preset). Two kinds:
// - PROGRAMS: the plugin's program lists (IUnitInfo) for its program-change parameter
//   (kIsProgramChange). Loading one sets that parameter, on every instance (HostedEffect).
//   Id: "program\n<param>\n<index>\n<count>".
// - PRESET FILES (.vstpreset): in the folders the VST3 spec names, per platform, under
//   <vendor>/<plugin name>/, plus each folder in GAPH_VST3_PRESET_PATH. A file is offered if its
//   class id is this plugin's. Its two chunks (Comp, Cont) are what our own state holds, so it is
//   loaded as a state. Id: "file\n<path>".
#include "vst3_processor.h"

#include "pluginterfaces/vst/ivstunits.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace gaph {

namespace {

namespace fs = std::filesystem;

std::string env(const char *p_name) {
	const char *v = std::getenv(p_name);
	return v ? std::string(v) : std::string();
}

// The spec's preset folders, user first.
std::vector<fs::path> preset_roots() {
	std::vector<fs::path> out;
	const std::string extra = env("GAPH_VST3_PRESET_PATH");
#if defined(_WIN32)
	const char list_sep = ';';
#else
	const char list_sep = ':';
#endif
	for (size_t a = 0; a < extra.size();) {
		const size_t b = extra.find(list_sep, a);
		const std::string one = extra.substr(a, b == std::string::npos ? std::string::npos : b - a);
		if (!one.empty()) {
			out.emplace_back(one);
		}
		a = b == std::string::npos ? extra.size() : b + 1;
	}
#if defined(_WIN32)
	if (!env("USERPROFILE").empty()) {
		out.push_back(fs::path(env("USERPROFILE")) / "Documents" / "VST3 Presets");
	}
	if (!env("APPDATA").empty()) {
		out.push_back(fs::path(env("APPDATA")) / "VST3 Presets");
	}
	if (!env("PROGRAMDATA").empty()) {
		out.push_back(fs::path(env("PROGRAMDATA")) / "VST3 Presets");
	}
#elif defined(__APPLE__)
	if (!env("HOME").empty()) {
		out.push_back(fs::path(env("HOME")) / "Library" / "Audio" / "Presets");
	}
	out.emplace_back("/Library/Audio/Presets");
#else
	if (!env("HOME").empty()) {
		out.push_back(fs::path(env("HOME")) / ".vst3" / "presets");
	}
	out.emplace_back("/usr/share/vst3/presets");
	out.emplace_back("/usr/local/share/vst3/presets");
#endif
	return out;
}

int64_t le(const std::vector<uint8_t> &p_b, size_t p_at, int p_bytes) {
	int64_t v = 0;
	for (int i = p_bytes - 1; i >= 0; i--) {
		v = (v << 8) | p_b[p_at + size_t(i)];
	}
	return v;
}

// A .vstpreset's class id and its chunks: false if it is not one.
struct PresetFile {
	std::string class_id;
	std::vector<uint8_t> comp, cont;
};

bool read_preset_file(const fs::path &p_path, bool p_chunks, PresetFile &r_out) {
	std::ifstream f(p_path, std::ios::binary);
	std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (b.size() < 48 || std::memcmp(b.data(), "VST3", 4) != 0) {
		return false;
	}
	r_out.class_id.assign(reinterpret_cast<const char *>(b.data() + 8), 32);
	if (!p_chunks) {
		return true;
	}
	const int64_t list = le(b, 40, 8);
	if (list < 0 || size_t(list) + 8 > b.size() || std::memcmp(b.data() + list, "List", 4) != 0) {
		return false;
	}
	const int64_t count = le(b, size_t(list) + 4, 4);
	for (int64_t i = 0; i < count; i++) {
		const size_t e = size_t(list) + 8 + size_t(i) * 20;
		if (e + 20 > b.size()) {
			return false;
		}
		const int64_t at = le(b, e + 4, 8), size = le(b, e + 12, 8);
		if (at < 0 || size < 0 || size_t(at + size) > b.size()) {
			return false;
		}
		std::vector<uint8_t> chunk(b.begin() + at, b.begin() + at + size);
		if (std::memcmp(b.data() + e, "Comp", 4) == 0) {
			r_out.comp = std::move(chunk);
		} else if (std::memcmp(b.data() + e, "Cont", 4) == 0) {
			r_out.cont = std::move(chunk);
		}
	}
	return !r_out.comp.empty();
}

bool same_class(const std::string &p_file_id, const VST3::UID &p_uid) {
	auto upper = [](std::string s) {
		for (char &c : s) {
			c = char(std::toupper(static_cast<unsigned char>(c)));
		}
		return s;
	};
	const std::string id = upper(p_file_id);
	return id == upper(p_uid.toString(false)) || id == upper(p_uid.toString(true));
}

} // namespace

std::vector<PresetInfo> Vst3Processor::list_presets() {
	std::vector<PresetInfo> out;
	// Programs: the list of the unit that owns the program-change parameter.
	FUnknownPtr<IUnitInfo> units(controller);
	if (controller && units) {
		for (int32 i = 0; i < controller->getParameterCount(); i++) {
			ParameterInfo pi{};
			if (controller->getParameterInfo(i, pi) != kResultOk || !(pi.flags & ParameterInfo::kIsProgramChange)) {
				continue;
			}
			ProgramListID list = kNoProgramListId;
			for (int32 u = 0; u < units->getUnitCount(); u++) {
				UnitInfo ui{};
				if (units->getUnitInfo(u, ui) == kResultOk && ui.id == pi.unitId) {
					list = ui.programListId;
				}
			}
			for (int32 l = 0; l < units->getProgramListCount(); l++) {
				ProgramListInfo li{};
				if (units->getProgramListInfo(l, li) != kResultOk || (list != kNoProgramListId && li.id != list)) {
					continue;
				}
				for (int32 p = 0; p < li.programCount; p++) {
					String128 name{};
					units->getProgramName(li.id, p, name);
					PresetInfo info;
					info.id = "program\n" + std::to_string(pi.id) + "\n" + std::to_string(p) + "\n" + std::to_string(li.programCount);
					info.name = StringConvert::convert(name);
					info.collection = StringConvert::convert(li.name);
					out.push_back(info);
				}
				break;
			}
			break;
		}
	}
	// Preset files, under <root>/<vendor>/<plugin>/ (and anywhere below a GAPH_VST3_PRESET_PATH folder).
	std::set<std::string> seen;
	const std::string vendor = class_info.vendor(), plugin = class_info.name();
	for (const fs::path &root : preset_roots()) {
		for (const fs::path &dir : { root / vendor / plugin, root }) {
			std::error_code ec;
			if (!fs::is_directory(dir, ec)) {
				continue;
			}
			std::set<std::string> files;
			for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
				if (it->is_regular_file(ec) && it->path().extension() == ".vstpreset") {
					files.insert(it->path().string());
				}
			}
			for (const std::string &f : files) {
				PresetFile pf;
				if (!seen.insert(f).second || !read_preset_file(f, false, pf) || !same_class(pf.class_id, class_info.ID())) {
					continue;
				}
				PresetInfo info;
				info.id = "file\n" + f;
				info.name = fs::path(f).stem().string();
				const fs::path parent = fs::path(f).parent_path();
				info.collection = parent == dir ? std::string() : parent.lexically_relative(dir).string();
				out.push_back(info);
			}
		}
	}
	return out;
}

bool Vst3Processor::load_preset(const std::string &p_id, std::string &r_error, uint32_t &r_param, double &r_value) {
	if (p_id.rfind("program\n", 0) == 0) {
		uint32_t param = 0;
		long index = 0, count = 0;
		if (std::sscanf(p_id.c_str() + 8, "%u\n%ld\n%ld", &param, &index, &count) != 3 || index < 0 || index >= count) {
			r_error = "not a VST3 program id";
			return false;
		}
		r_param = param;
		r_value = count > 1 ? double(index) / double(count - 1) : 0.0;
		return true;
	}
	if (p_id.rfind("file\n", 0) != 0) {
		r_error = "not a VST3 preset id";
		return false;
	}
	PresetFile pf;
	const fs::path path(p_id.substr(5));
	if (!read_preset_file(path, true, pf)) {
		r_error = "cannot read " + path.string() + " as a .vstpreset";
		return false;
	}
	if (!same_class(pf.class_id, class_info.ID())) {
		r_error = path.string() + " is a preset for another plugin";
		return false;
	}
	// Our state format: magic, the component's state, the controller's (see save_state).
	std::vector<uint8_t> state(STATE_MAGIC, STATE_MAGIC + 8);
	for (const std::vector<uint8_t> *part : { &pf.comp, &pf.cont }) {
		for (int i = 0; i < 8; i++) {
			state.push_back(uint8_t(uint64_t(part->size()) >> (8 * i)));
		}
		state.insert(state.end(), part->begin(), part->end());
	}
	if (!load_state(state)) {
		r_error = "the plugin refused the preset in " + path.string();
		return false;
	}
	return true;
}

} // namespace gaph
