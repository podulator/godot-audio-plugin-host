#include "clap_instrument.h"

#include "godot_common/param_properties.h"
#include "godot_common/plugin_paths.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

using gaph_godot::to_std;

// --- ClapInstrumentPlayback ------------------------------------------------------------------
// The sound is the instrument's, shared by all its playbacks (godot_common/shared_voice.h).

void ClapInstrumentPlayback::setup(const Ref<ClapInstrument> &p_instrument, uint32_t p_output) {
	instrument = p_instrument;
	output = p_output;
	instrument->get_voice().join(instrument->get_hosted(), tap, instrument->output_count());
}

ClapInstrumentPlayback::~ClapInstrumentPlayback() {
	if (instrument.is_valid()) {
		instrument->get_voice().leave();
	}
}

void ClapInstrumentPlayback::_start(double) {
	playing = true;
}

void ClapInstrumentPlayback::_stop() {
	playing = false;
}

bool ClapInstrumentPlayback::_is_playing() const {
	return playing;
}

int32_t ClapInstrumentPlayback::_mix(AudioFrame *p_buffer, float, int32_t p_frames) {
	return instrument->get_voice().mix(tap, p_buffer, p_frames, output);
}

// --- ClapInstrument: setup and loading (as ClapPluginEffect does it) ---------------------------

// HostedEffect takes Godot's audio lock as two lambdas, so the core includes no Godot header.
ClapInstrument::ClapInstrument() :
		hosted(gaph::HostedEffect::AudioLock{
				[] { AudioServer::get_singleton()->lock(); },
				[] { AudioServer::get_singleton()->unlock(); } }) {
}

ClapInstrument::~ClapInstrument() {
	// The plugin's GUI and its window first, while the instance showing it still exists.
	editor_window.close(hosted);
	hosted.set_reference(nullptr); // before the probe it points to goes
}

void ClapInstrument::set_plugin_path(const String &p_path) {
	if (p_path == plugin_path) {
		return;
	}
	plugin_path = p_path;
	hosted.clear_values();
	hosted.clear_state();
	load_pending = true;
	callable_mp(this, &ClapInstrument::ensure_loaded).call_deferred();
}

void ClapInstrument::set_plugin_id(const String &p_id) {
	if (p_id == plugin_id) {
		return;
	}
	plugin_id = p_id;
	hosted.clear_values();
	hosted.clear_state();
	load_pending = true;
	callable_mp(this, &ClapInstrument::ensure_loaded).call_deferred();
}

void ClapInstrument::ensure_loaded() {
	if (load_pending) {
		load_plugin();
	}
}

bool ClapInstrument::is_plugin_loaded() {
	ensure_loaded();
	return hosted.has_plugin();
}

String ClapInstrument::get_load_error() {
	ensure_loaded();
	return load_error;
}

String ClapInstrument::_get_stream_name() const {
	return plugin_path.get_file();
}

void ClapInstrument::load_plugin() {
	load_pending = false;
	load_error = "";
	hosted.set_reference(nullptr);
	probe.reset();
	auto fail = [this](const std::string &p_err) {
		load_error = String::utf8(p_err.c_str());
		UtilityFunctions::push_error("ClapInstrument: ", plugin_path, ": ", load_error);
		hosted.set_plugin({}, {}); // every playback falls back to silence
		library.reset();
		notify_property_list_changed();
	};
	if (plugin_path.is_empty()) {
		hosted.set_plugin({}, {});
		library.reset();
		return;
	}
	const std::string resolved = gaph_godot::resolve_plugin_path(plugin_path, gaph_godot::search_dirs("clap", gaph::clap_default_search_paths()));
	if (resolved.empty()) {
		fail("not found in any plugin folder (Project Settings > Audio > Plugin Host > CLAP)");
		return;
	}
	std::string err;
	std::shared_ptr<gaph::ClapLibrary> lib = gaph::ClapLibrary::open(resolved, err);
	if (!lib) {
		fail(err);
		return;
	}
	// The probe: one instance, never activated, for the parameter list and plugin info.
	const double rate = AudioServer::get_singleton()->get_mix_rate();
	const std::string id = to_std(plugin_id);
	probe = gaph::ClapProcessor::create(lib, id, rate, gaph::EffectRuntime::MAX_BLOCK, false, err);
	if (!probe) {
		fail(err);
		return;
	}
	gaph::HostedEffect::Factory factory = [lib, id, rate](std::string &r_error) -> std::unique_ptr<gaph::PluginProcessor> {
		return gaph::ClapProcessor::create(lib, id, rate, gaph::EffectRuntime::MAX_BLOCK, true, r_error);
	};
	hosted.set_reference(probe.get());
	output_names.clear();
	for (const std::string &name : probe->output_names()) {
		output_names.push_back(String::utf8(name.c_str()));
	}
	err = hosted.set_plugin(factory, probe->read_params());
	if (!err.empty()) {
		UtilityFunctions::push_error("ClapInstrument: ", String::utf8(err.c_str()));
	}
	library = lib;
	connect_idle();
	notify_property_list_changed(); // the inspector rebuilds its parameter sliders
}

