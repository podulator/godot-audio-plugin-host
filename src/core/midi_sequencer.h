#pragma once

// Sequencer: what the MIDI driver does between a script's call and an instrument. Plain C++, no
// Godot, so the core tests run it with made-up times. MAIN THREAD only.
//
// THE PATH OF AN EVENT
// 1. A script asks for it, with an `at` (Transport): NOW, or a running position.
// 2. NOW events go out at once. Others wait in the SCHEDULE, sorted by running position.
// 3. Every frame, tick() hands out what falls within the LOOKAHEAD (about 100 ms), each with its
//    host time; the instrument places it on the exact sample (block_clock.h).
// 4. Handed out but not heard yet, an event is IN FLIGHT. Once its time has passed it is HEARD:
//    only then does it count for note tracking and for chasing.
//
// Pause takes the in-flight events back into the schedule (and tells the instruments to drop
// them), so nothing planned is lost; stop and seek drop the schedule, as a DAW re-plans from the
// new position. Notes are tracked by id from the call to their end, so all of them can be ended.

#include "midi_transport.h"
#include "midi_values.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace gaph::midi {

// What an instrument is told to do with the events it holds from one driver, from a moment on.
enum class StopMode : uint8_t {
	WITHDRAW, // drop the events not heard yet; sounding notes go on (a tempo change re-plans)
	RELEASE, // that, and a note-off for every sounding note: tails ring out (pause, stop, seek)
	CUT, // that, and silence at once (panic)
};

// Where the sequencer's output goes. The Godot side implements it: a target is an object id, and
// send/stop become calls to the target's midi_event and midi_stop.
struct Sink {
	virtual ~Sink() = default;
	virtual void send(uint64_t p_target, int64_t p_time_us, const Event &p_event) = 0;
	virtual void stop(uint64_t p_target, int64_t p_time_us, StopMode p_mode) = 0;
	// A running beat was crossed; p_song_beat is the song position there (loops fold it back).
	virtual void beat(int64_t p_running_beat, double p_song_beat) = 0;
};

// A note's life, as the driver knows it.
enum class NoteState : uint8_t {
	SCHEDULED, // its note-on is in the schedule, or in flight
	SOUNDING, // its note-on has been heard
	ENDED, // its note-off was heard, or it was ended by pause, stop, seek or panic
};

class Sequencer {
public:
	static constexpr int64_t DEFAULT_LOOKAHEAD_US = 100000;

	explicit Sequencer(Sink &p_sink) :
			sink(p_sink) {}

	Transport &transport() { return clock; }
	const Transport &transport() const { return clock; }

	void set_lookahead_us(int64_t p_us) { lookahead_us = p_us > 0 ? p_us : 0; }
	int64_t get_lookahead_us() const { return lookahead_us; }

	// Which target hears a channel (1-16); 0 for none. Notes keep the target they started with.
	void set_channel_target(int p_channel, uint64_t p_target) {
		if (p_channel >= 1 && p_channel <= 16) {
			channel_target[p_channel - 1] = p_target;
		}
	}
	uint64_t get_channel_target(int p_channel) const {
		return p_channel >= 1 && p_channel <= 16 ? channel_target[p_channel - 1] : 0;
	}

	// A note: on at p_at, and, for a p_length above 0 beats, off again that much later; held
	// until ended otherwise. Returns the note's id, or -1 if p_at is never(). Values inside.
	int64_t note_on(uint8_t p_channel, uint16_t p_key, uint32_t p_velocity, double p_length, double p_at, int64_t p_now_us) {
		if (std::isnan(p_at)) {
			return -1;
		}
		const int64_t id = next_note_id++;
		notes[id] = Note{ p_channel, p_key, 0, NoteState::SCHEDULED };
		Event on{ Kind::NOTE_ON, p_channel, p_key, p_velocity, id };
		Event off{ Kind::NOTE_OFF, p_channel, p_key, NOTE_OFF_VELOCITY, id };
		if (p_at == Transport::NOW) {
			dispatch(on, p_now_us, Transport::never());
			if (p_length > 0) {
				if (clock.is_playing()) {
					plan(off, clock.running_at(p_now_us) + p_length);
				} else {
					free_plan(off, p_now_us + int64_t(p_length * clock.us_per_beat()));
				}
			}
		} else {
			plan(on, p_at);
			if (p_length > 0) {
				plan(off, p_at + p_length);
			}
		}
		return id;
	}

	// Any other message (control, pitch bend, pressures, program) at p_at.
	void post(const Event &p_event, double p_at, int64_t p_now_us) {
		if (std::isnan(p_at)) {
			return;
		}
		if (p_at == Transport::NOW) {
			dispatch(p_event, p_now_us, Transport::never());
		} else {
			plan(p_event, p_at);
		}
	}

