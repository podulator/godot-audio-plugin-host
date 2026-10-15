#include "vst3_plugin_effect.h"
#include "godot_common/plugin_paths.h"
#include "godot_common/param_properties.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>

namespace godot {

using gaph_godot::to_std;

// --- Vst3PluginEffectInstance ---------------------------------------------------------------

// Both of these run on the main thread WITH THE AUDIO LOCK HELD (see effect_runtime.h), so they
// do nothing slow: no plugin is loaded, created or destroyed here. A plugin still waiting for
// its deferred load reaches this runtime later, through set_plugin(); see HostedEffect::attach.
void Vst3PluginEffectInstance::setup(const Ref<Vst3PluginEffect> &p_effect) {
	effect = p_effect;
	runtime.set_sample_rate(AudioServer::get_singleton()->get_mix_rate());
	effect->get_hosted().attach(runtime);
}

Vst3PluginEffectInstance::~Vst3PluginEffectInstance() {
	// After this, the runtime member is destroyed, and with it the plugin instance.
	// (Comments that only repeat clap_plugin_effect.cpp are kept short here.)
	if (effect.is_valid()) {
		effect->get_hosted().detach(runtime);
	}
}

// AUDIO THREAD. Godot's AudioFrame is {float left; float right;} with no padding, so an array of
// them has exactly the layout of interleaved floats, and reinterpret_cast reads it as one.
void Vst3PluginEffectInstance::_process(const void *p_src_buffer, AudioFrame *p_dst_buffer, int32_t p_frame_count) {
	runtime.process(static_cast<const float *>(p_src_buffer), reinterpret_cast<float *>(p_dst_buffer), uint32_t(p_frame_count));
}

// AUDIO THREAD. Godot skips effects on a silent bus unless this returns true. With a plugin
// loaded we keep running, so a reverb or delay tail rings out after the sound that fed it stops.
bool Vst3PluginEffectInstance::_process_silence() const {
	return runtime.has_processor();
}

// --- Vst3PluginEffect: lifecycle and loading ------------------------------------------------

// HostedEffect gets the audio lock as two lambdas, so the core never includes a Godot header.
// AudioServer's lock is recursive (Godot's Mutex wraps std::recursive_mutex), so taking it
// again while Godot already holds it, inside _instantiate, is safe.
Vst3PluginEffect::Vst3PluginEffect() :
		hosted(gaph::HostedEffect::AudioLock{
				[] { AudioServer::get_singleton()->lock(); },
				[] { AudioServer::get_singleton()->unlock(); } }) {
}

Vst3PluginEffect::~Vst3PluginEffect() {
	// The plugin's GUI and its window go first, explicitly: members are destroyed in reverse
	// order, and the GUI must close while its window and its plugin instance still exist.
	editor_window.close(hosted);
	hosted.set_reference(nullptr);
}

Ref<AudioEffectInstance> Vst3PluginEffect::_instantiate() {
	Ref<Vst3PluginEffectInstance> instance;
	instance.instantiate(); // Creates the object, like `Vst3PluginEffectInstance.new()`.
	instance->setup(Ref<Vst3PluginEffect>(this));
	return instance;
}

// Setting plugin_path or plugin_id only marks the plugin as needing a (re)load. The load itself
// happens on the next idle frame (call_deferred), or sooner if something needs the plugin first.
// This way a saved scene, which sets both properties in turn, loads the plugin once, not twice.
// A changed plugin also clears the stored values: parameter ids are per plugin.
void Vst3PluginEffect::set_plugin_path(const String &p_path) {
	if (p_path == plugin_path) {
		return;
	}
	plugin_path = p_path;
	hosted.clear_values();
	hosted.clear_state();
	load_pending = true;
	callable_mp(this, &Vst3PluginEffect::ensure_loaded).call_deferred();
}

String Vst3PluginEffect::get_plugin_path() const {
	return plugin_path;
}

void Vst3PluginEffect::set_plugin_id(const String &p_id) {
	if (p_id == plugin_id) {
		return;
	}
	plugin_id = p_id;
	hosted.clear_values();
	hosted.clear_state();
	load_pending = true;
	callable_mp(this, &Vst3PluginEffect::ensure_loaded).call_deferred();
}

String Vst3PluginEffect::get_plugin_id() const {
	return plugin_id;
}

// The name of a sidechain channel, as set on a Vst3SidechainSend; empty for none. Applied to
// running instances immediately (under the audio lock, inside HostedEffect).
void Vst3PluginEffect::set_sidechain(const String &p_channel) {
	sidechain_name = p_channel;
	hosted.set_sidechain(gaph::sidechain_channel(gaph_godot::to_std(p_channel)));
}

String Vst3PluginEffect::get_sidechain() const {
	return sidechain_name;
}

// plugin_state: the plugin's own saved state, stored in the scene file next to param_values.
// The getter asks the plugin, so the bytes are always current. When Godot loads a saved scene,
// the setter runs before the deferred plugin load, so the bytes are kept and applied as the
// plugin comes up (HostedEffect::set_plugin). That also keeps all plugin calls on the main
// thread, even if the scene itself was loaded on a worker thread.
void Vst3PluginEffect::set_plugin_state(const PackedByteArray &p_state) {
	std::vector<uint8_t> bytes(p_state.ptr(), p_state.ptr() + p_state.size());
	if (load_pending || !hosted.has_plugin()) {
		hosted.set_pending_state(std::move(bytes));
	} else {
		hosted.load_state(bytes);
		notify_property_list_changed(); // A preset moves the sliders.
	}
}

PackedByteArray Vst3PluginEffect::get_plugin_state() {
	ensure_loaded();
	std::vector<uint8_t> bytes;
	PackedByteArray out;
	if (hosted.save_state(bytes)) {
		out.resize(int64_t(bytes.size()));
		std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	}
	return out;
}

void Vst3PluginEffect::ensure_loaded() {
	if (load_pending) {
		load_plugin();
	}
}

void Vst3PluginEffect::load_plugin() {
	load_pending = false;
	load_error = "";
	// HostedEffect holds a raw pointer to the probe: clear it before the probe goes away.
	hosted.set_reference(nullptr);
	probe.reset();

	auto fail = [this](const std::string &p_err) {
		load_error = String::utf8(p_err.c_str());
		UtilityFunctions::push_error("Vst3PluginEffect: ", plugin_path, ": ", load_error);
		hosted.set_plugin({}, {}); // Every instance falls back to passthrough.
		module.reset();
		notify_property_list_changed();
	};

	if (plugin_path.is_empty()) {
		hosted.set_plugin({}, {});
		module.reset();
		notify_property_list_changed();
		return;
	}

	// plugin_path may be absolute, res://, or relative to a search folder (what the Add Effect
	// menu stores, so scenes work on other machines): godot_common/plugin_paths.h.
	const std::string resolved = gaph_godot::resolve_plugin_path(plugin_path, gaph_godot::search_dirs("vst3", gaph::vst3_default_search_paths()));
	if (resolved.empty()) {
		fail("not found in any plugin folder (Project Settings > Audio > Plugin Host > VST3)");
		return;
	}
	const String real_path = gaph_godot::to_godot(resolved);

	std::string err;
	VST3::Hosting::Module::Ptr mod = gaph::vst3_open_module(to_std(real_path), err);
	if (!mod) {
		fail(err);
		return;
	}

	// The probe: one instance with its controller, never started, used for the parameter list
	// and value text even when the effect is on no bus. See clap_plugin_effect.cpp.
	const double rate = AudioServer::get_singleton()->get_mix_rate();
	const std::string id = to_std(plugin_id);
	probe = gaph::Vst3Processor::create(mod, id, rate, gaph::EffectRuntime::MAX_BLOCK, false, err);
	if (!probe) {
		fail(err);
		return;
	}

	// The capture list copies `mod` (a shared_ptr), keeping the module loaded while the factory
	// exists.
	gaph::HostedEffect::Factory factory = [mod, id, rate](std::string &r_error) -> std::unique_ptr<gaph::PluginProcessor> {
		return gaph::Vst3Processor::create(mod, id, rate, gaph::EffectRuntime::MAX_BLOCK, true, r_error);
	};
	hosted.set_reference(probe.get());
	err = hosted.set_plugin(factory, probe->read_params());
	if (!err.empty()) {
		UtilityFunctions::push_error("Vst3PluginEffect: ", String::utf8(err.c_str()));
	}
	module = mod;
	connect_idle();
	notify_property_list_changed(); // Tells the inspector to rebuild the parameter sliders.
}

// CLAP plugins need regular main-thread time (on_main_thread, restarts), and we need to collect
// the parameter changes they report. A Resource has no _process() of its own, so we borrow the
// SceneTree's process_frame signal, which fires once per frame, in the editor too.
void Vst3PluginEffect::connect_idle() {
	if (idle_connected) {
		return;
	}
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (!tree) {
		return; // Too early in startup; the next load_plugin() tries again.
	}
	tree->connect("process_frame", callable_mp(this, &Vst3PluginEffect::on_process_frame));
	idle_connected = true;
}

void Vst3PluginEffect::on_process_frame() {
	const std::vector<gaph::ParamEvent> changes = hosted.idle();
	const std::string err = hosted.take_error();
	if (!err.empty()) {
		UtilityFunctions::push_error("Vst3PluginEffect: ", String::utf8(err.c_str()));
	}
	for (const gaph::ParamEvent &ev : changes) {
		emit_signal("parameter_changed", int64_t(ev.param_id), ev.value);
	}
	// After idle(), which may have destroyed the instance showing the GUI: tick() reattaches it.
	if (editor_window.tick(hosted)) {
		emit_signal("editor_closed");
	}
	if (hosted.take_params_list_changed()) {
		notify_property_list_changed(); // The inspector rebuilds its sliders.
		emit_signal("parameters_changed");
	}
}

// --- Parameters as inspector properties -----------------------------------------------------
// The work is in godot_common/param_properties.h, shared with the VST3 extension; it also
// explains the two views (stored param_values, editor-only parameters/<name>).

bool Vst3PluginEffect::_set(const StringName &p_name, const Variant &p_value) {
	uint32_t id;
	if (!gaph_godot::param_id_from_property(hosted, p_name, id)) {
		return false; // Not ours: Godot falls back to the normal, bound properties.
	}
	hosted.set_param(id, double(p_value));
	return true;
}

bool Vst3PluginEffect::_get(const StringName &p_name, Variant &r_ret) const {
	uint32_t id;
	if (!gaph_godot::param_id_from_property(hosted, p_name, id)) {
		return false;
	}
	r_ret = hosted.get_param(id);
	return true;
}

void Vst3PluginEffect::_get_property_list(List<PropertyInfo> *p_list) const {
	// The inspector may ask before the deferred load has run. const_cast removes `const` from
	// `this`, which is allowed because loading only fills caches; nothing visible to the caller
	// changes.
	const_cast<Vst3PluginEffect *>(this)->ensure_loaded();
	gaph_godot::append_param_properties(hosted, p_list);
}

void Vst3PluginEffect::set_param_values(const Dictionary &p_values) {
	gaph_godot::values_from_dictionary(hosted, p_values);
}

Dictionary Vst3PluginEffect::get_param_values() {
	return gaph_godot::values_to_dictionary(hosted);
}

// --- Scripting API --------------------------------------------------------------------------

bool Vst3PluginEffect::is_plugin_loaded() {
	ensure_loaded();
	return hosted.has_plugin();
}

String Vst3PluginEffect::get_load_error() {
	ensure_loaded();
	return load_error;
}

// What a script sees about a class. The id is the 16-byte class id as 32 hex digits, which is
// also what plugin_id takes. VST3's subcategories ("Fx|Dynamics|Filter") take the place of CLAP's
// feature list, so both formats fill in the same "features" key.
static Dictionary describe(const VST3::Hosting::ClassInfo &p_info) {
	Dictionary d;
	d["id"] = String::utf8(p_info.ID().toString().c_str());
	d["name"] = String::utf8(p_info.name().c_str());
	d["vendor"] = String::utf8(p_info.vendor().c_str());
	d["version"] = String::utf8(p_info.version().c_str());
	d["description"] = "";
	PackedStringArray features;
	for (const std::string &sub : p_info.subCategories()) {
		features.push_back(String::utf8(sub.c_str()));
	}
	d["features"] = features;
	return d;
}

Dictionary Vst3PluginEffect::get_plugin_info() {
	ensure_loaded();
	if (!probe) {
		return Dictionary();
	}
	Dictionary d = describe(probe->get_class_info());
	// Same shape as ClapPluginEffect's "inputs"/"outputs", so scripts can treat both alike.
	auto buses = [](const std::vector<gaph::Vst3BusInfo> &p_buses) {
		Array a;
		for (const gaph::Vst3BusInfo &b : p_buses) {
			Dictionary bus;
			bus["name"] = String::utf8(b.name.c_str());
			bus["channels"] = b.channels;
			bus["main"] = b.is_main;
			a.push_back(bus);
		}
		return a;
	};
	d["inputs"] = buses(probe->get_input_buses());
	d["outputs"] = buses(probe->get_output_buses());
	return d;
}

TypedArray<Dictionary> Vst3PluginEffect::get_parameters() {
	ensure_loaded();
	return gaph_godot::parameters_to_array(hosted, probe.get());
}

void Vst3PluginEffect::set_parameter(int64_t p_id, double p_value) {
	ensure_loaded();
	hosted.set_param(uint32_t(p_id), p_value);
}

// Like set_parameter, but heard p_delay seconds from now, placed to the sample. Changes made
// with either are heard one block (~11.6 ms) after the call, evenly spaced; see block_clock.h.
void Vst3PluginEffect::set_parameter_at(int64_t p_id, double p_value, double p_delay) {
	ensure_loaded();
	hosted.set_param_at(uint32_t(p_id), p_value, int64_t(p_delay * 1e6));
}

double Vst3PluginEffect::get_parameter(int64_t p_id) {
	ensure_loaded();
	return hosted.get_param(uint32_t(p_id));
}

int64_t Vst3PluginEffect::find_parameter(const String &p_name) {
	ensure_loaded();
	return gaph_godot::find_parameter(hosted, p_name);
}

// The plugin's own display text for the current value, such as "-12.0 dB" or "Large Hall".
String Vst3PluginEffect::get_parameter_text(int64_t p_id) {
	ensure_loaded();
	return gaph_godot::parameter_text(hosted, probe.get(), p_id);
}

void Vst3PluginEffect::set_spare_instances(int p_count) {
	hosted.set_spare_count(uint32_t(p_count < 0 ? 0 : p_count));
}

int Vst3PluginEffect::get_spare_instances() const {
	return int(hosted.get_spare_count());
}

int Vst3PluginEffect::get_active_instance_count() {
	return int(hosted.active_instance_count());
}

bool Vst3PluginEffect::open_editor() {
	ensure_loaded();
	if (!hosted.has_plugin()) {
		UtilityFunctions::push_error("Vst3PluginEffect: no plugin loaded, so no editor to open");
		return false;
	}
	connect_idle(); // The window is serviced every frame.
	std::string title = "VST3 plugin";
	const Dictionary info = get_plugin_info();
	if (info.has("name")) {
		title = gaph_godot::to_std(String(info["name"])) + " (VST3)";
	}
	std::string err;
	if (!editor_window.open(hosted, title, err)) {
		UtilityFunctions::push_warning("Vst3PluginEffect: cannot open the plugin window: ", String::utf8(err.c_str()));
		return false;
	}
	return true;
}

void Vst3PluginEffect::close_editor() {
	if (editor_window.is_open()) {
		editor_window.close(hosted);
		emit_signal("editor_closed");
	}
}

bool Vst3PluginEffect::is_editor_open() const {
	return editor_window.is_open();
}

int Vst3PluginEffect::get_instance_count() {
	return int(hosted.instance_count());
}

int64_t Vst3PluginEffect::get_frames_processed() {
	return int64_t(hosted.frames_processed());
}

// Lists every VST3 effect on the system: ~/.vst3, /usr/lib/vst3 and /usr/local/lib/vst3 on
// Linux, plus any extra folders given. Bundles with a moduleinfo.json are read from it without
// loading their binary at all; the rest are loaded to ask them ("from_moduleinfo" says which).
// Instruments are left out either way; see vst3_module.cpp.
TypedArray<Dictionary> Vst3PluginEffect::scan_plugins(const PackedStringArray &p_extra_dirs) {
	std::vector<std::string> dirs;
	for (int i = 0; i < p_extra_dirs.size(); i++) {
		dirs.push_back(to_std(p_extra_dirs[i]));
	}
	for (const std::string &d : gaph_godot::search_dirs("vst3", gaph::vst3_default_search_paths())) {
		dirs.push_back(d);
	}
	TypedArray<Dictionary> out;
	for (const std::string &path : gaph::vst3_find_bundles(dirs)) {
		std::string err;
		bool from_info = false;
		std::vector<gaph::Vst3EffectEntry> found = gaph::vst3_scan_bundle(path, from_info, err);
		if (!err.empty()) {
			Dictionary d;
			d["path"] = String::utf8(path.c_str());
			d["error"] = String::utf8(err.c_str());
			out.push_back(d);
			continue;
		}
		for (const gaph::Vst3EffectEntry &e : found) {
			Dictionary d;
			d["id"] = String::utf8(e.id.c_str());
			d["name"] = String::utf8(e.name.c_str());
			d["vendor"] = String::utf8(e.vendor.c_str());
			d["version"] = String::utf8(e.version.c_str());
			d["description"] = "";
			PackedStringArray features;
			for (const std::string &sub : e.sub_categories) {
				features.push_back(String::utf8(sub.c_str()));
			}
			d["features"] = features;
			d["path"] = String::utf8(path.c_str());
			d["from_moduleinfo"] = from_info;
			out.push_back(d);
		}
	}
	return out;
}

// --- Registration ---------------------------------------------------------------------------
//
// Everything GDScript can see is declared here. D_METHOD gives the argument names that appear in
// the docs and in autocompletion. ADD_PROPERTY joins a getter/setter pair into one property,
// and the property order here is also the order Godot saves (and later loads) them in. That is
// why plugin_path comes before param_values: by the time the values arrive, the path is known.

void Vst3PluginEffect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_plugin_path", "path"), &Vst3PluginEffect::set_plugin_path);
	ClassDB::bind_method(D_METHOD("get_plugin_path"), &Vst3PluginEffect::get_plugin_path);
	ClassDB::bind_method(D_METHOD("set_plugin_id", "id"), &Vst3PluginEffect::set_plugin_id);
	ClassDB::bind_method(D_METHOD("get_plugin_id"), &Vst3PluginEffect::get_plugin_id);
	ClassDB::bind_method(D_METHOD("set_sidechain", "channel"), &Vst3PluginEffect::set_sidechain);
	ClassDB::bind_method(D_METHOD("get_sidechain"), &Vst3PluginEffect::get_sidechain);
	ClassDB::bind_method(D_METHOD("set_plugin_state", "state"), &Vst3PluginEffect::set_plugin_state);
	ClassDB::bind_method(D_METHOD("get_plugin_state"), &Vst3PluginEffect::get_plugin_state);
	ClassDB::bind_method(D_METHOD("set_param_values", "values"), &Vst3PluginEffect::set_param_values);
	ClassDB::bind_method(D_METHOD("get_param_values"), &Vst3PluginEffect::get_param_values);

