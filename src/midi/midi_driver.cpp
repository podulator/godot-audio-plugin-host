#include "midi_driver.h"

#include "core/block_clock.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;
using gaph::midi::Event;
using gaph::midi::Kind;
using gaph::midi::NoteState;
using gaph::midi::Transport;

// --- MidiNote ------------------------------------------------------------------------------------

void MidiNote::setup(MidiDriver *p_driver, int64_t p_id, int p_channel, int p_key) {
	driver_id = p_driver->get_instance_id();
	id = p_id;
	channel = p_channel;
	key = p_key;
}

// The driver may have been freed while a script still holds the note: looked up each time.
MidiDriver *MidiNote::driver_or_null() const {
	return Object::cast_to<MidiDriver>(ObjectDB::get_instance(driver_id));
}

void MidiNote::off(double p_at) {
	if (MidiDriver *d = driver_or_null()) {
		d->end_note(id, p_at);
	}
}

bool MidiNote::is_active() const {
	MidiDriver *d = driver_or_null();
	return d && d->note_state(id) != NoteState::ENDED;
}

bool MidiNote::is_sounding() const {
	MidiDriver *d = driver_or_null();
	return d && d->note_state(id) == NoteState::SOUNDING;
}

void MidiNote::_bind_methods() {
	ClassDB::bind_method(D_METHOD("off", "at"), &MidiNote::off, DEFVAL(Transport::NOW));
	ClassDB::bind_method(D_METHOD("is_active"), &MidiNote::is_active);
	ClassDB::bind_method(D_METHOD("is_sounding"), &MidiNote::is_sounding);
	ClassDB::bind_method(D_METHOD("get_id"), &MidiNote::get_id);
	ClassDB::bind_method(D_METHOD("get_channel"), &MidiNote::get_channel);
	ClassDB::bind_method(D_METHOD("get_key"), &MidiNote::get_key);
}

// --- MidiDriver: setup and properties ------------------------------------------------------------

MidiDriver::MidiDriver() {
	set_process_mode(PROCESS_MODE_ALWAYS);
}

MidiDriver *MidiDriver::create(int p_midi_version) {
	MidiDriver *d = memnew(MidiDriver);
	d->set_midi_version(p_midi_version);
	return d;
}

void MidiDriver::set_midi_version(int p_version) {
	ERR_FAIL_COND_MSG(p_version != 1 && p_version != 2, "MidiDriver: midi_version must be 1 or 2.");
	version = p_version;
}

void MidiDriver::set_bpm(double p_bpm) {
	ERR_FAIL_COND_MSG(!(p_bpm > 0), "MidiDriver: bpm must be above 0.");
	if (p_bpm == seq.transport().get_bpm()) {
		return;
	}
	seq.set_bpm(p_bpm, get_time_us());
	emit_signal("tempo_changed", p_bpm);
}
double MidiDriver::get_bpm() const { return seq.transport().get_bpm(); }

void MidiDriver::set_lookahead_ms(double p_ms) { seq.set_lookahead_us(int64_t(p_ms * 1000.0)); }
double MidiDriver::get_lookahead_ms() const { return double(seq.get_lookahead_us()) / 1000.0; }

int64_t MidiDriver::get_velocity_max() const { return gaph::midi::max_value(Kind::NOTE_ON, version); }
int64_t MidiDriver::get_controller_max() const { return gaph::midi::max_value(Kind::CONTROL, version); }
int64_t MidiDriver::get_pitch_bend_max() const { return gaph::midi::max_value(Kind::PITCH_BEND, version); }

int64_t MidiDriver::get_time_us() { return gaph::host_time_us(); }

// --- Messages ------------------------------------------------------------------------------------

bool MidiDriver::check_channel(int p_channel) const {
	ERR_FAIL_COND_V_MSG(p_channel < 1 || p_channel > 16, false,
			vformat("MidiDriver: channel %d is not 1-16 (channels count from 1, as on MIDI gear; Godot's InputEventMIDI counts from 0).", p_channel));
	return true;
}

