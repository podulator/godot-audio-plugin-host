// The VST3 addon on Android: an extension that registers nothing.
//
// There are no VST 3 plugins for Android, so there is no VST3 host there either. But a project
// holds both addons, and Godot reports three errors at every start of an Android game, and two
// warnings at every export, for a .gdextension with no Android library. Leaving the file out of
// the export does not help: the game's extension list is built from the export selection first.
// So vst3_host gets this instead, and Android games simply have no Vst3PluginEffect class.

#include <gdextension_interface.h>

static void initialize(void *p_userdata, GDExtensionInitializationLevel p_level) {
	(void)p_userdata;
	(void)p_level;
}

static void deinitialize(void *p_userdata, GDExtensionInitializationLevel p_level) {
	(void)p_userdata;
	(void)p_level;
}

__attribute__((visibility("default"))) GDExtensionBool vst3_host_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	(void)p_get_proc_address;
	(void)p_library;
	r_initialization->minimum_initialization_level = GDEXTENSION_INITIALIZATION_SCENE;
	r_initialization->userdata = 0;
	r_initialization->initialize = initialize;
	r_initialization->deinitialize = deinitialize;
	return 1;
}
