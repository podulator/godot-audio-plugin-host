#pragma once

// Menu classes: one entry per installed plugin in the Audio bus panel's "Add Effect" menu.
//
// HOW GODOT BUILDS THAT MENU (editor/editor_audio_buses.cpp, 4.5)
// It lists every class that inherits AudioEffect and can be instantiated, by class name with
// "AudioEffect" removed, and creates the chosen one with ClassDB::instantiate(name).
//
// THE TRICK
// GDExtension lets us register a class with our own create function and a userdata pointer
// (classdb_register_extension_class5). So at editor start we register one tiny class per plugin
// found, e.g. "DragonflyHallReverb", whose create function makes an ordinary ClapPluginEffect with
// the plugin's path already set, and hands THAT back. The menu shows the plugin by name, but the
// effect on the bus, and in the saved scene, is a plain ClapPluginEffect + path. So a game needs
// no scan and no per-plugin classes, and a scene still opens where the plugin is missing.
//
// godot-cpp's ClassDB::register_class<T>() needs one C++ type per class, so this goes through
// the C API directly. The class never has instances of its own, so most callbacks stay null.

#include <gdextension_interface.h>
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <cctype>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace gaph_godot {

using namespace godot;

class MenuClasses {
public:
	// Returns the new object (an ordinary effect), or nullptr.
	using Create = std::function<Object *()>;

	// A class name for a plugin: the format prefix, then the plugin's name in CamelCase with
	// everything but letters and digits dropped ("Dragonfly Hall Reverb" -> Clap_DragonflyHallReverb).
	// Godot only accepts plain identifiers as class names (checked: "Dragonfly Plate Reverb (CLAP)"
	// is refused). Names already taken, by another plugin or an existing class, get _2, _3...
	static std::string class_name_for(const std::string &p_prefix, const std::string &p_plugin_name, std::set<std::string> &r_used) {
		std::string base = p_prefix + "_";
		bool upper_next = true;
		for (unsigned char c : p_plugin_name) {
			if (std::isalnum(c)) {
				base += upper_next ? char(std::toupper(c)) : char(c);
				upper_next = false;
			} else {
				upper_next = true;
			}
		}
		if (base.size() == p_prefix.size() + 1) {
			base += "Plugin";
		}
		std::string name = base;
		for (int n = 2; r_used.count(name) || ClassDBSingleton::get_singleton()->class_exists(StringName(name.c_str())); n++) {
			name = base + "_" + std::to_string(n);
		}
		r_used.insert(name);
		return name;
	}

	// MAIN THREAD, during extension initialisation. p_parent must be a registered class.
	// Returns false if Godot refused the class.
	bool add(const StringName &p_name, const StringName &p_parent, Create p_create) {
		std::unique_ptr<Entry> e(new Entry{ p_name, std::move(p_create) });
		GDExtensionClassCreationInfo5 info = {};
		info.is_exposed = true;
		info.create_instance_func = &create_instance;
		info.free_instance_func = &free_instance;
		info.class_userdata = e.get();
		internal::gdextension_interface_classdb_register_extension_class5(
				internal::library, e->name._native_ptr(), p_parent._native_ptr(), &info);
		// Kept only if it registered, so remove_all() never unregisters a class that isn't there.
		if (!ClassDBSingleton::get_singleton()->class_exists(p_name)) {
			return false;
		}
		entries.push_back(std::move(e));
		return true;
	}

	// During deinitialisation, before the library goes away.
	void remove_all() {
		for (const std::unique_ptr<Entry> &e : entries) {
			internal::gdextension_interface_classdb_unregister_extension_class(internal::library, e->name._native_ptr());
		}
		entries.clear();
	}

private:
	struct Entry {
		StringName name;
		Create create;
	};

	static GDExtensionObjectPtr create_instance(void *p_userdata, GDExtensionBool) {
		Object *obj = static_cast<Entry *>(p_userdata)->create();
		// The engine wants its own object pointer, not godot-cpp's wrapper around it.
		return obj ? internal::gdextension_interface_object_get_instance_from_id(obj->get_instance_id()) : nullptr;
	}

	// Never called: create_instance returns an object of another class, which that class frees.
	static void free_instance(void *, GDExtensionClassInstancePtr) {}

	std::vector<std::unique_ptr<Entry>> entries;
};

} // namespace gaph_godot