bool MidiDriver::check_7bit(int p_value, const char *p_what) const {
	ERR_FAIL_COND_V_MSG(p_value < 0 || p_value > 127, false, vformat("MidiDriver: %s %d is not 0-127.", p_what, p_value));
	return true;
}

// A value from the caller, in this driver's MIDI version's range, to the inside resolution.
bool MidiDriver::inside(Kind p_kind, int64_t p_value, uint32_t &r_inside) const {
	bool ok = false;
	r_inside = gaph::midi::to_inside(p_kind, p_value, version, ok);
	ERR_FAIL_COND_V_MSG(!ok, false,
			vformat("MidiDriver: %d is outside MIDI %d.0's range for this message (0 to %d%s).", p_value, version,
					gaph::midi::max_value(p_kind, version), p_kind == Kind::PITCH_BEND ? ", and as far below 0" : ""));
	return true;
}

Ref<MidiNote> MidiDriver::note_on(int p_channel, int p_key, int64_t p_velocity, double p_length, double p_at) {
	uint32_t velocity = 0;
	if (!check_channel(p_channel) || !check_7bit(p_key, "note") || !inside(Kind::NOTE_ON, p_velocity, velocity)) {
		return Ref<MidiNote>();
	}
	// MIDI's own rule: a note-on with velocity 0 is a note-off.
	if (p_velocity == 0) {
		note_off(p_channel, p_key, p_at);
		return Ref<MidiNote>();
	}
	ERR_FAIL_COND_V_MSG(std::isnan(p_at), Ref<MidiNote>(), "MidiDriver: that position is never reached (outside the running loop).");
	const int64_t id = seq.note_on(uint8_t(p_channel), uint16_t(p_key), velocity, p_length, p_at, get_time_us());
	Ref<MidiNote> n;
	n.instantiate();
	n->setup(this, id, p_channel, p_key);
	return n;
}

void MidiDriver::note_off(int p_channel, int p_key, double p_at) {
	if (check_channel(p_channel) && check_7bit(p_key, "note")) {
		seq.note_off(uint8_t(p_channel), uint16_t(p_key), p_at, get_time_us());
	}
}

void MidiDriver::end_note(int64_t p_id, double p_at) {
	seq.end_note(p_id, p_at, get_time_us());
}

void MidiDriver::control_change(int p_channel, int p_controller, int64_t p_value, double p_at) {
	uint32_t v = 0;
	if (check_channel(p_channel) && check_7bit(p_controller, "controller") && inside(Kind::CONTROL, p_value, v)) {
		seq.post(Event{ Kind::CONTROL, uint8_t(p_channel), uint16_t(p_controller), v, -1 }, p_at, get_time_us());
	}
}

void MidiDriver::pitch_bend(int p_channel, int64_t p_value, double p_at) {
	uint32_t v = 0;
	if (check_channel(p_channel) && inside(Kind::PITCH_BEND, p_value, v)) {
		seq.post(Event{ Kind::PITCH_BEND, uint8_t(p_channel), 0, v, -1 }, p_at, get_time_us());
	}
}

void MidiDriver::poly_aftertouch(int p_channel, int p_key, int64_t p_value, double p_at) {
	uint32_t v = 0;
	if (check_channel(p_channel) && check_7bit(p_key, "note") && inside(Kind::POLY_PRESSURE, p_value, v)) {
		seq.post(Event{ Kind::POLY_PRESSURE, uint8_t(p_channel), uint16_t(p_key), v, -1 }, p_at, get_time_us());
	}
}

void MidiDriver::channel_pressure(int p_channel, int64_t p_value, double p_at) {
	uint32_t v = 0;
	if (check_channel(p_channel) && inside(Kind::CHANNEL_PRESSURE, p_value, v)) {
		seq.post(Event{ Kind::CHANNEL_PRESSURE, uint8_t(p_channel), 0, v, -1 }, p_at, get_time_us());
	}
}

