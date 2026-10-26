#pragma once

// NoteBook: an instrument's record of the notes it was sent, kept on the MAIN thread, so that a
// midi_stop can be answered exactly. Plain C++, no Godot.
//
// WHY ON THE MAIN THREAD
// midi_stop(T, source, mode) means: drop every event from that source due at or after T, and (for
// RELEASE and CUT) end every note of it still sounding at T. The audio thread only needs to be told
// the first half (a DROP event, effect_runtime.h). Which notes are sounding at T follows from what
// was queued, and the main thread queued all of it: a note sounds at T if its note-on is due before
// T and its note-off is not (none, or one at or after T, which the DROP removes). So the book
// answers that, and the instrument queues a note-off at T for each such note.

#include <cstdint>
#include <vector>

namespace gaph::midi {

class NoteBook {
public:
	struct Sounding {
		uint64_t source;
		int64_t note_id; // -1 for notes sent without one (then channel and key identify them)
		uint8_t channel;
		uint16_t key;
	};

	void note_on(uint64_t p_source, int64_t p_note_id, uint8_t p_channel, uint16_t p_key, int64_t p_due_us) {
		notes.push_back(Entry{ p_source, p_note_id, p_channel, p_key, p_due_us, NO_OFF });
	}

	// Ends the matching note that started earliest and has no end yet (by id, or by channel and
	// key for notes without an id).
	void note_off(uint64_t p_source, int64_t p_note_id, uint8_t p_channel, uint16_t p_key, int64_t p_due_us) {
		for (Entry &e : notes) {
			if (e.source == p_source && e.off_us == NO_OFF && matches(e, p_note_id, p_channel, p_key)) {
				e.off_us = p_due_us;
				return;
			}
		}
	}

	// midi_stop's bookkeeping: every note of p_source sounding at p_time_us (they need a note-off
	// there), and everything from p_time_us on forgotten, as the audio thread drops it.
	std::vector<Sounding> stop(uint64_t p_source, int64_t p_time_us) {
		std::vector<Sounding> out;
		size_t keep = 0;
		for (const Entry &e : notes) {
			if (e.source != p_source) {
				notes[keep++] = e;
				continue;
			}
			if (e.on_us < p_time_us && (e.off_us == NO_OFF || e.off_us >= p_time_us)) {
				out.push_back(Sounding{ e.source, e.note_id, e.channel, e.key });
			}
			// Gone either way: ended before T, or ended now (the caller's note-off at T), or not
			// started before T (dropped).
		}
		notes.resize(keep);
		return out;
	}

	// midi_stop's WITHDRAW (a tempo change): what is due from p_time_us on is dropped, but sounding
	// notes go on. So notes not started by then are forgotten, and an end planned from then on is
	// undone (the driver sends it again, re-timed).
	void withdraw(uint64_t p_source, int64_t p_time_us) {
		size_t keep = 0;
		for (Entry &e : notes) {
			if (e.source == p_source && e.on_us >= p_time_us) {
				continue;
			}
			if (e.source == p_source && e.off_us != NO_OFF && e.off_us >= p_time_us) {
				e.off_us = NO_OFF;
			}
			notes[keep++] = e;
		}
		notes.resize(keep);
	}

	// Forgets notes that ended before p_now_us. Called now and then, so the book stays small.
	void prune(int64_t p_now_us) {
		size_t keep = 0;
		for (const Entry &e : notes) {
			if (e.off_us == NO_OFF || e.off_us >= p_now_us) {
				notes[keep++] = e;
			}
		}
		notes.resize(keep);
	}

	size_t size() const { return notes.size(); }

private:
	static constexpr int64_t NO_OFF = INT64_MAX;

	struct Entry {
		uint64_t source;
		int64_t note_id;
		uint8_t channel;
		uint16_t key;
		int64_t on_us;
		int64_t off_us;
	};

	static bool matches(const Entry &e, int64_t p_note_id, uint8_t p_channel, uint16_t p_key) {
		if (p_note_id >= 0 && e.note_id >= 0) {
			return e.note_id == p_note_id;
		}
		return e.channel == p_channel && e.key == p_key;
	}

	std::vector<Entry> notes;
};

} // namespace gaph::midi
