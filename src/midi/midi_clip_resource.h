#pragma once

// MidiClip: a piece of MIDI on a beat timeline, as a resource. Imported from a .mid file (MIDI 1.0)
// or a .midi2 MIDI Clip File (MIDI 2.0), loaded from any file at run time (load_file), or built in
// code as a pattern (add_note, add_event). A MidiDriver plays it: place_clip() on the song's
// timeline, play_clip() from a moment on (midi_driver.h).
//
// The events are gaph::midi::Clip (core/midi_clip.h), shared with the parts playing them: editing
// a clip makes a fresh copy, so a part already playing keeps what it started with, and the next
// place_clip() or play_clip() gets the edit.
//
// SAVED as plain arrays (a resource file, or a scene's built-in resource): midi_version, length,
// track_names, and the events, tempo map and meters packed as numbers.

#include "core/midi_clip.h"

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <memory>

namespace godot {

class MidiClip : public Resource {
	GDCLASS(MidiClip, Resource);

public:
	MidiClip();

	// From a file or its bytes: null, with an error printed, if it is neither kind of MIDI file.
	static Ref<MidiClip> load_file(const String &p_path);
	static Ref<MidiClip> from_bytes(const PackedByteArray &p_bytes);

	void set_midi_version(int p_version);
	int get_midi_version() const { return clip->midi_version; }
	void set_length(double p_beats);
	double get_length() const { return clip->length; }
	void set_track_names(const PackedStringArray &p_names);
	PackedStringArray get_track_names() const;

	// Building a pattern. Values in this clip's MIDI version's range, as MidiDriver takes them.
	void add_note(double p_beat, int p_channel, int p_key, int64_t p_velocity, double p_length, int p_track);
	void add_event(double p_beat, int p_kind, int p_channel, int p_index, int64_t p_value, int p_track);
	void set_tempo(double p_beat, double p_bpm);
	void set_meter(double p_beat, int p_numerator, int p_denominator);
	void clear();

	int get_event_count() const { return int(clip->events.size()); }
	TypedArray<Dictionary> get_events() const;
	TypedArray<Dictionary> get_tempo_map() const;
	double get_bpm() const; // the first tempo, or 120

	// For MidiDriver: the events as the sequencer reads them.
	std::shared_ptr<const gaph::midi::Clip> get_clip() const { return clip; }
	void set_clip(gaph::midi::Clip &&p_clip);

	// Storage only (saved, not shown): the events, tempos and meters packed as numbers.
	void set_event_data(const PackedFloat64Array &p_data);
	PackedFloat64Array get_event_data() const;
	void set_tempo_data(const PackedFloat64Array &p_data);
	PackedFloat64Array get_tempo_data() const;
	void set_meter_data(const PackedFloat64Array &p_data);
	PackedFloat64Array get_meter_data() const;

protected:
	static void _bind_methods();

private:
	gaph::midi::Clip &edit(); // a fresh copy if any part holds this one
	bool check(int p_channel, int p_index, int p_track) const;
	std::shared_ptr<gaph::midi::Clip> clip;
};

} // namespace godot