// CLAP plugins need main-thread time every frame (callbacks, restarts), and HostedEffect builds
// new instances and destroys old ones there too. A Resource has no _process, so the SceneTree's
// process_frame signal stands in, as for effects.
void ClapInstrument::connect_idle() {
	if (idle_connected) {
		return;
	}
	if (SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop())) {
		tree->connect("process_frame", callable_mp(this, &ClapInstrument::on_process_frame));
		idle_connected = true;
	}
}

void ClapInstrument::on_process_frame() {
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
		UtilityFunctions::push_error("ClapInstrument: ", String::utf8(err.c_str()));
	}
	input.prune();
}

// --- Playing ----------------------------------------------------------------------------------

Ref<AudioStreamPlayback> ClapInstrument::_instantiate_playback() const {
	ClapInstrument *self = const_cast<ClapInstrument *>(this); // see `hosted` in the header
	self->ensure_loaded();
	Ref<ClapInstrumentPlayback> playback;
	playback.instantiate();
	playback->setup(Ref<ClapInstrument>(self));
	return playback;
}

bool ClapInstrument::is_plugin_sleeping() {
	return hosted.is_sleeping();
}

int64_t ClapInstrument::get_frames_processed() {
	return int64_t(hosted.frames_processed());
}

// --- Parameters and info ----------------------------------------------------------------------

