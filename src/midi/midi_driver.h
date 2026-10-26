#pragma once

// MidiDriver: the node scripts talk to. It turns calls into timed MIDI events and hands them to
// the targets bound to each channel, through gaph::midi::Sequencer (src/core/midi_sequencer.h),
// which does the planning; this class is the Godot face of it: properties, argument checks,
// the frame tick, and calling the targets.
//
// TARGETS
// A target is any object with two methods (it may live in another extension, or be a script):
//   midi_event(time_us, source, kind, channel, index, value, note_id)
//   midi_stop(time_us, source, mode)
// time_us is on gaph::host_time_us()'s clock (get_time_us() here), and the event is heard one
// audio block after it. Values arrive at MIDI 2.0 resolution whatever the driver's version.
//
// THREADS
// Everything here runs on the MAIN thread: calls from scripts, and the tick, which is an internal
// process notification every frame. Nothing runs on the audio thread; the targets take it from
// there. process_mode is ALWAYS, so a paused SceneTree (a game's pause menu) does not stop the
// music by itself: pause() does.

#include "core/midi_sequencer.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>

namespace godot {

class MidiDriver;

// One note, as note_on() returns it: MIDI 1.0 knows a note only by channel and key, this handle
// knows exactly this one. It only refers to the note; dropping it does not end the note.
class MidiNote : public RefCounted {
	GDCLASS(MidiNote, RefCounted);

public:
	void off(double p_at);
	bool is_active() const; // not ended yet: scheduled, or sounding
	bool is_sounding() const; // its note-on has been heard, and its note-off not yet
	int64_t get_id() const { return id; }
	int get_channel() const { return channel; }
	int get_key() const { return key; }

	void setup(MidiDriver *p_driver, int64_t p_id, int p_channel, int p_key);

protected:
	static void _bind_methods();

private:
	MidiDriver *driver_or_null() const;
	ObjectID driver_id;
	int64_t id = -1;
	int channel = 0, key = 0;
};

class MidiDriver : public Node, private gaph::midi::Sink {
	GDCLASS(MidiDriver, Node);

public:
	// For targets: what midi_event's `kind` and midi_stop's `mode` mean.
	enum EventKind {
		EVENT_NOTE_ON = int(gaph::midi::Kind::NOTE_ON),
		EVENT_NOTE_OFF = int(gaph::midi::Kind::NOTE_OFF),
		EVENT_CONTROL = int(gaph::midi::Kind::CONTROL),
		EVENT_PITCH_BEND = int(gaph::midi::Kind::PITCH_BEND),
		EVENT_POLY_PRESSURE = int(gaph::midi::Kind::POLY_PRESSURE),
		EVENT_CHANNEL_PRESSURE = int(gaph::midi::Kind::CHANNEL_PRESSURE),
		EVENT_PROGRAM = int(gaph::midi::Kind::PROGRAM),
	};
	enum StopMode {
		STOP_WITHDRAW = int(gaph::midi::StopMode::WITHDRAW),
		STOP_RELEASE = int(gaph::midi::StopMode::RELEASE),
		STOP_CUT = int(gaph::midi::StopMode::CUT),
	};

	MidiDriver();

	static MidiDriver *create(int p_midi_version);

	void set_midi_version(int p_version);
	int get_midi_version() const { return version; }
	void set_bpm(double p_bpm);
	double get_bpm() const;
	void set_lookahead_ms(double p_ms);
	double get_lookahead_ms() const;
	int64_t get_velocity_max() const;
	int64_t get_controller_max() const;
	int64_t get_pitch_bend_max() const;

	void set_channel_target(int p_channel, Object *p_target);
	Object *get_channel_target(int p_channel) const;

	// Messages. `at`: a running position from next(), after() or at_beat(), or NOW (-1).
	Ref<MidiNote> note_on(int p_channel, int p_key, int64_t p_velocity, double p_length, double p_at);
	void note_off(int p_channel, int p_key, double p_at);
	void control_change(int p_channel, int p_controller, int64_t p_value, double p_at);
	void pitch_bend(int p_channel, int64_t p_value, double p_at);
	void poly_aftertouch(int p_channel, int p_key, int64_t p_value, double p_at);
	void channel_pressure(int p_channel, int64_t p_value, double p_at);
	void program_change(int p_channel, int p_program, double p_at);
	void all_notes_off(double p_at);

	// The transport.
	void play();
	void pause();
	void stop();
	void panic();
	void seek(double p_song_beat);
	void set_loop(double p_start, double p_end);
	void clear_loop();
	bool is_playing() const;
	double get_position() const; // the song position now
	double next(double p_grid) const;
	double after(double p_beats) const;
	double at_beat(double p_song_beat) const;

	// The clock targets get times on (microseconds).
	static int64_t get_time_us();

	// For MidiNote.
	void end_note(int64_t p_id, double p_at);
	gaph::midi::NoteState note_state(int64_t p_id) const { return seq.note_state(p_id); }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	// gaph::midi::Sink: the sequencer's output, as calls on the targets.
	void send(uint64_t p_target, int64_t p_time_us, const gaph::midi::Event &p_event) override;
	void stop(uint64_t p_target, int64_t p_time_us, gaph::midi::StopMode p_mode) override;
	void beat(int64_t p_running_beat, double p_song_beat) override;
	void looped(double p_running, double p_song_beat) override;

	bool check_channel(int p_channel) const;
	bool check_7bit(int p_value, const char *p_what) const;
	bool inside(gaph::midi::Kind p_kind, int64_t p_value, uint32_t &r_inside) const;

	gaph::midi::Sequencer seq{ *this };
	int version = 1;
};

} // namespace godot

VARIANT_ENUM_CAST(MidiDriver::EventKind);
VARIANT_ENUM_CAST(MidiDriver::StopMode);
