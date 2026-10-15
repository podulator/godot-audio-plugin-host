#include "clap_plugin_effect.h"
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

// --- ClapPluginEffectInstance ---------------------------------------------------------------

// Both of these run on the main thread WITH THE AUDIO LOCK HELD (see effect_runtime.h), so they
// do nothing slow: no plugin is loaded, created or destroyed here. A plugin still waiting for
// its deferred load reaches this runtime later, through set_plugin(); see HostedEffect::attach.
void ClapPluginEffectInstance::setup(const Ref<ClapPluginEffect> &p_effect) {
	effect = p_effect;
	runtime.set_sample_rate(AudioServer::get_singleton()->get_mix_rate());
	effect->get_hosted().attach(runtime);
}

ClapPluginEffectInstance::~ClapPluginEffectInstance() {
	// After this, the runtime member is destroyed, and with it the plugin instance.
	if (effect.is_valid()) {
		effect->get_hosted().detach(runtime);
	}
}

// AUDIO THREAD. Godot's AudioFrame is {float left; float right;} with no padding, so an array of
// them has exactly the layout of interleaved floats, and reinterpret_cast reads it as one.
void ClapPluginEffectInstance::_process(const void *p_src_buffer, AudioFrame *p_dst_buffer, int32_t p_frame_count) {
	runtime.process(static_cast<const float *>(p_src_buffer), reinterpret_cast<float *>(p_dst_buffer), uint32_t(p_frame_count));
}

// AUDIO THREAD. Godot skips effects on a silent bus unless this returns true. With a plugin
// loaded we keep running, so a reverb or delay tail rings out after the sound that fed it stops.
bool ClapPluginEffectInstance::_process_silence() const {
	return runtime.has_processor();
}

// --- ClapPluginEffect: lifecycle and loading ------------------------------------------------

// HostedEffect gets the audio lock as two lambdas, so the core never includes a Godot header.
// AudioServer's lock is recursive (Godot's Mutex wraps std::recursive_mutex), so taking it
// again while Godot already holds it, inside _instantiate, is safe.
ClapPluginEffect::ClapPluginEffect() :
		hosted(gaph::HostedEffect::AudioLock{
				[] { AudioServer::get_singleton()->lock(); },
				[] { AudioServer::get_singleton()->unlock(); } }) {
}

ClapPluginEffect::~ClapPluginEffect() {
	// The plugin's GUI and its window go first, explicitly: members are destroyed in reverse
	// order, and the GUI must close while its window and its plugin instance still exist.
	editor_window.close(hosted);
	hosted.set_reference(nullptr);
}

Ref<AudioEffectInstance> ClapPluginEffect::_instantiate() {
	Ref<ClapPluginEffectInstance> instance;
	instance.instantiate(); // Creates the object, like `ClapPluginEffectInstance.new()`.
	instance->setup(Ref<ClapPluginEffect>(this));
	return instance;
}

// Setting plugin_path or plugin_id only marks the plugin as needing a (re)load. The load itself
// happens on the next idle frame (call_deferred), or sooner if something needs the plugin first.
// This way a saved scene, which sets both properties in turn, loads the plugin once, not twice.
// A changed plugin also clears the stored values: CLAP ids are per plugin, so parameter 3 of
// one reverb has nothing to do with parameter 3 of another.
void ClapPluginEffect::set_plugin_path(const String &p_path) {
	if (p_path == plugin_path) {
		return;
	}
	plugin_path = p_path;
	hosted.clear_values();
	hosted.clear_state();
	load_pending = true;
	callable_mp(this, &ClapPluginEffect::ensure_loaded).call_deferred();
}

String ClapPluginEffect::get_plugin_path() const {
	return plugin_path;
}

void ClapPluginEffect::set_plugin_id(const String &p_id) {
	if (p_id == plugin_id) {
		return;
	}
	plugin_id = p_id;
	hosted.clear_values();
	hosted.clear_state();
	load_pending = true;
	callable_mp(this, &ClapPluginEffect::ensure_loaded).call_deferred();
}

String ClapPluginEffect::get_plugin_id() const {
	return plugin_id;
}