	NoteState note_state(int64_t p_id) const {
		auto it = notes.find(p_id);
		return it == notes.end() ? NoteState::ENDED : it->second.state;
	}

	// Ends note p_id at p_at. A note whose note-on has not gone out yet simply never starts; one
	// already handed out but not heard ends right after it starts (same sample at the earliest:
	// the instrument keeps events of one moment in the order they came).
	void end_note(int64_t p_id, double p_at, int64_t p_now_us) {
		auto it = notes.find(p_id);
		if (it == notes.end() || it->second.state == NoteState::ENDED) {
			return;
		}
		if (unplan_note(p_id, true)) { // its note-on was still waiting: drop it, and its length
			unplan_note(p_id, false);
			notes.erase(it);
			return;
		}
		unplan_note(p_id, false); // an earlier planned note-off (a length) gives way to this one
		const Note &n = it->second;
		Event off{ Kind::NOTE_OFF, n.channel, n.key, NOTE_OFF_VELOCITY, p_id };
		if (p_at == Transport::NOW || std::isnan(p_at)) {
			dispatch(off, std::max(p_now_us, on_time_in_flight(p_id)), Transport::never());
		} else {
			plan(off, p_at);
		}
	}

	// The MIDI way: every note of this driver on that channel and key.
	void note_off(uint8_t p_channel, uint16_t p_key, double p_at, int64_t p_now_us) {
		std::vector<int64_t> ids;
		for (const auto &[id, n] : notes) {
			if (n.channel == p_channel && n.key == p_key && n.state != NoteState::ENDED) {
				ids.push_back(id);
			}
		}
		for (int64_t id : ids) {
			end_note(id, p_at, p_now_us);
		}
	}

	// Every note of this driver, ended at p_at (MIDI's "all notes off", for this driver's notes).
	void end_every_note(double p_at, int64_t p_now_us) {
		std::vector<int64_t> ids;
		for (const auto &[id, n] : notes) {
			ids.push_back(id);
		}
		for (int64_t id : ids) {
			end_note(id, p_at, p_now_us);
		}
	}

	// --- The transport -----------------------------------------------------------------------

	// Starts, or resumes after pause(): then each channel's last heard controller, pitch bend,
	// pressure and program go out again first ("chasing"), so the instruments are in the state
	// the music left them in.
	void play(int64_t p_now_us) {
		if (clock.is_playing()) {
			return;
		}
		if (paused) {
			chase(p_now_us);
			paused = false;
		}
		clock.play(p_now_us);
		last_beat = int64_t(std::ceil(clock.running_at(p_now_us))) - 1;
		tick(p_now_us);
	}

	// The position stays; sounding notes end (tails ring out); what was planned plays on resume.
	void pause(int64_t p_now_us) {
		if (!clock.is_playing()) {
			return;
		}
		take_back(p_now_us, StopMode::RELEASE, true);
		end_sounding_notes();
		clock.pause(p_now_us);
		paused = true;
	}

	// Back to the start: every note ends, and the plan is dropped.
	void stop(int64_t p_now_us) { stop_with(p_now_us, StopMode::RELEASE); }

	// Like stop, but every note is silenced at once.
	void panic(int64_t p_now_us) { stop_with(p_now_us, StopMode::CUT); }

	// The song continues at p_song. Every note ends and the plan is dropped, as a DAW re-plans
	// from the new position; playing or paused, the transport stays so.
	void seek(double p_song, int64_t p_now_us) {
		take_back(p_now_us, StopMode::RELEASE, false);
		end_all_notes();
		clock.seek(p_song, p_now_us);
	}

	// A new tempo from now. Events already handed out were timed for the old one: they are
	// withdrawn (sounding notes go on) and handed out again with new times.
	void set_bpm(double p_bpm, int64_t p_now_us) {
		if (clock.is_playing()) {
			take_back(p_now_us, StopMode::WITHDRAW, true);
		}
		clock.set_bpm(p_bpm, p_now_us);
		if (clock.is_playing()) {
			tick(p_now_us);
		}
	}

	void set_loop(double p_start, double p_end, int64_t p_now_us) { clock.set_loop(p_start, p_end, p_now_us); }

