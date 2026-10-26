// The midi_host extension: MidiDriver and MidiNote. Format-neutral: no plugin code at all.
#include "midi_driver.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static void initialize_midi_host(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(MidiDriver);
	GDREGISTER_CLASS(MidiNote);
}

static void uninitialize_midi_host(ModuleInitializationLevel) {}

extern "C" GDExtensionBool GDE_EXPORT midi_host_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(initialize_midi_host);
	init_obj.register_terminator(uninitialize_midi_host);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