void MidiDriver::program_change(int p_channel, int p_program, double p_at) {
	if (check_channel(p_channel) && check_7bit(p_program, "program")) {
		seq.post(Event{ Kind::PROGRAM, uint8_t(p_channel), 0, uint32_t(p_program), -1 }, p_at, get_time_us());
	}
}

void MidiDriver::all_notes_off(double p_at) {
	seq.end_every_note(p_at, get_time_us());
}

// --- The transport -------------------------------------------------------------------------------

// Each transport call emits its signal after the change, so a handler sees the new state.
void MidiDriver::play() {
	if (seq.transport().is_playing()) {
		return;
	}
	const bool resuming = seq.is_paused();
	seq.play(get_time_us());
	if (resuming) {
		emit_signal("resumed", get_position());
	} else {
		emit_signal("started");
	}
}

void MidiDriver::pause() {
	if (!seq.transport().is_playing()) {
		return;
	}
	seq.pause(get_time_us());
	emit_signal("paused", get_position());
}

void MidiDriver::stop() {
	seq.stop(get_time_us());
	emit_signal("stopped");
}

void MidiDriver::panic() {
	seq.panic(get_time_us());
	emit_signal("stopped");
}

void MidiDriver::seek(double p_song_beat) {
	seq.seek(p_song_beat, get_time_us());
	emit_signal("position_changed", p_song_beat);
}
void MidiDriver::set_loop(double p_start, double p_end) {
	ERR_FAIL_COND_MSG(!(p_end > p_start), "MidiDriver: a loop must end after it starts (clear_loop() turns looping off).");
	seq.set_loop(p_start, p_end, get_time_us());
}
void MidiDriver::clear_loop() { seq.set_loop(0, 0, get_time_us()); }
bool MidiDriver::is_playing() const { return seq.transport().is_playing(); }

double MidiDriver::get_position() const {
	const Transport &t = seq.transport();
	return t.song_at(t.running_at(get_time_us()));
}

double MidiDriver::next(double p_grid) const { return seq.transport().next(p_grid, get_time_us()); }
double MidiDriver::after(double p_beats) const { return seq.transport().after(p_beats, get_time_us()); }
double MidiDriver::at_beat(double p_song_beat) const { return seq.transport().at_song(p_song_beat, get_time_us()); }

// --- Targets -------------------------------------------------------------------------------------

void MidiDriver::set_channel_target(int p_channel, Object *p_target) {
	if (!check_channel(p_channel)) {
		return;
	}
	ERR_FAIL_COND_MSG(p_target && !p_target->has_method("midi_event"),
			"MidiDriver: a target needs midi_event(time_us, source, kind, channel, index, value, note_id) and midi_stop(time_us, source, mode).");
	seq.set_channel_target(p_channel, p_target ? uint64_t(p_target->get_instance_id()) : 0, get_time_us());
}

Object *MidiDriver::get_channel_target(int p_channel) const {
	if (!check_channel(p_channel)) {
		return nullptr;
	}
	return ObjectDB::get_instance(ObjectID(seq.get_channel_target(p_channel)));
}

// --- The sequencer's output ----------------------------------------------------------------------
// A target freed in the meantime is skipped: ObjectDB returns null for an id that no longer lives.
// `source` is this driver's instance id, so a target fed by several drivers keeps them apart.

void MidiDriver::send(uint64_t p_target, int64_t p_time_us, const Event &p_event) {
	if (Object *t = ObjectDB::get_instance(ObjectID(p_target))) {
		t->call("midi_event", p_time_us, int64_t(get_instance_id()), int(p_event.kind), int(p_event.channel), int(p_event.index),
				int64_t(p_event.value), p_event.note_id);
	}
}

void MidiDriver::stop(uint64_t p_target, int64_t p_time_us, gaph::midi::StopMode p_mode) {
	if (Object *t = ObjectDB::get_instance(ObjectID(p_target))) {
		t->call("midi_stop", p_time_us, int64_t(get_instance_id()), int(p_mode));
	}
}

