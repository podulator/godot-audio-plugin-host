#include "vst3_instrument.h"

#include "godot_common/param_properties.h"
#include "godot_common/plugin_paths.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

using gaph_godot::to_std;

// --- Vst3InstrumentPlayback ------------------------------------------------------------------
// The sound itself is gaph_godot::InstrumentVoice (godot_common/instrument_common.h).

void Vst3InstrumentPlayback::setup(const Ref<Vst3Instrument> &p_instrument) {
	instrument = p_instrument;
	voice.setup(instrument->get_hosted());
}

Vst3InstrumentPlayback::~Vst3InstrumentPlayback() {
	voice.detach();
}

// --- Vst3Instrument: setup and loading (as Vst3PluginEffect does it) ---------------------------

// HostedEffect takes Godot's audio lock as two lambdas, so the core includes no Godot header.
Vst3Instrument::Vst3Instrument() :
		hosted(gaph::HostedEffect::AudioLock{
				[] { AudioServer::get_singleton()->lock(); },
				[] { AudioServer::get_singleton()->unlock(); } }) {
}

Vst3Instrument::~Vst3Instrument() {
	// The plugin's GUI and its window first, while the instance showing it still exists.
	editor_window.close(hosted);
	hosted.set_reference(nullptr); // before the probe it points to goes
}

void Vst3Instrument::set_plugin_path(const String &p_path) {
	if (p_path == plugin_path) {
		return;
	}
	plugin_path = p_path;
	hosted.clear_values();
	hosted.clear_state();
	load_pending = true;
	callable_mp(this, &Vst3Instrument::ensure_loaded).call_deferred();
}

void Vst3Instrument::set_plugin_id(const String &p_id) {
	if (p_id == plugin_id) {
		return;
	}
	plugin_id = p_id;
	hosted.clear_values();
	hosted.clear_state();
	load_pending = true;
	callable_mp(this, &Vst3Instrument::ensure_loaded).call_deferred();
}

void Vst3Instrument::ensure_loaded() {
	if (load_pending) {
		load_plugin();
	}
}

bool Vst3Instrument::is_plugin_loaded() {
	ensure_loaded();
	return hosted.has_plugin();
}

String Vst3Instrument::get_load_error() {
	ensure_loaded();
	return load_error;
}

void Vst3Instrument::load_plugin() {
	load_pending = false;
	load_error = "";
	hosted.set_reference(nullptr);
	probe.reset();
	auto fail = [this](const std::string &p_err) {
		load_error = String::utf8(p_err.c_str());
		UtilityFunctions::push_error("Vst3Instrument: ", plugin_path, ": ", load_error);
		hosted.set_plugin({}, {}); // every playback falls back to silence
		module.reset();
		notify_property_list_changed();
	};
	if (plugin_path.is_empty()) {
		hosted.set_plugin({}, {});
		module.reset();
		return;
	}
	const std::string resolved = gaph_godot::resolve_plugin_path(plugin_path, gaph_godot::search_dirs("vst3", gaph::vst3_default_search_paths()));
	if (resolved.empty()) {
		fail("not found in any plugin folder (Project Settings > Audio > Plugin Host > VST3)");
		return;
	}
	std::string err;
	VST3::Hosting::Module::Ptr mod = gaph::vst3_open_module(resolved, err);
	if (!mod) {
		fail(err);
		return;
	}
	// The probe: one instance with its controller, never started, for the parameter list and info.
	// `true` at the end: the module's first INSTRUMENT class, its event input switched on.
	const double rate = AudioServer::get_singleton()->get_mix_rate();
	const std::string id = to_std(plugin_id);
	probe = gaph::Vst3Processor::create(mod, id, rate, gaph::EffectRuntime::MAX_BLOCK, false, err, true);
	if (!probe) {
		fail(err);
		return;
	}
	gaph::HostedEffect::Factory factory = [mod, id, rate](std::string &r_error) -> std::unique_ptr<gaph::PluginProcessor> {
		return gaph::Vst3Processor::create(mod, id, rate, gaph::EffectRuntime::MAX_BLOCK, true, r_error, true);
	};
	hosted.set_reference(probe.get());
	err = hosted.set_plugin(factory, probe->read_params());
	if (!err.empty()) {
		UtilityFunctions::push_error("Vst3Instrument: ", String::utf8(err.c_str()));
	}
	module = mod;
	connect_idle();
	notify_property_list_changed(); // the inspector rebuilds its parameter sliders
}

// VST3 plugins need main-thread time every frame (their run loop and timers on Linux, restarts), and HostedEffect builds
// new instances and destroys old ones there too. A Resource has no _process, so the SceneTree's
// process_frame signal stands in, as for effects.
void Vst3Instrument::connect_idle() {
	if (idle_connected) {
		return;
	}
	if (SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop())) {
		tree->connect("process_frame", callable_mp(this, &Vst3Instrument::on_process_frame));
		idle_connected = true;
	}
}

