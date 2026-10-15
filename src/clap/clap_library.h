#pragma once

// ClapLibrary: one loaded .clap file, and the plugins it contains.
//
// HOW A CLAP BINARY IS ORGANISED
//   .clap file (a shared library)
//     └─ clap_entry               the one exported symbol: init(), deinit(), get_factory()
//          └─ plugin factory      lists the plugins in this file and creates instances
//               ├─ descriptor     id, name, vendor, features ("audio-effect", "reverb", ...)
//               └─ descriptor     one file may hold several plugins (Dragonfly ships one each)
//
// LIFETIME RULES FROM THE SPEC (clap/entry.h)
//   - init() is called once, after loading, before anything else; deinit() once, before unloading.
//   - No plugin instance may outlive deinit().
// Both are enforced with std::shared_ptr: a pointer that counts how many owners it has and
// deletes the object when the last one lets go. The effect resource holds one, and so does every
// plugin instance, so the library cannot be unloaded while any instance still exists.

#include "core/dynlib.h"

#include <clap/clap.h>

#include <memory>
#include <string>
#include <vector>

namespace gaph {

class ClapLibrary {
public:
	// MAIN THREAD. Loads the binary, or returns the already-loaded copy for the same path, so
	// init() really is called only once even when the same reverb is on five buses. A `static`
	// member function belongs to the class, not to an object: call it as ClapLibrary::open(...).
	static std::shared_ptr<ClapLibrary> open(const std::string &p_path, std::string &r_error);

	~ClapLibrary();

	const std::string &get_path() const { return path; }
	const clap_plugin_factory_t *get_factory() const { return factory; }
	// The descriptors are owned by the plugin binary and stay valid until deinit().
	const std::vector<const clap_plugin_descriptor_t *> &get_descriptors() const { return descriptors; }

	// p_id empty means the first plugin in the binary: the common one-plugin-per-file case.
	const clap_plugin_descriptor_t *find(const std::string &p_id) const;

private:
	// A private constructor forces every caller through open(), and therefore through the cache.
	ClapLibrary() = default;

	std::string path;
	DynLib lib;
	const clap_plugin_entry_t *entry = nullptr;
	bool initialized = false;
	const clap_plugin_factory_t *factory = nullptr;
	std::vector<const clap_plugin_descriptor_t *> descriptors;
};

// The directories the CLAP spec says hosts should search on this platform. The CLAP_PATH
// environment variable comes first, so users can add their own.
std::vector<std::string> clap_default_search_paths();

// Every .clap below the given directories, recursively, in folder order, duplicates removed.
std::vector<std::string> clap_find_bundles(const std::vector<std::string> &p_dirs);

} // namespace gaph