void MidiDriver::beat(int64_t p_running_beat, double p_song_beat) {
	emit_signal("beat", p_running_beat, p_song_beat);
}

// Optional for targets: one without midi_transport (a GDScript synth with no tempo-synced parts,
// say) is simply not told. loop_end <= loop_start means no loop.
void MidiDriver::transport(uint64_t p_target, int64_t p_time_us, const gaph::midi::TransportInfo &p_info) {
	Object *t = ObjectDB::get_instance(ObjectID(p_target));
	if (!t || !t->has_method("midi_transport")) {
		return;
	}
	t->call("midi_transport", p_time_us, int64_t(get_instance_id()), p_info.playing, p_info.bpm, p_info.song_beat,
			p_info.looping ? p_info.loop_start : 0.0, p_info.looping ? p_info.loop_end : 0.0);
}

void MidiDriver::looped(double, double p_song_beat) {
	emit_signal("position_changed", p_song_beat);
}

void MidiDriver::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY:
			set_process_internal(true);
			break;
		case NOTIFICATION_INTERNAL_PROCESS:
			seq.tick(get_time_us());
			break;
		case NOTIFICATION_EXIT_TREE:
			// Leaving the tree, the driver can no longer tick: its notes would hang.
			seq.stop(get_time_us());
			break;
	}
}