	// MAIN THREAD, every frame: hand out what falls within the lookahead, note what has been
	// heard, and report the beats crossed.
	void tick(int64_t p_now_us) {
		prune(p_now_us);
		if (clock.is_playing()) {
			const double horizon = clock.running_at(p_now_us + lookahead_us);
			while (!schedule.empty() && schedule.begin()->first.first <= horizon) {
				const double r = schedule.begin()->first.first;
				const Event e = schedule.begin()->second;
				schedule.erase(schedule.begin());
				dispatch(e, std::max(p_now_us, clock.time_of(r)), r);
			}
			const int64_t beat_now = int64_t(std::floor(clock.running_at(p_now_us)));
			while (last_beat < beat_now) {
				last_beat++;
				sink.beat(last_beat, clock.song_at(double(last_beat)));
			}
		}
		while (!free_schedule.empty() && free_schedule.begin()->first.first <= p_now_us + lookahead_us) {
			const int64_t t = free_schedule.begin()->first.first;
			const Event e = free_schedule.begin()->second;
			free_schedule.erase(free_schedule.begin());
			dispatch(e, std::max(p_now_us, t), Transport::never());
		}
	}

	size_t planned_count() const { return schedule.size() + free_schedule.size(); }
	size_t in_flight_count() const { return in_flight.size(); }

private:
	static constexpr uint32_t NOTE_OFF_VELOCITY = 0x8000; // MIDI 1.0's customary 64, inside

	struct Note {
		uint8_t channel;
		uint16_t key;
		uint64_t target; // set when its note-on goes out; note-offs follow it there
		NoteState state;
	};

	// Handed out, not heard yet. `r` is its running position, or NaN for a NOW or free event.
	struct Flight {
		int64_t time_us;
		double r;
		uint64_t target;
		Event event;
	};

	// The last heard value of everything that chasing sends again, per target and channel.
	struct ChannelState {
		std::map<uint16_t, uint32_t> controls;
		bool has_bend = false, has_pressure = false, has_program = false;
		uint32_t bend = 0, pressure = 0, program = 0;
	};

	// Keyed by (position, sequence number): events at one position keep the order they came in.
	template <typename T>
	using Plan = std::multimap<std::pair<T, uint64_t>, Event>;

	Sink &sink;
	Transport clock;
	int64_t lookahead_us = DEFAULT_LOOKAHEAD_US;
	uint64_t channel_target[16] = {};
	Plan<double> schedule; // by running position
	Plan<int64_t> free_schedule; // by host time: note ends of notes played while stopped
	uint64_t next_seq = 0;
	std::vector<Flight> in_flight;
	std::map<int64_t, Note> notes; // every note not ended yet
	int64_t next_note_id = 1;
	std::map<uint64_t, std::array<ChannelState, 16>> heard_state;
	std::vector<uint64_t> targets_used; // every target ever sent to: a stop goes to all of them
	bool paused = false;
	int64_t last_beat = -1;

	void plan(const Event &p_event, double p_r) { schedule.emplace(std::make_pair(p_r, next_seq++), p_event); }
	void free_plan(const Event &p_event, int64_t p_time_us) { free_schedule.emplace(std::make_pair(p_time_us, next_seq++), p_event); }

	// Where an event goes: a note's events follow its note-on; anything else, the channel's
	// target at the moment it goes out.
	uint64_t route(const Event &p_event) const {
		if (p_event.note_id >= 0 && p_event.kind != Kind::NOTE_ON) {
			auto it = notes.find(p_event.note_id);
			if (it != notes.end() && it->second.target != 0) {
				return it->second.target;
			}
		}
		return get_channel_target(p_event.channel);
	}

	// Hands one event to its target. p_r: its running position (NaN for NOW and free events),
	// kept so pause or a tempo change can take it back into the schedule.
	void dispatch(const Event &p_event, int64_t p_time_us, double p_r) {
		const uint64_t target = route(p_event);
		if (p_event.kind == Kind::NOTE_ON) {
			auto it = notes.find(p_event.note_id);
			if (it == notes.end()) {
				return; // ended before it started
			}
			if (target == 0) {
				unplan_note(p_event.note_id, false); // nobody listens on that channel: no note
				notes.erase(it);
				return;
			}
			it->second.target = target;
		}
		if (target == 0) {
			return;
		}
		if (std::find(targets_used.begin(), targets_used.end(), target) == targets_used.end()) {
			targets_used.push_back(target);
		}
		sink.send(target, p_time_us, p_event);
		in_flight.push_back(Flight{ p_time_us, p_r, target, p_event });
	}

	// Everything in flight whose time has come is heard now: notes start and end, and the chase
	// memory takes the values.
	void prune(int64_t p_now_us) {
		size_t keep = 0;
		for (size_t i = 0; i < in_flight.size(); i++) {
			const Flight &f = in_flight[i];
			if (f.time_us > p_now_us) {
				in_flight[keep++] = f;
				continue;
			}
			heard(f);
		}
		in_flight.resize(keep);
	}

