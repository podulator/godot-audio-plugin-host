#include "godot_common/menu_classes.h"
#include "godot_common/param_properties.h"
#include "godot_common/plugin_paths.h"
#include "godot_common/scan_guard.h"
#include "vst3_module.h"
#include "vst3_editor_plugin.h"
#include "vst3_plugin_effect.h"
#include "vst3_sidechain_send.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/editor_plugin_registration.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include <set>

using namespace godot;

static gaph_godot::MenuClasses menu_classes;

// One "Add Effect" entry per installed VST3 effect, editor only: see clap/register_types.cpp,
// which this mirrors. Bundles with a moduleinfo.json are listed without loading their binary.
// Folders: Project Settings > Audio > Plugin Host > VST3, then the system ones, which include
// GAPH_VST3_PATH (VST3 has no standard variable for extra folders; CLAP has CLAP_PATH).
static void register_menu_classes() {
	const bool forced = !OS::get_singleton()->get_environment("GAPH_MENU_CLASSES").is_empty();
	if (!Engine::get_singleton()->is_editor_hint() && !forced) {
		return;
	}
	// Guarded and logged like the CLAP scan (godot_common/scan_guard.h), to vst3_scan.log. Only
	// bundles without a moduleinfo.json load their binary, but every bundle goes through the
	// guard, so the trail is complete.
	gaph_godot::ScanGuard guard("vst3");
	std::set<std::string> used;
	// The same folders, in the same order, that loading searches (godot_common/plugin_paths.h).
	const std::vector<std::string> dirs = gaph_godot::search_dirs("vst3", gaph::vst3_default_search_paths());
	for (const std::string &path : gaph::vst3_find_bundles(dirs)) {
		if (!guard.begin(path)) {
			continue;
		}
		std::string err;
		bool from_info = false;
		// vst3_scan_bundle releases the module before returning, unless something else still
		// holds it (the module cache only keeps weak references).
		const std::vector<gaph::Vst3EffectEntry> found = gaph::vst3_scan_bundle(path, from_info, err);
		String detail = err.empty() ? itos(int64_t(found.size())) + String(" effect(s)") : gaph_godot::to_godot(err);
		for (const gaph::Vst3EffectEntry &e : found) {
			detail += String(&e == &found.front() ? " (" : ", ") + gaph_godot::to_godot(e.name);
		}
		if (!found.empty()) {
			detail += ")";
		}
		if (err.empty()) {
			detail += from_info ? String(", from moduleinfo.json") : String(", binary loaded");
		}
		guard.end(path, err.empty(), detail, int(found.size()));

		for (const gaph::Vst3EffectEntry &e : found) {
			const std::string name = gaph_godot::MenuClasses::class_name_for("Vst3", e.name, used);
			const String godot_path = gaph_godot::portable_path(path, dirs); // relative: works elsewhere
			const String godot_id = gaph_godot::to_godot(e.id);
			menu_classes.add(StringName(name.c_str()), "Vst3PluginEffect", [godot_path, godot_id]() -> Object * {
				Vst3PluginEffect *fx = memnew(Vst3PluginEffect);
				fx->set_plugin_path(godot_path);
				fx->set_plugin_id(godot_id);
				return fx;
			});
		}
	}
	guard.finish();
}

static void initialize_vst3_host(ModuleInitializationLevel p_level) {
	// The EDITOR level runs only in the editor, after SCENE: the export step lives there.
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_INTERNAL_CLASS(Vst3ExportPlugin);
		GDREGISTER_INTERNAL_CLASS(Vst3EditorPlugin);
		EditorPlugins::add_by_type<Vst3EditorPlugin>();
		return;
	}
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	gaph_godot::register_path_settings("vst3");
	// VIRTUAL, so the bus panel's Add Effect menu leaves the generic entry out (it skips
	// ClassDB::is_virtual classes) while the per-plugin entries stand in for it. A virtual
	// extension class keeps its create function: .new(), saved scenes and the menu entries all
	// still make one. Checked in the real editor's menu (tests/run_all.sh).
	GDREGISTER_VIRTUAL_CLASS(Vst3PluginEffect);
	GDREGISTER_INTERNAL_CLASS(Vst3PluginEffectInstance);
	GDREGISTER_CLASS(Vst3SidechainSend);
	GDREGISTER_INTERNAL_CLASS(Vst3SidechainSendInstance);
	register_menu_classes();
}

static void uninitialize_vst3_host(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		menu_classes.remove_all();
	}
}

extern "C" GDExtensionBool GDE_EXPORT vst3_host_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(initialize_vst3_host);
	init_obj.register_terminator(uninitialize_vst3_host);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
