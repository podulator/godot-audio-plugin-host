// The midi_host extension: MidiDriver, MidiNote, MidiClip and MidiPart. Format-neutral: no plugin code at all.
#include "midi_driver.h"
#include "midi_import.h"
#include "midi_io.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/editor_plugin_registration.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static void initialize_midi_host(ModuleInitializationLevel p_level) {
	// The EDITOR level runs only in the editor: the .mid importer.
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_INTERNAL_CLASS(MidiClipImporter);
		GDREGISTER_INTERNAL_CLASS(MidiEditorPlugin);
		EditorPlugins::add_by_type<MidiEditorPlugin>();
		return;
	}
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(MidiDriver);
	GDREGISTER_CLASS(MidiNote);
	GDREGISTER_CLASS(MidiClip);
	GDREGISTER_CLASS(MidiPart);
	GDREGISTER_CLASS(MidiOutput);
	GDREGISTER_CLASS(MidiInput);
}

static void uninitialize_midi_host(ModuleInitializationLevel) {}

extern "C" GDExtensionBool GDE_EXPORT midi_host_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(initialize_midi_host);
	init_obj.register_terminator(uninitialize_midi_host);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