	void heard(const Flight &p_flight) {
		const Event &e = p_flight.event;
		ChannelState &st = heard_state[p_flight.target][e.channel - 1];
		switch (e.kind) {
			case Kind::NOTE_ON: {
				auto it = notes.find(e.note_id);
				if (it != notes.end() && it->second.state == NoteState::SCHEDULED) {
					it->second.state = NoteState::SOUNDING;
				}
				break;
			}
			case Kind::NOTE_OFF:
				notes.erase(e.note_id);
				break;
			case Kind::CONTROL:
				st.controls[e.index] = e.value;
				break;
			case Kind::PITCH_BEND:
				st.has_bend = true;
				st.bend = e.value;
				break;
			case Kind::CHANNEL_PRESSURE:
				st.has_pressure = true;
				st.pressure = e.value;
				break;
			case Kind::PROGRAM:
				st.has_program = true;
				st.program = e.value;
				break;
			case Kind::POLY_PRESSURE:
				break; // belongs to one note, which pause ends: nothing to chase
			case Kind::CHOKE:
				break; // never sent by a driver (instruments use it for midi_stop's CUT)
		}
	}

	// Tells every target to drop what it holds from us after p_now_us (and, by p_mode, to end
	// its notes), then takes the same events back here: into the schedule again if
	// p_return (pause, tempo change), or gone (stop, seek, panic).
	void take_back(int64_t p_now_us, StopMode p_mode, bool p_return) {
		prune(p_now_us);
		for (uint64_t t : targets_used) {
			sink.stop(t, p_now_us, p_mode);
		}
		for (const Flight &f : in_flight) {
			if (p_return && !std::isnan(f.r)) {
				plan(f.event, f.r);
			} else if (p_return) {
				free_plan(f.event, f.time_us);
			} else if (f.event.kind == Kind::NOTE_ON) {
				notes.erase(f.event.note_id);
			}
		}
		in_flight.clear();
	}

	// After a RELEASE or CUT: the notes that were sounding are over; their planned ends go too.
	void end_sounding_notes() {
		for (auto it = notes.begin(); it != notes.end();) {
			if (it->second.state == NoteState::SOUNDING) {
				unplan_note(it->first, false);
				it = notes.erase(it);
			} else {
				++it;
			}
		}
	}

	void end_all_notes() {
		schedule.clear();
		free_schedule.clear();
		notes.clear();
	}

	void stop_with(int64_t p_now_us, StopMode p_mode) {
		take_back(p_now_us, p_mode, false);
		end_all_notes();
		clock.stop();
		paused = false;
		last_beat = -1;
	}

	// Sends every target its channels' last heard values again: program first (it may reset the
	// others), then controllers, pitch bend and pressure.
	void chase(int64_t p_now_us) {
		for (auto &entry : heard_state) {
			const uint64_t target = entry.first; // a plain copy: lambdas cannot capture structured bindings before C++20
			for (uint8_t ch = 1; ch <= 16; ch++) {
				const ChannelState &st = entry.second[ch - 1];
				auto out = [&](Kind p_kind, uint16_t p_index, uint32_t p_value) {
					const Event e{ p_kind, ch, p_index, p_value, -1 };
					sink.send(target, p_now_us, e);
					in_flight.push_back(Flight{ p_now_us, Transport::never(), target, e });
				};
				if (st.has_program) {
					out(Kind::PROGRAM, 0, st.program);
				}
				for (const auto &[cc, value] : st.controls) {
					out(Kind::CONTROL, cc, value);
				}
				if (st.has_bend) {
					out(Kind::PITCH_BEND, 0, st.bend);
				}
				if (st.has_pressure) {
					out(Kind::CHANNEL_PRESSURE, 0, st.pressure);
				}
			}
		}
	}

	// Removes note p_id's planned note-on (p_on) or note-off events; true if there was one.
	bool unplan_note(int64_t p_id, bool p_on) {
		const Kind kind = p_on ? Kind::NOTE_ON : Kind::NOTE_OFF;
		bool found = false;
		auto drop = [&](auto &p_plan) {
			for (auto it = p_plan.begin(); it != p_plan.end();) {
				if (it->second.note_id == p_id && it->second.kind == kind) {
					it = p_plan.erase(it);
					found = true;
				} else {
					++it;
				}
			}
		};
		drop(schedule);
		drop(free_schedule);
		return found;
	}

	// When note p_id's note-on is due, if it is in flight; otherwise the earliest time there is.
	int64_t on_time_in_flight(int64_t p_id) const {
		for (const Flight &f : in_flight) {
			if (f.event.note_id == p_id && f.event.kind == Kind::NOTE_ON) {
				return f.time_us;
			}
		}
		return INT64_MIN;
	}
};

} // namespace gaph::midi