// The name of a sidechain channel, as set on a ClapSidechainSend; empty for none. Applied to
// running instances immediately (under the audio lock, inside HostedEffect).
void ClapPluginEffect::set_sidechain(const String &p_channel) {
	sidechain_name = p_channel;
	hosted.set_sidechain(gaph::sidechain_channel(gaph_godot::to_std(p_channel)));
}

String ClapPluginEffect::get_sidechain() const {
	return sidechain_name;
}

// plugin_state: the plugin's own saved state, stored in the scene file next to param_values.
// The getter asks the plugin, so the bytes are always current. When Godot loads a saved scene,
// the setter runs before the deferred plugin load, so the bytes are kept and applied as the
// plugin comes up (HostedEffect::set_plugin). That also keeps all plugin calls on the main
// thread, even if the scene itself was loaded on a worker thread.
void ClapPluginEffect::set_plugin_state(const PackedByteArray &p_state) {
	std::vector<uint8_t> bytes(p_state.ptr(), p_state.ptr() + p_state.size());
	if (load_pending || !hosted.has_plugin()) {
		hosted.set_pending_state(std::move(bytes));
	} else {
		hosted.load_state(bytes);
		notify_property_list_changed(); // A preset moves the sliders.
	}
}

PackedByteArray ClapPluginEffect::get_plugin_state() {
	ensure_loaded();
	std::vector<uint8_t> bytes;
	PackedByteArray out;
	if (hosted.save_state(bytes)) {
		out.resize(int64_t(bytes.size()));
		std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	}
	return out;
}

void ClapPluginEffect::ensure_loaded() {
	if (load_pending) {
		load_plugin();
	}
}

void ClapPluginEffect::load_plugin() {
	load_pending = false;
	load_error = "";
	// HostedEffect holds a raw pointer to the probe: clear it before the probe goes away.
	hosted.set_reference(nullptr);
	probe.reset();

	auto fail = [this](const std::string &p_err) {
		load_error = String::utf8(p_err.c_str());
		UtilityFunctions::push_error("ClapPluginEffect: ", plugin_path, ": ", load_error);
		hosted.set_plugin({}, {}); // Every instance falls back to passthrough.
		library.reset();
		notify_property_list_changed();
	};

	if (plugin_path.is_empty()) {
		hosted.set_plugin({}, {});
		library.reset();
		notify_property_list_changed();
		return;
	}

	// plugin_path may be absolute, res://, or relative to a search folder (what the Add Effect
	// menu stores, so scenes work on other machines): godot_common/plugin_paths.h.
	const std::string resolved = gaph_godot::resolve_plugin_path(plugin_path, gaph_godot::search_dirs("clap", gaph::clap_default_search_paths()));
	if (resolved.empty()) {
		fail("not found in any plugin folder (Project Settings > Audio > Plugin Host > CLAP)");
		return;
	}
	const String real_path = gaph_godot::to_godot(resolved);

	std::string err;
	std::shared_ptr<gaph::ClapLibrary> lib = gaph::ClapLibrary::open(to_std(real_path), err);
	if (!lib) {
		fail(err);
		return;
	}

	// The probe: one instance, initialised but never activated, used to read the parameter list
	// and turn values into text for the inspector. It works even when the effect is on no bus.
	const double rate = AudioServer::get_singleton()->get_mix_rate();
	const std::string id = to_std(plugin_id);
	probe = gaph::ClapProcessor::create(lib, id, rate, gaph::EffectRuntime::MAX_BLOCK, false, err);
	if (!probe) {
		fail(err);
		return;
	}

	// [lib, id, rate] is the capture list: the lambda keeps its own copies of those three, so
	// it stays valid for as long as HostedEffect keeps it, whatever happens to these locals.
	// Copying `lib`, a shared_ptr, also keeps the library loaded for as long as the factory exists.
	gaph::HostedEffect::Factory factory = [lib, id, rate](std::string &r_error) -> std::unique_ptr<gaph::PluginProcessor> {
		return gaph::ClapProcessor::create(lib, id, rate, gaph::EffectRuntime::MAX_BLOCK, true, r_error);
	};
	hosted.set_reference(probe.get());
	err = hosted.set_plugin(factory, probe->read_params());
	if (!err.empty()) {
		UtilityFunctions::push_error("ClapPluginEffect: ", String::utf8(err.c_str()));
	}
	library = lib;
	connect_idle();
	notify_property_list_changed(); // Tells the inspector to rebuild the parameter sliders.
}

