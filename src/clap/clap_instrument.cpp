#include "clap_instrument.h"

#include "core/midi_values.h"
#include "godot_common/plugin_paths.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstring>

namespace godot {

using gaph_godot::to_std;
using gaph::midi::Kind;

// --- ClapInstrumentPlayback ------------------------------------------------------------------

// MAIN THREAD, before Godot adds the playback to the mix, so no lock is needed to hand the runtime
// a plugin instance (HostedEffect::attach, which gives it a spare one if there is one).
void ClapInstrumentPlayback::setup(const Ref<ClapInstrument> &p_instrument) {
	instrument = p_instrument;
	runtime.set_sample_rate(AudioServer::get_singleton()->get_mix_rate());
	silence.assign(size_t(gaph::EffectRuntime::MAX_BLOCK) * 2, 0.0f);
	instrument->get_hosted().attach(runtime);
}

// MAIN THREAD, after Godot has taken the playback out of the mix (see the header).
ClapInstrumentPlayback::~ClapInstrumentPlayback() {
	if (instrument.is_valid()) {
		instrument->get_hosted().detach(runtime);
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

// AUDIO THREAD. The plugin's output for p_frames frames, in slices of at most MAX_BLOCK (Godot asks
// for 512 at a time; the loop is insurance). Silence goes in: an instrument has no audio input.
int32_t ClapInstrumentPlayback::_mix(AudioFrame *p_buffer, float, int32_t p_frames) {
	float *out = reinterpret_cast<float *>(p_buffer);
	int32_t done = 0;
	while (done < p_frames) {
		const uint32_t n = uint32_t(std::min<int32_t>(p_frames - done, int32_t(gaph::EffectRuntime::MAX_BLOCK)));
		runtime.process(silence.data(), out + size_t(done) * 2, n);
		done += int32_t(n);
	}
	return p_frames;
}

// --- ClapInstrument: setup and loading (as ClapPluginEffect does it) ---------------------------

// HostedEffect takes Godot's audio lock as two lambdas, so the core includes no Godot header.
ClapInstrument::ClapInstrument() :
		hosted(gaph::HostedEffect::AudioLock{
				[] { AudioServer::get_singleton()->lock(); },
				[] { AudioServer::get_singleton()->unlock(); } }) {
}

ClapInstrument::~ClapInstrument() {
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
	err = hosted.set_plugin(factory, probe->read_params());
	if (!err.empty()) {
		UtilityFunctions::push_error("ClapInstrument: ", String::utf8(err.c_str()));
	}
	library = lib;
	connect_idle();
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
	hosted.idle();
	const std::string err = hosted.take_error();
	if (!err.empty()) {
		UtilityFunctions::push_error("ClapInstrument: ", String::utf8(err.c_str()));
	}
	notes.prune(gaph::host_time_us() - 1000000); // a second's margin: the book stays small
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

// --- MIDI in ----------------------------------------------------------------------------------

void ClapInstrument::queue(int p_kind, int p_channel, int p_index, uint32_t p_value, int64_t p_note_id, uint64_t p_source, int64_t p_time_us) {
	gaph::ParamEvent ev;
	ev.type = gaph::ParamEvent::MIDI;
	ev.midi_kind = uint8_t(p_kind);
	ev.midi_channel = uint8_t(p_channel);
	ev.midi_index = uint16_t(p_index);
	ev.midi_value = p_value;
	ev.note_id = p_note_id;
	ev.source = p_source;
	ev.due_us = p_time_us;
	hosted.queue_midi(ev);
}

void ClapInstrument::midi_event(int64_t p_time_us, int64_t p_source, int p_kind, int p_channel, int p_index, int64_t p_value, int64_t p_note_id) {
	ERR_FAIL_COND_MSG(p_kind < int(Kind::NOTE_ON) || p_kind > int(Kind::PROGRAM), vformat("ClapInstrument: unknown MIDI event kind %d.", p_kind));
	ERR_FAIL_COND_MSG(p_channel < 1 || p_channel > 16, vformat("ClapInstrument: channel %d is not 1-16.", p_channel));
	ensure_loaded();
	const uint64_t source = uint64_t(p_source);
	if (p_kind == int(Kind::NOTE_ON)) {
		notes.note_on(source, p_note_id, uint8_t(p_channel), uint16_t(p_index), p_time_us);
	} else if (p_kind == int(Kind::NOTE_OFF)) {
		notes.note_off(source, p_note_id, uint8_t(p_channel), uint16_t(p_index), p_time_us);
	}
	queue(p_kind, p_channel, p_index, uint32_t(p_value), p_note_id, source, p_time_us);
}

// See the header: the audio side drops this source's events from p_time_us on, and the notes still
// sounding then are ended here, at p_time_us: a note-off lets their tails ring (RELEASE), a choke
// silences them (CUT). WITHDRAW leaves sounding notes alone.
void ClapInstrument::midi_stop(int64_t p_time_us, int64_t p_source, int p_mode) {
	ensure_loaded();
	const uint64_t source = uint64_t(p_source);
	hosted.queue_drop(source, p_time_us);
	if (p_mode == 0) {
		notes.withdraw(source, p_time_us);
		return;
	}
	const bool cut = p_mode == 2;
	for (const gaph::midi::NoteBook::Sounding &n : notes.stop(source, p_time_us)) {
		queue(int(cut ? Kind::CHOKE : Kind::NOTE_OFF), n.channel, n.key, cut ? 0u : 0x8000u, n.note_id, source, p_time_us);
	}
}

// The driver's transport, from p_time_us on, for the plugin (CLAP transport: tempo-synced
// arpeggiators, LFOs and delays follow the song). Travels with the notes, on the same exact grid.
void ClapInstrument::midi_transport(int64_t p_time_us, int64_t p_source, bool p_playing, double p_bpm, double p_song_beat, double p_loop_start, double p_loop_end) {
	ensure_loaded();
	gaph::ParamEvent ev;
	ev.type = gaph::ParamEvent::TRANSPORT;
	ev.midi_value = p_playing ? 1 : 0;
	ev.value = p_bpm;
	ev.transport_beat = p_song_beat;
	ev.loop_start = p_loop_start;
	ev.loop_end = p_loop_end;
	ev.source = uint64_t(p_source);
	ev.due_us = p_time_us;
	hosted.queue_midi(ev);
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
	ClassDB::bind_method(D_METHOD("midi_event", "time_us", "source", "kind", "channel", "index", "value", "note_id"), &ClapInstrument::midi_event);
	ClassDB::bind_method(D_METHOD("midi_stop", "time_us", "source", "mode"), &ClapInstrument::midi_stop);
	ClassDB::bind_method(D_METHOD("midi_transport", "time_us", "source", "playing", "bpm", "song_beat", "loop_start", "loop_end"), &ClapInstrument::midi_transport);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "plugin_path", PROPERTY_HINT_FILE, "*.clap"), "set_plugin_path", "get_plugin_path");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "plugin_id", PROPERTY_HINT_PLACEHOLDER_TEXT, "first plugin in the file"), "set_plugin_id", "get_plugin_id");
}

} // namespace godot