void Vst3Instrument::on_process_frame() {
	const std::vector<gaph::ParamEvent> idle_changes = hosted.idle(); // changes the plugin made itself
	// After idle(), which may have destroyed the instance showing the GUI: tick() reattaches it.
	for (const gaph::ParamEvent &ev : idle_changes) {
		emit_signal("parameter_changed", int64_t(ev.param_id), ev.value);
	}
	if (editor_window.tick(hosted)) {
		emit_signal("editor_closed");
	}
	if (hosted.take_params_list_changed()) {
		notify_property_list_changed();
		emit_signal("parameters_changed");
	}
	const std::string err = hosted.take_error();
	if (!err.empty()) {
		UtilityFunctions::push_error("Vst3Instrument: ", String::utf8(err.c_str()));
	}
	input.prune();
}

// --- Playing ----------------------------------------------------------------------------------

Ref<AudioStreamPlayback> Vst3Instrument::_instantiate_playback() const {
	Vst3Instrument *self = const_cast<Vst3Instrument *>(this); // see `hosted` in the header
	self->ensure_loaded();
	Ref<Vst3InstrumentPlayback> playback;
	playback.instantiate();
	playback->setup(Ref<Vst3Instrument>(self));
	return playback;
}

int64_t Vst3Instrument::get_frames_processed() {
	return int64_t(hosted.frames_processed());
}

// --- Parameters and info ----------------------------------------------------------------------

TypedArray<Dictionary> Vst3Instrument::get_parameters() {
	ensure_loaded();
	TypedArray<Dictionary> out;
	for (const gaph::ParamInfo &p : hosted.get_params()) {
		Dictionary d;
		d["id"] = int64_t(p.id);
		d["name"] = String::utf8(p.name.c_str());
		d["min"] = p.min_value;
		d["max"] = p.max_value;
		d["default"] = p.default_value;
		out.push_back(d);
	}
	return out;
}

void Vst3Instrument::set_parameter(int64_t p_id, double p_value) {
	ensure_loaded();
	hosted.set_param(uint32_t(p_id), p_value);
}

double Vst3Instrument::get_parameter(int64_t p_id) {
	ensure_loaded();
	return hosted.get_param(uint32_t(p_id));
}

Dictionary Vst3Instrument::get_plugin_info() {
	ensure_loaded();
	Dictionary d;
	if (probe) {
		const VST3::Hosting::ClassInfo &c = probe->get_class_info();
		d["id"] = String::utf8(c.ID().toString().c_str());
		d["name"] = String::utf8(c.name().c_str());
		d["vendor"] = String::utf8(c.vendor().c_str());
		d["version"] = String::utf8(c.version().c_str());
	}
	return d;
}

// --- Saving and the inspector's sliders ------------------------------------------------------

void Vst3Instrument::set_plugin_state(const PackedByteArray &p_state) {
	gaph_godot::set_state_bytes(hosted, load_pending, p_state);
	if (!load_pending && hosted.has_plugin()) {
		notify_property_list_changed(); // a preset moves the sliders
	}
}

PackedByteArray Vst3Instrument::get_plugin_state() {
	ensure_loaded();
	return gaph_godot::get_state_bytes(hosted);
}

void Vst3Instrument::set_param_values(const Dictionary &p_values) {
	gaph_godot::values_from_dictionary(hosted, p_values);
}

Dictionary Vst3Instrument::get_param_values() {
	return gaph_godot::values_to_dictionary(hosted);
}

bool Vst3Instrument::_set(const StringName &p_name, const Variant &p_value) {
	uint32_t id;
	if (!gaph_godot::param_id_from_property(hosted, p_name, id)) {
		return false; // not a parameter: Godot falls back to the bound properties
	}
	hosted.set_param(id, double(p_value));
	return true;
}

bool Vst3Instrument::_get(const StringName &p_name, Variant &r_ret) const {
	uint32_t id;
	if (!gaph_godot::param_id_from_property(hosted, p_name, id)) {
		return false;
	}
	r_ret = hosted.get_param(id);
	return true;
}

void Vst3Instrument::_get_property_list(List<PropertyInfo> *p_list) const {
	const_cast<Vst3Instrument *>(this)->ensure_loaded(); // the inspector may ask before the deferred load ran
	gaph_godot::append_param_properties(hosted, p_list);
}

// --- The plugin window ------------------------------------------------------------------------

bool Vst3Instrument::open_editor() {
	ensure_loaded();
	if (!hosted.has_plugin()) {
		UtilityFunctions::push_error("Vst3Instrument: no plugin loaded, so no editor to open");
		return false;
	}
	connect_idle(); // the window is serviced every frame
	std::string title = "VST3 instrument";
	const Dictionary info = get_plugin_info();
	if (info.has("name")) {
		title = gaph_godot::to_std(String(info["name"])) + " (VST3)";
	}
	std::string err;
	if (!editor_window.open(hosted, title, err)) {
		UtilityFunctions::push_warning("Vst3Instrument: cannot open the plugin window: ", String::utf8(err.c_str()));
		return false;
	}
	return true;
}

