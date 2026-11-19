#pragma once

// CLAP presets: what a library's preset-discovery factory (clap/factory/preset-discovery.h)
// offers for one plugin. MAIN THREAD. The factory's providers declare locations (folders, files,
// or the plugin itself) and file types; each file is handed to the provider, which names the
// presets in it. A preset belongs to our plugin if the provider says so (add_plugin_id), or names
// no plugin at all. Loading goes through the plugin's preset-load extension (ClapProcessor).
//
// A preset's id is "<kind>\n<location>\n<load key>", the three things preset-load takes.

#include "clap_library.h"
#include "core/plugin_processor.h"

#include <string>
#include <vector>

namespace gaph {

std::vector<PresetInfo> clap_discover_presets(const ClapLibrary &p_library, const std::string &p_plugin_id);
bool clap_parse_preset_id(const std::string &p_id, uint32_t &r_kind, std::string &r_location, std::string &r_key);

} // namespace gaph
