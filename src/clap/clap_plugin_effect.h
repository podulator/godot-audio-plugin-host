#pragma once

// The Godot-facing classes of the CLAP extension.
//
//   ClapPluginEffect          the RESOURCE: what you add to a bus in the Audio panel and edit in
//                             the inspector. Holds the plugin path, the parameter values, and
//                             the shared HostedEffect logic.
//   ClapPluginEffectInstance  what Godot creates from it per stereo channel pair per bus, and
//                             calls on the audio thread. Holds one EffectRuntime.
//
// GDCLASS(Name, Parent) is godot-cpp's macro that wires a C++ class into Godot's class system
// (ClassDB), so GDScript can create it, call its bound methods and read its properties.
// `_bind_methods()` is where that list is declared.
//
// These two names are CLAP-specific on purpose: the VST3 extension has its own pair. Two
// extensions registering the same class name is a hard load failure in Godot.

#include "clap_processor.h"
#include "core/effect_runtime.h"
#include "core/hosted_effect.h"
#include "godot_common/editor_window.h"

#include <godot_cpp/classes/audio_effect.hpp>
#include <godot_cpp/classes/audio_effect_instance.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <memory>
#include <string>

namespace godot {

class ClapPluginEffect;

class ClapPluginEffectInstance : public AudioEffectInstance {
	GDCLASS(ClapPluginEffectInstance, AudioEffectInstance);

public:
	ClapPluginEffectInstance() = default;
	~ClapPluginEffectInstance();

	void setup(const Ref<ClapPluginEffect> &p_effect);

	// `override` asks the compiler to check that this really replaces a virtual function of the
	// parent class. A typo in the signature then becomes a compile error instead of a new,
	// never-called function.
	void _process(const void *p_src_buffer, AudioFrame *p_dst_buffer, int32_t p_frame_count) override;
	bool _process_silence() const override;

protected:
	static void _bind_methods() {}

private:
	// Ref<T> is Godot's reference-counted pointer, its version of shared_ptr. Holding one keeps
	// the resource alive for as long as any instance made from it exists.
	Ref<ClapPluginEffect> effect;
	gaph::EffectRuntime runtime;
};

class ClapPluginEffect : public AudioEffect {
	GDCLASS(ClapPluginEffect, AudioEffect);

public:
	ClapPluginEffect();
	~ClapPluginEffect();

	Ref<AudioEffectInstance> _instantiate() override;

	// Properties.
	void set_plugin_path(const String &p_path);
	String get_plugin_path() const;
	void set_plugin_id(const String &p_id);
	String get_plugin_id() const;
	void set_sidechain(const String &p_channel);
	String get_sidechain() const;
	void set_plugin_state(const PackedByteArray &p_state);
	PackedByteArray get_plugin_state();
	void set_param_values(const Dictionary &p_values);
	Dictionary get_param_values();

	// Scripting API.
	bool is_plugin_loaded();
	String get_load_error();
	Dictionary get_plugin_info();
	TypedArray<Dictionary> get_parameters();
	void set_parameter(int64_t p_id, double p_value);
	double get_parameter(int64_t p_id);
	void set_parameter_at(int64_t p_id, double p_value, double p_delay);
	int64_t find_parameter(const String &p_name);
	String get_parameter_text(int64_t p_id);
	int get_instance_count();
	int get_active_instance_count();
	// The plugin's own GUI, in its own window (editor only; godot_common/editor_window.h).
	bool open_editor();
	void close_editor();
	bool is_editor_open() const;
	void set_spare_instances(int p_count);
	int get_spare_instances() const;
	int64_t get_frames_processed();
	static TypedArray<Dictionary> scan_plugins(const PackedStringArray &p_extra_dirs);

	gaph::HostedEffect &get_hosted() { return hosted; }
	void ensure_loaded();

protected:
	static void _bind_methods();
	// Dynamic properties: one inspector slider per plugin parameter. Godot asks for the list
	// (_get_property_list), then reads and writes each one by name (_get / _set).
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;

private:
	void load_plugin();
	void connect_idle();
	void on_process_frame();

	String plugin_path;
	String plugin_id;
	String sidechain_name;
	bool load_pending = false;
	bool idle_connected = false;
	String load_error;

	std::shared_ptr<gaph::ClapLibrary> library;
	// Kept so the editor can show "Size: 24.0 m" text without a bus running.
	std::unique_ptr<gaph::ClapProcessor> probe;
	gaph::HostedEffect hosted;
	gaph_godot::EditorWindow editor_window;
};

} // namespace godot