void Vst3Instrument::close_editor() {
	if (editor_window.is_open()) {
		editor_window.close(hosted);
		emit_signal("editor_closed");
	}
}

bool Vst3Instrument::is_editor_open() const {
	return editor_window.is_open();
}

// --- MIDI in: the MidiDriver target methods (godot_common/instrument_common.h) ---------------

void Vst3Instrument::midi_event(int64_t p_time_us, int64_t p_source, int p_kind, int p_channel, int p_index, int64_t p_value, int64_t p_note_id) {
	ensure_loaded();
	input.event(hosted, p_time_us, p_source, p_kind, p_channel, p_index, p_value, p_note_id);
}

void Vst3Instrument::midi_stop(int64_t p_time_us, int64_t p_source, int p_mode) {
	ensure_loaded();
	input.stop(hosted, p_time_us, p_source, p_mode);
}

void Vst3Instrument::midi_transport(int64_t p_time_us, int64_t p_source, bool p_playing, double p_bpm, double p_song_beat, double p_loop_start, double p_loop_end) {
	ensure_loaded();
	input.transport(hosted, p_time_us, p_source, p_playing, p_bpm, p_song_beat, p_loop_start, p_loop_end);
}

void Vst3Instrument::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_plugin_path", "path"), &Vst3Instrument::set_plugin_path);
	ClassDB::bind_method(D_METHOD("get_plugin_path"), &Vst3Instrument::get_plugin_path);
	ClassDB::bind_method(D_METHOD("set_plugin_id", "id"), &Vst3Instrument::set_plugin_id);
	ClassDB::bind_method(D_METHOD("get_plugin_id"), &Vst3Instrument::get_plugin_id);
	ClassDB::bind_method(D_METHOD("is_plugin_loaded"), &Vst3Instrument::is_plugin_loaded);
	ClassDB::bind_method(D_METHOD("get_load_error"), &Vst3Instrument::get_load_error);
	ClassDB::bind_method(D_METHOD("get_parameters"), &Vst3Instrument::get_parameters);
	ClassDB::bind_method(D_METHOD("set_parameter", "id", "value"), &Vst3Instrument::set_parameter);
	ClassDB::bind_method(D_METHOD("get_parameter", "id"), &Vst3Instrument::get_parameter);
	ClassDB::bind_method(D_METHOD("get_plugin_info"), &Vst3Instrument::get_plugin_info);
	ClassDB::bind_method(D_METHOD("get_frames_processed"), &Vst3Instrument::get_frames_processed);
	ClassDB::bind_method(D_METHOD("midi_event", "time_us", "source", "kind", "channel", "index", "value", "note_id"), &Vst3Instrument::midi_event);
	ClassDB::bind_method(D_METHOD("midi_stop", "time_us", "source", "mode"), &Vst3Instrument::midi_stop);
	ClassDB::bind_method(D_METHOD("midi_transport", "time_us", "source", "playing", "bpm", "song_beat", "loop_start", "loop_end"), &Vst3Instrument::midi_transport);
	ClassDB::bind_method(D_METHOD("open_editor"), &Vst3Instrument::open_editor);
	ClassDB::bind_method(D_METHOD("close_editor"), &Vst3Instrument::close_editor);
	ClassDB::bind_method(D_METHOD("is_editor_open"), &Vst3Instrument::is_editor_open);
	ADD_SIGNAL(MethodInfo("editor_closed"));
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "plugin_path", PROPERTY_HINT_PLACEHOLDER_TEXT, "a .vst3 bundle"), "set_plugin_path", "get_plugin_path");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "plugin_id", PROPERTY_HINT_PLACEHOLDER_TEXT, "first plugin in the file"), "set_plugin_id", "get_plugin_id");
	// After the plugin: Godot sets properties in this order when it loads a scene. State before
	// values, so values set by hand after the state was taken still win.
	ClassDB::bind_method(D_METHOD("set_plugin_state", "state"), &Vst3Instrument::set_plugin_state);
	ClassDB::bind_method(D_METHOD("get_plugin_state"), &Vst3Instrument::get_plugin_state);
	ClassDB::bind_method(D_METHOD("set_param_values", "values"), &Vst3Instrument::set_param_values);
	ClassDB::bind_method(D_METHOD("get_param_values"), &Vst3Instrument::get_param_values);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "plugin_state", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_plugin_state", "get_plugin_state");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "param_values", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_param_values", "get_param_values");
	ADD_SIGNAL(MethodInfo("parameter_changed", PropertyInfo(Variant::INT, "id"), PropertyInfo(Variant::FLOAT, "value")));
	ADD_SIGNAL(MethodInfo("parameters_changed"));
}

} // namespace godot
