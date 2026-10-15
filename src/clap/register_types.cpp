#include "clap_library.h"
#include "clap_editor_plugin.h"
#include "clap_plugin_effect.h"
#include "clap_sidechain_send.h"
#include "godot_common/menu_classes.h"
#include "godot_common/param_properties.h"
#include "godot_common/plugin_paths.h"
#include "godot_common/scan_guard.h"

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

// One "Add Effect" entry per installed CLAP effect (see godot_common/menu_classes.h).
// EDITOR ONLY: saved scenes refer to ClapPluginEffect, never to these, so games don't need them.
// Godot already knows it is the editor when extensions initialise (checked: is_editor_hint() is
// true here under --editor and false in a running game). GAPH_MENU_CLASSES=1 forces the scan,
// so headless tests can check it without starting the editor.
//
// The scan loads each .clap to read its descriptors, at editor start, inside the editor's own
// process. A plugin that crashes on load would take the editor with it, so each load is guarded
// (godot_common/scan_guard.h): a plugin that crashed a previous start is skipped with a warning,
// and every step is logged to clap_scan.log.
static void register_menu_classes() {
	const bool forced = !OS::get_singleton()->get_environment("GAPH_MENU_CLASSES").is_empty();
	if (!Engine::get_singleton()->is_editor_hint() && !forced) {
		return;
	}
	struct Found {
		String path, id, name;
		bool single;
	};
	gaph_godot::ScanGuard guard("clap");
	std::set<std::string> used;
	// The same folders, in the same order, that loading searches (godot_common/plugin_paths.h).
	const std::vector<std::string> dirs = gaph_godot::search_dirs("clap", gaph::clap_default_search_paths());
	for (const std::string &path : gaph::clap_find_bundles(dirs)) {
		if (!guard.begin(path)) {
			continue;
		}
		std::vector<Found> found;
		std::string err;
		{
			// The braces end the library's scope: it is unloaded (deinit, dlclose) at the closing
			// brace, before guard.end() marks this plugin as safely done.
			std::shared_ptr<gaph::ClapLibrary> lib = gaph::ClapLibrary::open(path, err);
			if (lib) {
				for (const clap_plugin_descriptor_t *d : lib->get_descriptors()) {
					// Only effects: an instrument on a bus would replace the bus audio with its own.
					bool effect = false;
					for (const char *const *f = d->features; f && *f; f++) {
						effect = effect || std::string(*f) == CLAP_PLUGIN_FEATURE_AUDIO_EFFECT;
					}
					if (effect) {
						// Stored relative to its search folder, so the scene works on other machines.
						found.push_back(Found{ gaph_godot::portable_path(path, dirs), gaph_godot::to_godot(d->id),
								gaph_godot::to_godot(d->name ? d->name : d->id), lib->get_descriptors().size() == 1 });
					}
				}
			}
		}
		String detail = err.empty() ? itos(int64_t(found.size())) + " effect(s)" : gaph_godot::to_godot(err);
		for (const Found &f : found) {
			detail += (&f == &found.front() ? " (" : ", ") + f.name;
		}
		if (!found.empty()) {
			detail += ")";
		}
		guard.end(path, err.empty(), detail, int(found.size()));

		for (const Found &f : found) {
			const std::string name = gaph_godot::MenuClasses::class_name_for("Clap", gaph_godot::to_std(f.name), used);
			// The lambda keeps its own copy of `f` (captured by value).
			menu_classes.add(StringName(name.c_str()), "ClapPluginEffect", [f]() -> Object * {
				ClapPluginEffect *fx = memnew(ClapPluginEffect);
				fx->set_plugin_path(f.path);
				if (!f.single) {
					fx->set_plugin_id(f.id); // Only needed when the file holds several plugins.
				}
				return fx;
			});
		}
	}
	guard.finish();
}

static void initialize_clap_host(ModuleInitializationLevel p_level) {
	// The EDITOR level runs only in the editor, after SCENE: the export step lives there.
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_INTERNAL_CLASS(ClapExportPlugin);
		GDREGISTER_INTERNAL_CLASS(ClapEditorPlugin);
		EditorPlugins::add_by_type<ClapEditorPlugin>();
		return;
	}
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	gaph_godot::register_path_settings("clap");
	// VIRTUAL, so the bus panel's Add Effect menu leaves the generic entry out (it skips
	// ClassDB::is_virtual classes) while the per-plugin entries stand in for it. A virtual
	// extension class keeps its create function: .new(), saved scenes and the menu entries all
	// still make one. Checked in the real editor's menu (tests/run_all.sh).
	GDREGISTER_VIRTUAL_CLASS(ClapPluginEffect);
	GDREGISTER_INTERNAL_CLASS(ClapPluginEffectInstance);
	GDREGISTER_CLASS(ClapSidechainSend);
	GDREGISTER_INTERNAL_CLASS(ClapSidechainSendInstance);
	register_menu_classes();
}

static void uninitialize_clap_host(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		menu_classes.remove_all();
	}
}

extern "C" GDExtensionBool GDE_EXPORT clap_host_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(initialize_clap_host);
	init_obj.register_terminator(uninitialize_clap_host);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