void MidiDriver::_bind_methods() {
	// NOW as an `at`. Godot's class constants are integers; -1 becomes -1.0 where an `at` is taken.
	const Variant NOW = Transport::NOW;
	ClassDB::bind_integer_constant(get_class_static(), StringName(), "NOW", int64_t(Transport::NOW));
	ClassDB::bind_static_method("MidiDriver", D_METHOD("create", "midi_version"), &MidiDriver::create, DEFVAL(1));
	ClassDB::bind_static_method("MidiDriver", D_METHOD("get_time_us"), &MidiDriver::get_time_us);

	ClassDB::bind_method(D_METHOD("set_midi_version", "version"), &MidiDriver::set_midi_version);
	ClassDB::bind_method(D_METHOD("get_midi_version"), &MidiDriver::get_midi_version);
	ClassDB::bind_method(D_METHOD("set_bpm", "bpm"), &MidiDriver::set_bpm);
	ClassDB::bind_method(D_METHOD("get_bpm"), &MidiDriver::get_bpm);
	ClassDB::bind_method(D_METHOD("set_lookahead_ms", "ms"), &MidiDriver::set_lookahead_ms);
	ClassDB::bind_method(D_METHOD("get_lookahead_ms"), &MidiDriver::get_lookahead_ms);
	ClassDB::bind_method(D_METHOD("get_velocity_max"), &MidiDriver::get_velocity_max);
	ClassDB::bind_method(D_METHOD("get_controller_max"), &MidiDriver::get_controller_max);
	ClassDB::bind_method(D_METHOD("get_pitch_bend_max"), &MidiDriver::get_pitch_bend_max);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "midi_version", PROPERTY_HINT_ENUM, "MIDI 1.0:1,MIDI 2.0:2"), "set_midi_version", "get_midi_version");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bpm", PROPERTY_HINT_RANGE, "1,999,0.01,or_greater"), "set_bpm", "get_bpm");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lookahead_ms", PROPERTY_HINT_RANGE, "0,1000,1,suffix:ms"), "set_lookahead_ms", "get_lookahead_ms");

	ClassDB::bind_method(D_METHOD("set_channel_target", "channel", "target"), &MidiDriver::set_channel_target);
	ClassDB::bind_method(D_METHOD("get_channel_target", "channel"), &MidiDriver::get_channel_target);

	ClassDB::bind_method(D_METHOD("note_on", "channel", "key", "velocity", "length", "at"), &MidiDriver::note_on, DEFVAL(0.0), DEFVAL(NOW));
	ClassDB::bind_method(D_METHOD("note_off", "channel", "key", "at"), &MidiDriver::note_off, DEFVAL(NOW));
	ClassDB::bind_method(D_METHOD("control_change", "channel", "controller", "value", "at"), &MidiDriver::control_change, DEFVAL(NOW));
	ClassDB::bind_method(D_METHOD("pitch_bend", "channel", "value", "at"), &MidiDriver::pitch_bend, DEFVAL(NOW));
	ClassDB::bind_method(D_METHOD("poly_aftertouch", "channel", "key", "value", "at"), &MidiDriver::poly_aftertouch, DEFVAL(NOW));
	ClassDB::bind_method(D_METHOD("channel_pressure", "channel", "value", "at"), &MidiDriver::channel_pressure, DEFVAL(NOW));
	ClassDB::bind_method(D_METHOD("program_change", "channel", "program", "at"), &MidiDriver::program_change, DEFVAL(NOW));
	ClassDB::bind_method(D_METHOD("all_notes_off", "at"), &MidiDriver::all_notes_off, DEFVAL(NOW));

	ClassDB::bind_method(D_METHOD("play"), &MidiDriver::play);
	ClassDB::bind_method(D_METHOD("pause"), &MidiDriver::pause);
	// stop() is overloaded (the sequencer's output has one too): the cast picks the transport's.
	ClassDB::bind_method(D_METHOD("stop"), static_cast<void (MidiDriver::*)()>(&MidiDriver::stop));
	ClassDB::bind_method(D_METHOD("panic"), &MidiDriver::panic);
	ClassDB::bind_method(D_METHOD("seek", "song_beat"), &MidiDriver::seek);
	ClassDB::bind_method(D_METHOD("set_loop", "start", "end"), &MidiDriver::set_loop);
	ClassDB::bind_method(D_METHOD("clear_loop"), &MidiDriver::clear_loop);
	ClassDB::bind_method(D_METHOD("is_playing"), &MidiDriver::is_playing);
	ClassDB::bind_method(D_METHOD("get_position"), &MidiDriver::get_position);
	ClassDB::bind_method(D_METHOD("next", "grid"), &MidiDriver::next);
	ClassDB::bind_method(D_METHOD("after", "beats"), &MidiDriver::after);
	ClassDB::bind_method(D_METHOD("at_beat", "song_beat"), &MidiDriver::at_beat);

	ADD_SIGNAL(MethodInfo("beat", PropertyInfo(Variant::INT, "running_beat"), PropertyInfo(Variant::FLOAT, "song_beat")));
	ADD_SIGNAL(MethodInfo("started"));
	ADD_SIGNAL(MethodInfo("paused", PropertyInfo(Variant::FLOAT, "song_beat")));
	ADD_SIGNAL(MethodInfo("resumed", PropertyInfo(Variant::FLOAT, "song_beat")));
	ADD_SIGNAL(MethodInfo("stopped"));
	ADD_SIGNAL(MethodInfo("position_changed", PropertyInfo(Variant::FLOAT, "song_beat")));
	ADD_SIGNAL(MethodInfo("tempo_changed", PropertyInfo(Variant::FLOAT, "bpm")));

	BIND_ENUM_CONSTANT(EVENT_NOTE_ON);
	BIND_ENUM_CONSTANT(EVENT_NOTE_OFF);
	BIND_ENUM_CONSTANT(EVENT_CONTROL);
	BIND_ENUM_CONSTANT(EVENT_PITCH_BEND);
	BIND_ENUM_CONSTANT(EVENT_POLY_PRESSURE);
	BIND_ENUM_CONSTANT(EVENT_CHANNEL_PRESSURE);
	BIND_ENUM_CONSTANT(EVENT_PROGRAM);
	BIND_ENUM_CONSTANT(STOP_WITHDRAW);
	BIND_ENUM_CONSTANT(STOP_RELEASE);
	BIND_ENUM_CONSTANT(STOP_CUT);
}