TypedArray<Dictionary> ClapInstrument::get_parameters() {
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

void ClapInstrument::set_parameter(int64_t p_id, double p_value) {
	ensure_loaded();
	hosted.set_param(uint32_t(p_id), p_value);
}

double ClapInstrument::get_parameter(int64_t p_id) {
	ensure_loaded();
	return hosted.get_param(uint32_t(p_id));
}

Dictionary ClapInstrument::get_plugin_info() {
	ensure_loaded();
	Dictionary d;
	if (probe) {
		const clap_plugin_descriptor_t *desc = probe->get_descriptor();
		if (desc) {
			d["id"] = String::utf8(desc->id);
			d["name"] = String::utf8(desc->name ? desc->name : "");
			d["vendor"] = String::utf8(desc->vendor ? desc->vendor : "");
			d["version"] = String::utf8(desc->version ? desc->version : "");
		}
	}
	return d;
}

// --- Saving and the inspector's sliders ------------------------------------------------------

void ClapInstrument::set_plugin_state(const PackedByteArray &p_state) {
	gaph_godot::set_state_bytes(hosted, load_pending, p_state);
	if (!load_pending && hosted.has_plugin()) {
		notify_property_list_changed(); // a preset moves the sliders
	}
}

PackedByteArray ClapInstrument::get_plugin_state() {
	ensure_loaded();
	return gaph_godot::get_state_bytes(hosted);
}

void ClapInstrument::set_param_values(const Dictionary &p_values) {
	gaph_godot::values_from_dictionary(hosted, p_values);
}

Dictionary ClapInstrument::get_param_values() {
	return gaph_godot::values_to_dictionary(hosted);
}

bool ClapInstrument::_set(const StringName &p_name, const Variant &p_value) {
	uint32_t id;
	if (!gaph_godot::param_id_from_property(hosted, p_name, id)) {
		return false; // not a parameter: Godot falls back to the bound properties
	}
	hosted.set_param(id, double(p_value));
	return true;
}

bool ClapInstrument::_get(const StringName &p_name, Variant &r_ret) const {
	uint32_t id;
	if (!gaph_godot::param_id_from_property(hosted, p_name, id)) {
		return false;
	}
	r_ret = hosted.get_param(id);
	return true;
}

void ClapInstrument::_get_property_list(List<PropertyInfo> *p_list) const {
	const_cast<ClapInstrument *>(this)->ensure_loaded(); // the inspector may ask before the deferred load ran
	gaph_godot::append_param_properties(hosted, p_list);
}

// --- Outputs ----------------------------------------------------------------------------------

int ClapInstrument::get_output_count() {
	ensure_loaded();
	return int(output_count());
}

PackedStringArray ClapInstrument::get_output_names() {
	ensure_loaded();
	return output_names;
}

// Output 0 is the instrument itself; any other is a small stream that plays that output. A new
// one each call: it only refers to the instrument, and all of them read the one shared voice.
Ref<AudioStream> ClapInstrument::get_output(int p_index) {
	ensure_loaded();
	ERR_FAIL_COND_V_MSG(p_index < 0 || p_index >= int(output_count()), Ref<AudioStream>(),
			vformat("ClapInstrument: output %d does not exist; the plugin has %d (get_output_names()).", p_index, int(output_count())));
	if (p_index == 0) {
		return Ref<AudioStream>(this);
	}
	Ref<ClapInstrumentOutput> out;
	out.instantiate();
	out->setup(Ref<ClapInstrument>(this), p_index);
	return out;
}

// Output i to player i (AudioStreamPlayer, 2D or 3D: anything with a `stream` and play()), and
// starts each. Extra players beyond the plugin's outputs are refused.
void ClapInstrument::bind_outputs(const Array &p_players) {
	ensure_loaded();
	for (int i = 0; i < p_players.size(); i++) {
		Object *player = p_players[i];
		ERR_CONTINUE_MSG(!player || !player->has_method("play"), vformat("ClapInstrument: bind_outputs() item %d is not a player.", i));
		Ref<AudioStream> stream = get_output(i);
		if (stream.is_null()) {
			continue;
		}
		player->set("stream", stream);
		player->call("play");
	}
}

// --- The plugin window ------------------------------------------------------------------------

bool ClapInstrument::open_editor() {
	ensure_loaded();
	if (!hosted.has_plugin()) {
		UtilityFunctions::push_error("ClapInstrument: no plugin loaded, so no editor to open");
		return false;
	}
	connect_idle(); // the window is serviced every frame
	std::string title = "CLAP instrument";
	const Dictionary info = get_plugin_info();
	if (info.has("name")) {
		title = gaph_godot::to_std(String(info["name"])) + " (CLAP)";
	}
	std::string err;
	if (!editor_window.open(hosted, title, err)) {
		UtilityFunctions::push_warning("ClapInstrument: cannot open the plugin window: ", String::utf8(err.c_str()));
		return false;
	}
	return true;
}

void ClapInstrument::close_editor() {
	if (editor_window.is_open()) {
		editor_window.close(hosted);
		emit_signal("editor_closed");
	}
}

bool ClapInstrument::is_editor_open() const {
	return editor_window.is_open();
}

// --- MIDI in: the MidiDriver target methods (godot_common/instrument_common.h) ---------------

void ClapInstrument::midi_event(int64_t p_time_us, int64_t p_source, int p_kind, int p_channel, int p_index, int64_t p_value, int64_t p_note_id) {
	ensure_loaded();
	input.event(hosted, p_time_us, p_source, p_kind, p_channel, p_index, p_value, p_note_id);
}

void ClapInstrument::midi_stop(int64_t p_time_us, int64_t p_source, int p_mode) {
	ensure_loaded();
	input.stop(hosted, p_time_us, p_source, p_mode);
}

void ClapInstrument::midi_transport(int64_t p_time_us, int64_t p_source, bool p_playing, double p_bpm, double p_song_beat, double p_loop_start, double p_loop_end) {
	ensure_loaded();
	input.transport(hosted, p_time_us, p_source, p_playing, p_bpm, p_song_beat, p_loop_start, p_loop_end);
}

void ClapInstrument::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_plugin_path", "path"), &ClapInstrument::set_plugin_path);
	ClassDB::bind_method(D_METHOD("get_plugin_path"), &ClapInstrument::get_plugin_path);
	ClassDB::bind_method(D_METHOD("set_plugin_id", "id"), &ClapInstrument::set_plugin_id);
	ClassDB::bind_method(D_METHOD("get_plugin_id"), &ClapInstrument::get_plugin_id);
	ClassDB::bind_method(D_METHOD("is_plugin_loaded"), &ClapInstrument::is_plugin_loaded);
	ClassDB::bind_method(D_METHOD("get_load_error"), &ClapInstrument::get_load_error);
	ClassDB::bind_method(D_METHOD("get_parameters"), &ClapInstrument::get_parameters);
	ClassDB::bind_method(D_METHOD("set_parameter", "id", "value"), &ClapInstrument::set_parameter);
	ClassDB::bind_method(D_METHOD("get_parameter", "id"), &ClapInstrument::get_parameter);
	ClassDB::bind_method(D_METHOD("get_plugin_info"), &ClapInstrument::get_plugin_info);
	ClassDB::bind_method(D_METHOD("get_frames_processed"), &ClapInstrument::get_frames_processed);
	ClassDB::bind_method(D_METHOD("is_plugin_sleeping"), &ClapInstrument::is_plugin_sleeping);
	ClassDB::bind_method(D_METHOD("midi_event", "time_us", "source", "kind", "channel", "index", "value", "note_id"), &ClapInstrument::midi_event);
	ClassDB::bind_method(D_METHOD("midi_stop", "time_us", "source", "mode"), &ClapInstrument::midi_stop);
	ClassDB::bind_method(D_METHOD("midi_transport", "time_us", "source", "playing", "bpm", "song_beat", "loop_start", "loop_end"), &ClapInstrument::midi_transport);
	ClassDB::bind_method(D_METHOD("get_output_count"), &ClapInstrument::get_output_count);
	ClassDB::bind_method(D_METHOD("get_output_names"), &ClapInstrument::get_output_names);
	ClassDB::bind_method(D_METHOD("get_output", "index"), &ClapInstrument::get_output);
	ClassDB::bind_method(D_METHOD("bind_outputs", "players"), &ClapInstrument::bind_outputs);
	ClassDB::bind_method(D_METHOD("open_editor"), &ClapInstrument::open_editor);
	ClassDB::bind_method(D_METHOD("close_editor"), &ClapInstrument::close_editor);
	ClassDB::bind_method(D_METHOD("is_editor_open"), &ClapInstrument::is_editor_open);
	ADD_SIGNAL(MethodInfo("editor_closed"));
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "plugin_path", PROPERTY_HINT_FILE, "*.clap"), "set_plugin_path", "get_plugin_path");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "plugin_id", PROPERTY_HINT_PLACEHOLDER_TEXT, "first plugin in the file"), "set_plugin_id", "get_plugin_id");
	// After the plugin: Godot sets properties in this order when it loads a scene. State before
	// values, so values set by hand after the state was taken still win.
	ClassDB::bind_method(D_METHOD("set_plugin_state", "state"), &ClapInstrument::set_plugin_state);
	ClassDB::bind_method(D_METHOD("get_plugin_state"), &ClapInstrument::get_plugin_state);
	ClassDB::bind_method(D_METHOD("set_param_values", "values"), &ClapInstrument::set_param_values);
	ClassDB::bind_method(D_METHOD("get_param_values"), &ClapInstrument::get_param_values);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "plugin_state", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_plugin_state", "get_plugin_state");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "param_values", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_param_values", "get_param_values");
	ADD_SIGNAL(MethodInfo("parameter_changed", PropertyInfo(Variant::INT, "id"), PropertyInfo(Variant::FLOAT, "value")));
	ADD_SIGNAL(MethodInfo("parameters_changed"));
}

// --- ClapInstrumentOutput ---------------------------------------------------------------------------

Ref<AudioStreamPlayback> ClapInstrumentOutput::_instantiate_playback() const {
	ERR_FAIL_COND_V(instrument.is_null(), Ref<AudioStreamPlayback>());
	Ref<ClapInstrumentPlayback> playback;
	playback.instantiate();
	playback->setup(instrument, uint32_t(index));
	return playback;
}

String ClapInstrumentOutput::_get_stream_name() const {
	if (instrument.is_null()) {
		return "";
	}
	const PackedStringArray names = const_cast<ClapInstrument *>(instrument.ptr())->get_output_names();
	return index < names.size() ? names[index] : String::num_int64(index);
}

void ClapInstrumentOutput::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_instrument"), &ClapInstrumentOutput::get_instrument);
	ClassDB::bind_method(D_METHOD("get_index"), &ClapInstrumentOutput::get_index);
}

} // namespace godot
