#pragma once

// Clip: a piece of MIDI on a beat timeline, as a MidiClip resource holds it. Plain C++, no Godot.
//
// Read from a Standard MIDI File or a MIDI Clip File (midi_file.h), or built by hand (a pattern).
// Positions are in BEATS (quarter notes) from the clip's start, so a clip plays at any tempo; the
// file's own tempo map is kept beside the events, for a driver that follows it. Values are held
// at MIDI 2.0 resolution, like everything inside (midi_values.h), whatever the file's version.
// Notes are stored with their length, paired when the file is read, so a clip can be started
// anywhere and still end every note it starts.

#include "midi_values.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace gaph::midi {

struct ClipEvent {
	double beat = 0; // from the clip's start
	double length = 0; // notes: beats until the note-off; 0 for other kinds
	uint16_t track = 0;
	Kind kind = Kind::NOTE_ON; // NOTE_ON (a whole note), CONTROL, PITCH_BEND, POLY_PRESSURE, CHANNEL_PRESSURE, PROGRAM
	uint8_t channel = 1; // 1-16
	uint16_t index = 0; // the note, or the controller number
	uint32_t value = 0; // at MIDI 2.0 resolution
};

// A tempo from a beat on, in units of 10 ns per beat: MIDI 2.0's own unit, and a Standard MIDI
// File's microseconds per quarter note times 100, so both are held exactly.
struct ClipTempo {
	double beat = 0;
	uint32_t ten_ns_per_beat = 50000000; // 120 bpm, the files' default
	double bpm() const { return 6e9 / double(ten_ns_per_beat); }
};

struct ClipMeter {
	double beat = 0;
	uint8_t numerator = 4, denominator = 4;
};

struct Clip {
	int midi_version = 1; // what the file stated (a .mid is always 1); a pattern: 1 unless set
	double length = 0; // beats: where a repeat starts again
	std::vector<std::string> track_names;
	std::vector<ClipEvent> events; // sorted by beat, keeping file order within a beat
	std::vector<ClipTempo> tempos; // sorted by beat; empty if the file sets none
	std::vector<ClipMeter> meters;

	void sort() {
		std::stable_sort(events.begin(), events.end(), [](const ClipEvent &a, const ClipEvent &b) { return a.beat < b.beat; });
		std::stable_sort(tempos.begin(), tempos.end(), [](const ClipTempo &a, const ClipTempo &b) { return a.beat < b.beat; });
		std::stable_sort(meters.begin(), meters.end(), [](const ClipMeter &a, const ClipMeter &b) { return a.beat < b.beat; });
	}

	// The tempo in force just before p_beat (a change exactly at p_beat is not yet in force);
	// a file without tempos, or before its first, plays at 120 bpm.
	uint32_t tempo_before(double p_beat) const {
		uint32_t t = 50000000;
		for (const ClipTempo &c : tempos) {
			if (c.beat >= p_beat) {
				break;
			}
			t = c.ten_ns_per_beat;
		}
		return t;
	}

	// The first event at or after p_beat.
	size_t first_at(double p_beat) const {
		return size_t(std::lower_bound(events.begin(), events.end(), p_beat, [](const ClipEvent &e, double b) { return e.beat < b; }) - events.begin());
	}

	// One bar of the first meter, in beats (4 in 4/4, 3 in 6/8).
	double bar_beats() const {
		const ClipMeter m = meters.empty() ? ClipMeter{} : meters.front();
		return m.denominator ? 4.0 * m.numerator / m.denominator : 4.0;
	}
};

} // namespace gaph::midi
