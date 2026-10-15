#pragma once

// Loading VST3 modules, and the host-side objects every plugin instance shares.
//
// HOW A VST3 PLUGIN IS ORGANISED
//   Example.vst3/                      a bundle: a folder, on every platform
//     Contents/x86_64-linux/example.so the binary for this OS and CPU
//       GetPluginFactory()             the front door, like CLAP's clap_entry
//         IPluginFactory               lists "classes" and creates them by 16-byte id
//           class "Example"            category kVstAudioEffectClass: the processor half
//           (often) a controller class the edit-controller half, see vst3_processor.h
//
// The SDK's VST3::Hosting::Module does the bundle parsing, dlopen, and the ModuleEntry and
// ModuleExit calls Linux requires. We add a cache, as for CLAP, so one binary is entered once
// no matter how many effects use it.
//
// COM-STYLE OBJECTS
// VST3 is built on a small copy of Microsoft's COM. Every object derives from FUnknown, which has
// three functions: queryInterface (ask "do you also implement interface X?"), addRef and
// release (reference counting). IPtr<T> is the SDK's smart pointer that calls addRef and release
// for you, playing the part Godot's Ref<T> or std::shared_ptr do elsewhere.

#include "public.sdk/source/vst/hosting/module.h"

#include <memory>
#include <string>
#include <vector>

namespace gaph {

// MAIN THREAD. The already-loaded module for this path, or a freshly loaded one.
VST3::Hosting::Module::Ptr vst3_open_module(const std::string &p_path, std::string &r_error);

// The platform's standard VST3 folders (~/.vst3, /usr/lib/vst3, /usr/local/lib/vst3 on Linux),
// after any in the GAPH_VST3_PATH environment variable (VST3 defines no variable of its own).
std::vector<std::string> vst3_default_search_paths();

// Every .vst3 below the given directories, in their order. Duplicates are removed by real path,
// which matters on distributions where /usr/lib64 is a symlink to /usr/lib.
std::vector<std::string> vst3_find_bundles(const std::vector<std::string> &p_dirs);

// The audio-effect classes a module offers. Instruments and anything else are skipped.
std::vector<VST3::Hosting::ClassInfo> vst3_effect_classes(const VST3::Hosting::Module::Ptr &p_module);

// One effect class, as a scan reports it.
struct Vst3EffectEntry {
	std::string id; // 32 hex digits, what plugin_id takes
	std::string name;
	std::string vendor;
	std::string version;
	std::vector<std::string> sub_categories;
};

// MAIN THREAD. The effect classes in the bundle at p_path, for scanning.
//
// A VST3 bundle may carry Contents/Resources/moduleinfo.json, a description of its classes
// written at build time (SDK 3.7.5 and later). Reading it is far cheaper than loading the binary:
// no dlopen, no ModuleEntry, no plugin code run at all, which also keeps a crashing or slow
// plugin from taking the scan down with it. So it is used when present (r_from_moduleinfo set
// true), and the binary is loaded only for bundles without one.
std::vector<Vst3EffectEntry> vst3_scan_bundle(const std::string &p_path, bool &r_from_moduleinfo, std::string &r_error);

// The host object passed to every plugin's initialize(). Plugins ask it for our name, and use it
// to create the message objects the processor and controller send each other. One per process,
// created on first use and kept until exit.
Steinberg::FUnknown *vst3_host_context();

} // namespace gaph