	ClassDB::bind_method(D_METHOD("is_plugin_loaded"), &Vst3PluginEffect::is_plugin_loaded);
	ClassDB::bind_method(D_METHOD("get_load_error"), &Vst3PluginEffect::get_load_error);
	ClassDB::bind_method(D_METHOD("get_plugin_info"), &Vst3PluginEffect::get_plugin_info);
	ClassDB::bind_method(D_METHOD("get_parameters"), &Vst3PluginEffect::get_parameters);
	ClassDB::bind_method(D_METHOD("set_parameter", "id", "value"), &Vst3PluginEffect::set_parameter);
	ClassDB::bind_method(D_METHOD("get_parameter", "id"), &Vst3PluginEffect::get_parameter);
	ClassDB::bind_method(D_METHOD("set_parameter_at", "id", "value", "delay"), &Vst3PluginEffect::set_parameter_at);
	ClassDB::bind_method(D_METHOD("find_parameter", "name"), &Vst3PluginEffect::find_parameter);
	ClassDB::bind_method(D_METHOD("get_parameter_text", "id"), &Vst3PluginEffect::get_parameter_text);
	ClassDB::bind_method(D_METHOD("get_instance_count"), &Vst3PluginEffect::get_instance_count);
	ClassDB::bind_method(D_METHOD("open_editor"), &Vst3PluginEffect::open_editor);
	ClassDB::bind_method(D_METHOD("close_editor"), &Vst3PluginEffect::close_editor);
	ClassDB::bind_method(D_METHOD("is_editor_open"), &Vst3PluginEffect::is_editor_open);
	ClassDB::bind_method(D_METHOD("get_active_instance_count"), &Vst3PluginEffect::get_active_instance_count);
	ClassDB::bind_method(D_METHOD("set_spare_instances", "count"), &Vst3PluginEffect::set_spare_instances);
	ClassDB::bind_method(D_METHOD("get_spare_instances"), &Vst3PluginEffect::get_spare_instances);
	ClassDB::bind_method(D_METHOD("get_frames_processed"), &Vst3PluginEffect::get_frames_processed);
	// Static: called on the class, Vst3PluginEffect.scan_plugins(), without an object.
	ClassDB::bind_static_method("Vst3PluginEffect", D_METHOD("scan_plugins", "extra_dirs"), &Vst3PluginEffect::scan_plugins, DEFVAL(PackedStringArray()));

