#include "clap_presets.h"

#include <clap/clap.h>

#include <filesystem>
#include <set>
#include <system_error>

namespace gaph {

namespace {

namespace fs = std::filesystem;

// Everything the provider tells us, while indexing one library.
struct Index {
	std::string plugin_id;
	std::vector<clap_preset_discovery_location_t> locations; // pointers copied into strings below
	std::vector<std::string> location_names, location_paths;
	std::vector<std::string> extensions; // declared file types; empty matches every file
	bool any_extension = false;
	std::vector<PresetInfo> out;
	// The preset being read: its plugin ids decide whether it is kept.
	PresetInfo current;
	bool open = false, for_us = false, named_plugins = false;
	uint32_t kind = 0;
	std::string location;

	void close() {
		if (open && (for_us || !named_plugins)) {
			out.push_back(current);
		}
		open = false;
	}
};

Index &index_of(const clap_preset_discovery_indexer_t *p) { return *static_cast<Index *>(p->indexer_data); }
Index &index_of(const clap_preset_discovery_metadata_receiver_t *p) { return *static_cast<Index *>(p->receiver_data); }

bool declare_filetype(const clap_preset_discovery_indexer_t *p, const clap_preset_discovery_filetype_t *t) {
	Index &ix = index_of(p);
	if (!t->file_extension || !*t->file_extension) {
		ix.any_extension = true;
	} else {
		ix.extensions.push_back(t->file_extension);
	}
	return true;
}

bool declare_location(const clap_preset_discovery_indexer_t *p, const clap_preset_discovery_location_t *l) {
	Index &ix = index_of(p);
	ix.locations.push_back(*l);
	ix.location_names.push_back(l->name ? l->name : "");
	ix.location_paths.push_back(l->location ? l->location : "");
	return true;
}

bool declare_soundpack(const clap_preset_discovery_indexer_t *, const clap_preset_discovery_soundpack_t *) { return true; }
const void *indexer_extension(const clap_preset_discovery_indexer_t *, const char *) { return nullptr; }

void on_error(const clap_preset_discovery_metadata_receiver_t *, int32_t, const char *) {}

bool begin_preset(const clap_preset_discovery_metadata_receiver_t *p, const char *p_name, const char *p_key) {
	Index &ix = index_of(p);
	ix.close();
	ix.current = PresetInfo{};
	ix.current.id = std::to_string(ix.kind) + "\n" + ix.location + "\n" + (p_key ? p_key : "");
	ix.current.name = p_name && *p_name ? p_name : fs::path(ix.location).stem().string();
	ix.open = true;
	ix.for_us = false;
	ix.named_plugins = false;
	return true;
}

void add_plugin_id(const clap_preset_discovery_metadata_receiver_t *p, const clap_universal_plugin_id_t *p_id) {
	Index &ix = index_of(p);
	if (!p_id || !p_id->abi || std::string(p_id->abi) != "clap") {
		return; // a preset for this plugin's VST3 twin, say: not for us
	}
	ix.named_plugins = true;
	ix.for_us = ix.for_us || (p_id->id && ix.plugin_id == p_id->id);
}

void set_soundpack(const clap_preset_discovery_metadata_receiver_t *p, const char *p_pack) { index_of(p).current.collection = p_pack ? p_pack : ""; }
void set_flags(const clap_preset_discovery_metadata_receiver_t *, uint32_t) {}
void add_creator(const clap_preset_discovery_metadata_receiver_t *p, const char *p_c) {
	std::string &c = index_of(p).current.creator;
	c += (c.empty() ? "" : ", ") + std::string(p_c ? p_c : "");
}
void set_description(const clap_preset_discovery_metadata_receiver_t *p, const char *p_d) { index_of(p).current.description = p_d ? p_d : ""; }
void set_timestamps(const clap_preset_discovery_metadata_receiver_t *, clap_timestamp, clap_timestamp) {}
void add_feature(const clap_preset_discovery_metadata_receiver_t *p, const char *p_f) {
	if (p_f) {
		index_of(p).current.tags.push_back(p_f);
	}
}
void add_extra_info(const clap_preset_discovery_metadata_receiver_t *, const char *, const char *) {}

bool wanted(const Index &p_ix, const fs::path &p_file) {
	if (p_ix.any_extension || p_ix.extensions.empty()) {
		return true;
	}
	const std::string ext = p_file.extension().string();
	for (const std::string &e : p_ix.extensions) {
		if (ext == "." + e) {
			return true;
		}
	}
	return false;
}

} // namespace

std::vector<PresetInfo> clap_discover_presets(const ClapLibrary &p_library, const std::string &p_plugin_id) {
	Index ix;
	ix.plugin_id = p_plugin_id;
	const clap_plugin_entry_t *entry = p_library.get_entry();
	const auto *factory = entry ? static_cast<const clap_preset_discovery_factory_t *>(entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID)) : nullptr;
	if (!factory && entry) {
		factory = static_cast<const clap_preset_discovery_factory_t *>(entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT));
	}
	if (!factory) {
		return ix.out;
	}
	const clap_preset_discovery_indexer_t indexer = { CLAP_VERSION_INIT, "Godot Audio Plugin Host", "", "", "", &ix,
		declare_filetype, declare_location, declare_soundpack, indexer_extension };
	const clap_preset_discovery_metadata_receiver_t receiver = { &ix, on_error, begin_preset, add_plugin_id, set_soundpack, set_flags,
		add_creator, set_description, set_timestamps, add_feature, add_extra_info };
	for (uint32_t i = 0; i < factory->count(factory); i++) {
		const clap_preset_discovery_provider_descriptor_t *d = factory->get_descriptor(factory, i);
		const clap_preset_discovery_provider_t *provider = d ? factory->create(factory, &indexer, d->id) : nullptr;
		if (!provider) {
			continue;
		}
		ix.locations.clear(), ix.location_names.clear(), ix.location_paths.clear(), ix.extensions.clear();
		ix.any_extension = false;
		if (provider->init(provider)) {
			for (size_t l = 0; l < ix.locations.size(); l++) {
				const uint32_t kind = ix.locations[l].kind;
				auto read = [&](const std::string &p_where) {
					ix.kind = kind;
					ix.location = p_where;
					provider->get_metadata(provider, kind, kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? nullptr : p_where.c_str(), &receiver);
					ix.close();
				};
				if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
					read("");
					continue;
				}
				const fs::path root(ix.location_paths[l]);
				std::error_code ec;
				if (fs::is_regular_file(root, ec)) {
					read(root.string());
					continue;
				}
				std::set<std::string> files; // sorted, for a stable order
				for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
					if (it->is_regular_file(ec) && wanted(ix, it->path())) {
						files.insert(it->path().string());
					}
				}
				for (const std::string &f : files) {
					read(f);
				}
			}
		}
		provider->destroy(provider);
	}
	return ix.out;
}

bool clap_parse_preset_id(const std::string &p_id, uint32_t &r_kind, std::string &r_location, std::string &r_key) {
	const size_t a = p_id.find('\n'), b = a == std::string::npos ? a : p_id.find('\n', a + 1);
	if (b == std::string::npos) {
		return false;
	}
	r_kind = uint32_t(std::strtoul(p_id.substr(0, a).c_str(), nullptr, 10));
	r_location = p_id.substr(a + 1, b - a - 1);
	r_key = p_id.substr(b + 1);
	return true;
}

} // namespace gaph