// CLAP plugins need regular main-thread time (on_main_thread, restarts), and we need to collect
// the parameter changes they report. A Resource has no _process() of its own, so we borrow the
// SceneTree's process_frame signal, which fires once per frame, in the editor too.
void ClapPluginEffect::connect_idle() {
	if (idle_connected) {
		return;
	}
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (!tree) {
		return; // Too early in startup; the next load_plugin() tries again.
	}
	tree->connect("process_frame", callable_mp(this, &ClapPluginEffect::on_process_frame));
	idle_connected = true;
}

void ClapPluginEffect::on_process_frame() {
	const std::vector<gaph::ParamEvent> changes = hosted.idle();
	const std::string err = hosted.take_error();
	if (!err.empty()) {
		UtilityFunctions::push_error("ClapPluginEffect: ", String::utf8(err.c_str()));
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

bool ClapPluginEffect::_set(const StringName &p_name, const Variant &p_value) {
	uint32_t id;
	if (!gaph_godot::param_id_from_property(hosted, p_name, id)) {
		return false; // Not ours: Godot falls back to the normal, bound properties.
	}
	hosted.set_param(id, double(p_value));
	return true;
}

bool ClapPluginEffect::_get(const StringName &p_name, Variant &r_ret) const {
	uint32_t id;
	if (!gaph_godot::param_id_from_property(hosted, p_name, id)) {
		return false;
	}
	r_ret = hosted.get_param(id);
	return true;
}

void ClapPluginEffect::_get_property_list(List<PropertyInfo> *p_list) const {
	// The inspector may ask before the deferred load has run. const_cast removes `const` from
	// `this`, which is allowed because loading only fills caches; nothing visible to the caller
	// changes.
	const_cast<ClapPluginEffect *>(this)->ensure_loaded();
	gaph_godot::append_param_properties(hosted, p_list);
}

void ClapPluginEffect::set_param_values(const Dictionary &p_values) {
	gaph_godot::values_from_dictionary(hosted, p_values);
}

Dictionary ClapPluginEffect::get_param_values() {
	return gaph_godot::values_to_dictionary(hosted);
}

// --- Scripting API --------------------------------------------------------------------------

bool ClapPluginEffect::is_plugin_loaded() {
	ensure_loaded();
	return hosted.has_plugin();
}

String ClapPluginEffect::get_load_error() {
	ensure_loaded();
	return load_error;
}

static PackedStringArray features_of(const clap_plugin_descriptor_t *p_desc) {
	// `features` is a C array of strings, ended by a nullptr entry.
	PackedStringArray out;
	for (const char *const *f = p_desc->features; f && *f; f++) {
		out.push_back(String::utf8(*f));
	}
	return out;
}

static Dictionary describe(const clap_plugin_descriptor_t *p_desc) {
	Dictionary d;
	d["id"] = String::utf8(p_desc->id);
	d["name"] = String::utf8(p_desc->name ? p_desc->name : "");
	d["vendor"] = String::utf8(p_desc->vendor ? p_desc->vendor : "");
	d["version"] = String::utf8(p_desc->version ? p_desc->version : "");
	d["description"] = String::utf8(p_desc->description ? p_desc->description : "");
	d["features"] = features_of(p_desc);
	return d;
}

Dictionary ClapPluginEffect::get_plugin_info() {
	ensure_loaded();
	if (!probe) {
		return Dictionary();
	}
	Dictionary d = describe(probe->get_descriptor());
	auto ports = [](const std::vector<gaph::ClapPortInfo> &p_ports) {
		Array a;
		for (const gaph::ClapPortInfo &p : p_ports) {
			Dictionary port;
			port["name"] = String::utf8(p.name.c_str());
			port["channels"] = p.channels;
			port["main"] = p.is_main;
			a.push_back(port);
		}
		return a;
	};
	d["inputs"] = ports(probe->get_input_ports());
	d["outputs"] = ports(probe->get_output_ports());
	return d;
}

TypedArray<Dictionary> ClapPluginEffect::get_parameters() {
	ensure_loaded();
	return gaph_godot::parameters_to_array(hosted, probe.get());
}

void ClapPluginEffect::set_parameter(int64_t p_id, double p_value) {
	ensure_loaded();
	hosted.set_param(uint32_t(p_id), p_value);
}

// Like set_parameter, but heard p_delay seconds from now, placed to the sample. Changes made
// with either are heard one block (~11.6 ms) after the call, evenly spaced; see block_clock.h.
void ClapPluginEffect::set_parameter_at(int64_t p_id, double p_value, double p_delay) {
	ensure_loaded();
	hosted.set_param_at(uint32_t(p_id), p_value, int64_t(p_delay * 1e6));
}

double ClapPluginEffect::get_parameter(int64_t p_id) {
	ensure_loaded();
	return hosted.get_param(uint32_t(p_id));
}

int64_t ClapPluginEffect::find_parameter(const String &p_name) {
	ensure_loaded();
	return gaph_godot::find_parameter(hosted, p_name);
}

// The plugin's own display text for the current value, such as "-12.0 dB" or "Large Hall".
String ClapPluginEffect::get_parameter_text(int64_t p_id) {
	ensure_loaded();
	return gaph_godot::parameter_text(hosted, probe.get(), p_id);
}

void ClapPluginEffect::set_spare_instances(int p_count) {
	hosted.set_spare_count(uint32_t(p_count < 0 ? 0 : p_count));
}

int ClapPluginEffect::get_spare_instances() const {
	return int(hosted.get_spare_count());
}

int ClapPluginEffect::get_active_instance_count() {
	return int(hosted.active_instance_count());
}

bool ClapPluginEffect::open_editor() {
	ensure_loaded();
	if (!hosted.has_plugin()) {
		UtilityFunctions::push_error("ClapPluginEffect: no plugin loaded, so no editor to open");
		return false;
	}
	connect_idle(); // The window is serviced every frame.
	std::string title = "CLAP plugin";
	const Dictionary info = get_plugin_info();
	if (info.has("name")) {
		title = gaph_godot::to_std(String(info["name"])) + " (CLAP)";
	}
	std::string err;
	if (!editor_window.open(hosted, title, err)) {
		UtilityFunctions::push_warning("ClapPluginEffect: cannot open the plugin window: ", String::utf8(err.c_str()));
		return false;
	}
	return true;
}

void ClapPluginEffect::close_editor() {
	if (editor_window.is_open()) {
		editor_window.close(hosted);
		emit_signal("editor_closed");
	}
}

bool ClapPluginEffect::is_editor_open() const {
	return editor_window.is_open();
}

int ClapPluginEffect::get_instance_count() {
	return int(hosted.instance_count());
}

int64_t ClapPluginEffect::get_frames_processed() {
	return int64_t(hosted.frames_processed());
}

// Lists every CLAP plugin on the system. CLAP has no metadata files, so this loads each binary
// to read its descriptors, then unloads it again when `lib` goes out of scope. That can take a
// while with many plugins installed; call it from a menu, not every frame.
TypedArray<Dictionary> ClapPluginEffect::scan_plugins(const PackedStringArray &p_extra_dirs) {
	std::vector<std::string> dirs;
	for (int i = 0; i < p_extra_dirs.size(); i++) {
		dirs.push_back(to_std(p_extra_dirs[i]));
	}
	for (const std::string &d : gaph_godot::search_dirs("clap", gaph::clap_default_search_paths())) {
		dirs.push_back(d);
	}
	TypedArray<Dictionary> out;
	for (const std::string &path : gaph::clap_find_bundles(dirs)) {
		std::string err;
		std::shared_ptr<gaph::ClapLibrary> lib = gaph::ClapLibrary::open(path, err);
		if (!lib) {
			Dictionary d;
			d["path"] = String::utf8(path.c_str());
			d["error"] = String::utf8(err.c_str());
			out.push_back(d);
			continue;
		}
		for (const clap_plugin_descriptor_t *desc : lib->get_descriptors()) {
			Dictionary d = describe(desc);
			d["path"] = String::utf8(path.c_str());
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

void ClapPluginEffect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_plugin_path", "path"), &ClapPluginEffect::set_plugin_path);
	ClassDB::bind_method(D_METHOD("get_plugin_path"), &ClapPluginEffect::get_plugin_path);
	ClassDB::bind_method(D_METHOD("set_plugin_id", "id"), &ClapPluginEffect::set_plugin_id);
	ClassDB::bind_method(D_METHOD("get_plugin_id"), &ClapPluginEffect::get_plugin_id);
	ClassDB::bind_method(D_METHOD("set_sidechain", "channel"), &ClapPluginEffect::set_sidechain);
	ClassDB::bind_method(D_METHOD("get_sidechain"), &ClapPluginEffect::get_sidechain);
	ClassDB::bind_method(D_METHOD("set_plugin_state", "state"), &ClapPluginEffect::set_plugin_state);
	ClassDB::bind_method(D_METHOD("get_plugin_state"), &ClapPluginEffect::get_plugin_state);
	ClassDB::bind_method(D_METHOD("set_param_values", "values"), &ClapPluginEffect::set_param_values);
	ClassDB::bind_method(D_METHOD("get_param_values"), &ClapPluginEffect::get_param_values);

	ClassDB::bind_method(D_METHOD("is_plugin_loaded"), &ClapPluginEffect::is_plugin_loaded);
	ClassDB::bind_method(D_METHOD("get_load_error"), &ClapPluginEffect::get_load_error);
	ClassDB::bind_method(D_METHOD("get_plugin_info"), &ClapPluginEffect::get_plugin_info);
	ClassDB::bind_method(D_METHOD("get_parameters"), &ClapPluginEffect::get_parameters);
	ClassDB::bind_method(D_METHOD("set_parameter", "id", "value"), &ClapPluginEffect::set_parameter);
	ClassDB::bind_method(D_METHOD("get_parameter", "id"), &ClapPluginEffect::get_parameter);
	ClassDB::bind_method(D_METHOD("set_parameter_at", "id", "value", "delay"), &ClapPluginEffect::set_parameter_at);
	ClassDB::bind_method(D_METHOD("find_parameter", "name"), &ClapPluginEffect::find_parameter);
	ClassDB::bind_method(D_METHOD("get_parameter_text", "id"), &ClapPluginEffect::get_parameter_text);
	ClassDB::bind_method(D_METHOD("get_instance_count"), &ClapPluginEffect::get_instance_count);
	ClassDB::bind_method(D_METHOD("open_editor"), &ClapPluginEffect::open_editor);
	ClassDB::bind_method(D_METHOD("close_editor"), &ClapPluginEffect::close_editor);
	ClassDB::bind_method(D_METHOD("is_editor_open"), &ClapPluginEffect::is_editor_open);
	ClassDB::bind_method(D_METHOD("get_active_instance_count"), &ClapPluginEffect::get_active_instance_count);
	ClassDB::bind_method(D_METHOD("set_spare_instances", "count"), &ClapPluginEffect::set_spare_instances);
	ClassDB::bind_method(D_METHOD("get_spare_instances"), &ClapPluginEffect::get_spare_instances);
	ClassDB::bind_method(D_METHOD("get_frames_processed"), &ClapPluginEffect::get_frames_processed);
	// Static: called on the class, ClapPluginEffect.scan_plugins(), without an object.
	ClassDB::bind_static_method("ClapPluginEffect", D_METHOD("scan_plugins", "extra_dirs"), &ClapPluginEffect::scan_plugins, DEFVAL(PackedStringArray()));

	// PROPERTY_HINT_GLOBAL_FILE gives a file picker for paths outside the project, filtered to
	// .clap files.
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "plugin_path", PROPERTY_HINT_GLOBAL_FILE, "*.clap"), "set_plugin_path", "get_plugin_path");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "plugin_id", PROPERTY_HINT_PLACEHOLDER_TEXT, "first plugin in the file"), "set_plugin_id", "get_plugin_id");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "sidechain", PROPERTY_HINT_PLACEHOLDER_TEXT, "channel of a ClapSidechainSend"), "set_sidechain", "get_sidechain");
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
	ADD_SIGNAL(MethodInfo("parameter_changed", PropertyInfo(Variant::INT, "id"), PropertyInfo(Variant::FLOAT, "value")));
	// Fires when the plugin's parameter LIST changed: names, ranges, or which parameters exist.
	ADD_SIGNAL(MethodInfo("parameters_changed"));
	// The plugin's window closed: by the user, by the plugin, or by close_editor().
	ADD_SIGNAL(MethodInfo("editor_closed"));
}

} // namespace godot