	// A .vst3 is a folder (a bundle), so the picker is a folder picker, not a file picker.
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "plugin_path", PROPERTY_HINT_GLOBAL_DIR, ""), "set_plugin_path", "get_plugin_path");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "plugin_id", PROPERTY_HINT_PLACEHOLDER_TEXT, "first effect in the module (32-hex-digit class id)"), "set_plugin_id", "get_plugin_id");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "sidechain", PROPERTY_HINT_PLACEHOLDER_TEXT, "channel of a Vst3SidechainSend"), "set_sidechain", "get_sidechain");
	// Ready-made plugin instances kept for Godot to pick up instantly when it (re)builds a bus.
	// Each costs one plugin instance of memory; with 0, a new bus waits a frame or two, playing
	// its audio unprocessed meanwhile.
	ADD_PROPERTY(PropertyInfo(Variant::INT, "spare_instances", PROPERTY_HINT_RANGE, "0,8"), "set_spare_instances", "get_spare_instances");
	// Storage only: saved to disk, but hidden in the inspector. State before values, so values
	// set by hand after the state was taken still win when the scene loads.
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "plugin_state", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_plugin_state", "get_plugin_state");
	// Storage only: saved to disk, but hidden in the inspector, which shows the sliders instead.
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "param_values", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_param_values", "get_param_values");

	// Fires when the plugin changes a parameter itself, from its own GUI for example (step 5).
	// For VST3 these values are normalised, 0..1, like every other VST3 parameter value.
	ADD_SIGNAL(MethodInfo("parameter_changed", PropertyInfo(Variant::INT, "id"), PropertyInfo(Variant::FLOAT, "value")));
	// Fires when the plugin's parameter LIST changed: names, ranges, or which parameters exist.
	ADD_SIGNAL(MethodInfo("parameters_changed"));
	// The plugin's window closed: by the user, by the plugin, or by close_editor().
	ADD_SIGNAL(MethodInfo("editor_closed"));
}

} // namespace godot
