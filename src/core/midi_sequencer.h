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

#include "midi_clip.h"
#include "midi_transport.h"
#include "midi_values.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <vector>

namespace gaph::midi {

// What an instrument is told to do with the events it holds from one driver, from a moment on.
enum class StopMode : uint8_t {
	WITHDRAW, // drop the events not heard yet; sounding notes go on (a tempo change re-plans)
	RELEASE, // that, and a note-off for every sounding note: tails ring out (pause, stop, seek)
	CUT, // that, and silence at once (panic)
};

// The transport as a target sees it, from a moment on: whether it plays, at what tempo, the song
// position at that moment, and the loop. In between, the target works the position out itself
// (song_beat + elapsed time x bpm / 60), so a snapshot comes only when something changes: play,
// pause, stop, seek, tempo or loop, and each loop jump (sent ahead, like events).
struct TransportInfo {
	bool playing = false;
	double bpm = 120.0;
	double song_beat = 0.0;
	bool looping = false;
	double loop_start = 0.0, loop_end = 0.0;
};

// Where the sequencer's output goes. The Godot side implements it: a target is an object id, and
// send/stop/transport become calls to the target's midi_event, midi_stop and midi_transport.
struct Sink {
	virtual ~Sink() = default;
	virtual void send(uint64_t p_target, int64_t p_time_us, const Event &p_event) = 0;
	virtual void stop(uint64_t p_target, int64_t p_time_us, StopMode p_mode) = 0;
	// A running beat was crossed; p_song_beat is the song position there (loops fold it back).
	virtual void beat(int64_t p_running_beat, double p_song_beat) = 0;
	// The song passed the loop's end at running position p_running and went back to p_song_beat.
	virtual void looped(double p_running, double p_song_beat) = 0;
	// The transport from p_time_us on, for one target.
	virtual void transport(uint64_t p_target, int64_t p_time_us, const TransportInfo &p_info) = 0;
	// A clip's tempo change was heard (the transport now runs at p_bpm).
	virtual void tempo(double p_bpm) { (void)p_bpm; }
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
	// A target new to this driver is told the transport's state straight away (p_now_us).
	void set_channel_target(int p_channel, uint64_t p_target, int64_t p_now_us = 0) {
		if (p_channel < 1 || p_channel > 16) {
			return;
		}
		const bool known = p_target == 0 || std::find(std::begin(channel_target), std::end(channel_target), p_target) != std::end(channel_target) ||
				std::find(targets_used.begin(), targets_used.end(), p_target) != targets_used.end();
		channel_target[p_channel - 1] = p_target;
		if (!known) {
			sink.transport(p_target, p_now_us, info_at(clock.running_at(p_now_us)));
		}
	}
	// A target that hears only the transport (a tempo-synced effect): no notes, the same snapshots
	// as instruments, told the state at once.
	void add_transport_target(uint64_t p_target, int64_t p_now_us) {
		if (p_target == 0 || std::find(transport_only.begin(), transport_only.end(), p_target) != transport_only.end()) {
			return;
		}
		transport_only.push_back(p_target);
		clock.settle(p_now_us);
		sink.transport(p_target, p_now_us, info_at(clock.running_at(p_now_us)));
	}
	void remove_transport_target(uint64_t p_target) {
		transport_only.erase(std::remove(transport_only.begin(), transport_only.end(), p_target), transport_only.end());
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
		notes[id] = Note{ p_channel, p_key, 0, NoteState::SCHEDULED, p_velocity };
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
		// Waiting to be chased on resume (already silent): ending it just forgets it.
		auto waiting = std::find(chased.begin(), chased.end(), p_id);
		if (waiting != chased.end()) {
			chased.erase(waiting);
			unplan_note(p_id, false);
			notes.erase(it);
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

	// CHASING NOTES (off by default, as in most DAWs). Pause ends every sounding note. With
	// chase_notes on, those that would still be sounding start again on resume: the same note (its
	// id, channel, key, velocity), and its planned end, if it has one, still comes at its beat.
	void set_chase_notes(bool p_on) { chase_notes = p_on; }
	bool get_chase_notes() const { return chase_notes; }

	// Starts, or resumes after pause(): then each channel's last heard controller, pitch bend,
	// pressure and program go out again first ("chasing"), so the instruments are in the state
	// the music left them in; and, with chase_notes, the notes the pause ended start again.
	void play(int64_t p_now_us) {
		if (clock.is_playing()) {
			return;
		}
		if (paused) {
			chase(p_now_us);
			for (int64_t id : chased) {
				auto it = notes.find(id);
				if (it != notes.end()) {
					dispatch(Event{ Kind::NOTE_ON, it->second.channel, it->second.key, it->second.velocity, id }, p_now_us, Transport::never());
				}
			}
			chased.clear();
			paused = false;
		}
		clock.play(p_now_us);
		last_beat = int64_t(std::ceil(clock.running_at(p_now_us))) - 1;
		last_running = clock.running_at(p_now_us);
		announce(p_now_us);
		tick(p_now_us);
	}

	// The position stays; sounding notes end (tails ring out); what was planned plays on resume.
	void pause(int64_t p_now_us) {
		if (!clock.is_playing()) {
			return;
		}
		take_back(p_now_us, StopMode::RELEASE, true);
		if (chase_notes) {
			keep_sounding_notes_for_chasing();
		} else {
			end_sounding_notes();
		}
		clock.pause(p_now_us);
		paused = true;
		announce(p_now_us);
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
		last_running = clock.running_at(p_now_us);
		restart_parts(p_now_us, true);
		announce(p_now_us);
	}

	bool is_paused() const { return paused; }

	// A new tempo from now. Events already handed out were timed for the old one: they are
	// withdrawn (sounding notes go on) and handed out again with new times.
	void set_bpm(double p_bpm, int64_t p_now_us) {
		if (clock.is_playing()) {
			take_back(p_now_us, StopMode::WITHDRAW, true);
		}
		clock.set_bpm(p_bpm, p_now_us);
		announce(p_now_us);
		if (clock.is_playing()) {
			tick(p_now_us);
		}
	}

	// A loop changes what is ahead: loop jumps already sent are taken back with everything else not
	// heard yet (sounding notes go on), and sent again for the new loop.
	void set_loop(double p_start, double p_end, int64_t p_now_us) {
		if (clock.is_playing()) {
			take_back(p_now_us, StopMode::WITHDRAW, true);
		}
		clock.set_loop(p_start, p_end, p_now_us);
		replan_song_parts(p_now_us);
		announce(p_now_us);
		if (clock.is_playing()) {
			tick(p_now_us);
		}
	}

	// MAIN THREAD, every frame: hand out what falls within the lookahead, note what has been
	// heard, and report the beats crossed.
	void tick(int64_t p_now_us) {
		prune(p_now_us);
		clock.settle(p_now_us);
		if (clock.is_playing()) {
			const double horizon = clock.running_at(p_now_us + lookahead_us);
			plan_parts(p_now_us, horizon);
			while (!schedule.empty() && schedule.begin()->first.first <= horizon) {
				const double r = schedule.begin()->first.first;
				const Event e = schedule.begin()->second;
				schedule.erase(schedule.begin());
				dispatch(e, std::max(p_now_us, clock.time_of(r)), r);
			}
			const double running_now = clock.running_at(p_now_us);
			const int64_t beat_now = int64_t(std::floor(running_now));
			while (last_beat < beat_now) {
				last_beat++;
				sink.beat(last_beat, clock.song_at(double(last_beat)));
			}
			// Loop jumps within the lookahead, sent ahead like events, so a target folds its song
			// position back on the exact sample.
			for (double jump = clock.next_loop_jump_after(jumps_sent_upto); !std::isnan(jump) && jump <= horizon;
					jump = clock.next_loop_jump_after(jump)) {
				TransportInfo info = info_at(jump);
				info.song_beat = clock.get_loop_start();
				for (uint64_t t : all_targets()) {
					sink.transport(t, clock.time_of(jump), info);
				}
				jumps_sent_upto = jump;
			}
			// Loop jumps since the last tick (one, or several after a long frame).
			for (double jump = clock.next_loop_jump_after(last_running); !std::isnan(jump) && jump <= running_now;
					jump = clock.next_loop_jump_after(jump)) {
				sink.looped(jump, clock.get_loop_start());
			}
			last_running = running_now;
		}
		while (!free_schedule.empty() && free_schedule.begin()->first.first <= p_now_us + lookahead_us) {
			const int64_t t = free_schedule.begin()->first.first;
			const Event e = free_schedule.begin()->second;
			free_schedule.erase(free_schedule.begin());
			dispatch(e, std::max(p_now_us, t), Transport::never());
		}
	}

	// --- Clips --------------------------------------------------------------------------------
	//
	// A PART is a clip playing: its events are worked out a lookahead ahead, as the transport
	// gets there, never all at once, so a long song or an endless pattern costs nothing up front.
	// - On the SONG (add_part with p_on_song): placed at song position p_start, as a part on a
	//   DAW's timeline. Seek and loop move through it as through the music: started in the
	//   middle, it first sends the controllers, programs, pitch bend and tempo in force there
	//   ("chasing"); at a loop's end its notes end, and it carries on from the loop's start.
	//   Stop, seek and loop keep it; only remove_part() takes it away.
	// - From a running position (an `at`, like a note's): a stinger on the next bar, say. It plays
	//   through whatever the song does (a loop does not fold it), like notes planned with `at`, and
	//   like them is dropped by stop and seek.
	// p_repeats: how many times it plays, one after the other, each clip.length beats long; 0 for
	// ever. p_follow_tempo: the clip's tempo map sets the transport's tempo as it plays.
	int32_t add_part(std::shared_ptr<const Clip> p_clip, bool p_on_song, double p_start, int p_repeats, bool p_follow_tempo, int64_t p_now_us) {
		if (!p_clip || std::isnan(p_start)) {
			return -1;
		}
		clock.settle(p_now_us);
		Part part;
		part.clip = std::move(p_clip);
		part.on_song = p_on_song;
		part.start = p_on_song || p_start != Transport::NOW ? p_start : clock.running_at(p_now_us);
		part.repeats = p_repeats > 0 ? p_repeats : 0;
		part.follow_tempo = p_follow_tempo;
		part.cursor = clock.running_at(p_now_us);
		part.track_muted.assign(part.clip->track_names.size(), false);
		for (int c = 0; c < 16; c++) {
			part.channel_map[c] = uint8_t(c + 1);
		}
		const int32_t id = next_part++;
		parts[id] = part;
		if (clock.is_playing()) {
			tick(p_now_us);
		}
		return id;
	}

	// Takes a part away: what it planned and has not handed out yet goes, and so do its notes not
	// yet heard; notes already sounding end at p_now_us.
	void remove_part(int32_t p_id, int64_t p_now_us) {
		if (!parts.count(p_id)) {
			return;
		}
		unplan_part(p_id, false);
		std::vector<int64_t> left; // handed out or sounding: they end now
		for (const auto &[id, n] : notes) {
			if (n.part == p_id) {
				left.push_back(id);
			}
		}
		for (int64_t id : left) {
			end_note(id, Transport::NOW, p_now_us);
		}
		parts.erase(p_id);
	}

	bool has_part(int32_t p_id) const { return parts.count(p_id) != 0; }

	// Muting stops new notes and messages; the ones sounding play on to their ends.
	void set_part_muted(int32_t p_id, bool p_muted) {
		if (parts.count(p_id)) {
			parts[p_id].muted = p_muted;
		}
	}
	bool is_part_muted(int32_t p_id) const { return parts.count(p_id) && parts.at(p_id).muted; }
	void set_part_track_muted(int32_t p_id, int p_track, bool p_muted) {
		auto it = parts.find(p_id);
		if (it != parts.end() && p_track >= 0 && size_t(p_track) < it->second.track_muted.size()) {
			it->second.track_muted[size_t(p_track)] = p_muted;
		}
	}
	bool is_part_track_muted(int32_t p_id, int p_track) const {
		auto it = parts.find(p_id);
		return it != parts.end() && p_track >= 0 && size_t(p_track) < it->second.track_muted.size() && it->second.track_muted[size_t(p_track)];
	}
	void set_part_follow_tempo(int32_t p_id, bool p_follow) {
		if (parts.count(p_id)) {
			parts[p_id].follow_tempo = p_follow;
		}
	}
	bool get_part_follow_tempo(int32_t p_id) const { return parts.count(p_id) && parts.at(p_id).follow_tempo; }

	// The clip's channel p_from (1-16) plays on p_to instead, for events planned from now on.
	void set_part_channel(int32_t p_id, int p_from, int p_to) {
		auto it = parts.find(p_id);
		if (it != parts.end() && p_from >= 1 && p_from <= 16 && p_to >= 1 && p_to <= 16) {
			it->second.channel_map[p_from - 1] = uint8_t(p_to);
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
		uint32_t velocity; // kept for chasing: a chased note starts again as it started
		int32_t part = -1; // the clip part that played it, or -1
	};

	struct Part {
		std::shared_ptr<const Clip> clip;
		bool on_song = true;
		double start = 0; // a song position (on_song), or a running position
		int repeats = 1; // 0: for ever
		bool follow_tempo = true;
		bool muted = false;
		std::vector<bool> track_muted;
		uint8_t channel_map[16] = {};
		double cursor = 0; // running position: events before it have been planned
		bool fresh = true; // next planning starts at a jump: chase first
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
	std::vector<uint64_t> transport_only; // targets of the transport alone (add_transport_target)
	bool paused = false;
	bool chase_notes = false;
	std::vector<int64_t> chased; // notes the pause ended, to start again on resume (chase_notes)
	std::map<int32_t, Part> parts;
	int32_t next_part = 1;
	int64_t last_beat = -1;
	double last_running = 0; // the running position at the last tick, for spotting loop jumps
	double jumps_sent_upto = 0; // loop jumps up to this running position have gone to the targets

	TransportInfo info_at(double p_running) const {
		TransportInfo info;
		info.playing = clock.is_playing();
		info.bpm = clock.bpm_at(p_running);
		info.song_beat = clock.song_at(p_running);
		info.looping = clock.is_looping();
		info.loop_start = clock.get_loop_start();
		info.loop_end = clock.get_loop_end();
		return info;
	}

	// Every target this driver feeds: the channels' current ones and any it ever sent to.
	std::vector<uint64_t> all_targets() const {
		std::vector<uint64_t> out = targets_used;
		for (uint64_t t : transport_only) {
			if (std::find(out.begin(), out.end(), t) == out.end()) {
				out.push_back(t);
			}
		}
		for (uint64_t t : channel_target) {
			if (t != 0 && std::find(out.begin(), out.end(), t) == out.end()) {
				out.push_back(t);
			}
		}
		return out;
	}

	// The transport changed at p_now_us: every target hears it; loop jumps are sent afresh.
	void announce(int64_t p_now_us) {
		const double r = clock.running_at(p_now_us);
		const TransportInfo info = info_at(r);
		for (uint64_t t : all_targets()) {
			sink.transport(t, p_now_us, info);
		}
		jumps_sent_upto = r;
	}

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
		if (p_event.kind == Kind::TEMPO) {
			// The clock learns it ahead (exact on both sides of it), and every target gets the
			// transport from that moment on. Kept in flight, so pause can take it back.
			if (std::isnan(p_r) || p_event.value == 0) {
				return;
			}
			clock.tempo_at(p_r, 6e9 / double(p_event.value));
			const int64_t at = clock.time_of(p_r);
			const TransportInfo info = info_at(p_r);
			for (uint64_t t : all_targets()) {
				sink.transport(t, at, info);
			}
			in_flight.push_back(Flight{ at, p_r, 0, p_event });
			return;
		}
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
		if (e.kind == Kind::TEMPO) {
			sink.tempo(6e9 / double(e.value));
			return;
		}
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
			case Kind::TEMPO:
				break; // handled above
			case Kind::NOTE_EXPRESSION:
				break; // belongs to one note: nothing to chase
		}
	}

	// Tells every target to drop what it holds from us after p_now_us (and, by p_mode, to end
	// its notes), then takes the same events back here: into the schedule again if
	// p_return (pause, tempo change), or gone (stop, seek, panic).
	void take_back(int64_t p_now_us, StopMode p_mode, bool p_return) {
		prune(p_now_us);
		clock.settle(p_now_us); // tempo changes already reached stay; those ahead come back below
		clock.drop_pending();
		for (uint64_t t : targets_used) {
			sink.stop(t, p_now_us, p_mode);
		}
		for (uint64_t t : transport_only) { // their snapshots ahead (loop jumps, tempo) are withdrawn too
			if (std::find(targets_used.begin(), targets_used.end(), t) == targets_used.end()) {
				sink.stop(t, p_now_us, StopMode::WITHDRAW);
			}
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

	// With chase_notes: the notes that were sounding wait for the resume (their planned ends stay).
	void keep_sounding_notes_for_chasing() {
		for (auto &[id, n] : notes) {
			if (n.state == NoteState::SOUNDING) {
				n.state = NoteState::SCHEDULED;
				chased.push_back(id);
			}
		}
	}

	void end_all_notes() {
		schedule.clear();
		free_schedule.clear();
		notes.clear();
		chased.clear();
	}

	void stop_with(int64_t p_now_us, StopMode p_mode) {
		take_back(p_now_us, p_mode, false);
		end_all_notes();
		clock.stop();
		paused = false;
		last_beat = -1;
		restart_parts(p_now_us, true);
		announce(p_now_us);
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

	// --- Clip parts, inside ---------------------------------------------------------------------

	static constexpr double FOREVER = std::numeric_limits<double>::infinity();

	static double part_length(const Part &p_part) { return p_part.clip->length > 0 ? p_part.clip->length : FOREVER; }
	static double part_end(const Part &p_part) {
		const double length = part_length(p_part);
		return p_part.repeats > 0 && length != FOREVER ? p_part.start + length * p_part.repeats : FOREVER;
	}

	// Drops what part p_id planned and has not handed out: its notes not handed out go with their
	// note-offs; a note already handed out or sounding keeps its note-off.
	void unplan_part(int32_t p_id, bool p_tempo_too) {
		std::vector<int64_t> dropped;
		auto drop = [&](auto &p_plan) {
			for (auto it = p_plan.begin(); it != p_plan.end();) {
				const Event &e = it->second;
				if (e.part != p_id || e.kind == Kind::NOTE_OFF || (e.kind == Kind::TEMPO && !p_tempo_too)) {
					++it;
					continue;
				}
				if (e.kind == Kind::NOTE_ON) {
					dropped.push_back(e.note_id);
				}
				it = p_plan.erase(it);
			}
		};
		drop(schedule);
		drop(free_schedule);
		for (int64_t id : dropped) {
			unplan_note(id, false);
			notes.erase(id);
		}
	}

	// After stop or seek: parts on the song carry on from where the song is now, chasing first;
	// parts from a running position go, like notes planned with `at` (p_drop_running).
	void restart_parts(int64_t p_now_us, bool p_drop_running) {
		for (auto it = parts.begin(); it != parts.end();) {
			if (!it->second.on_song && p_drop_running) {
				it = parts.erase(it);
				continue;
			}
			it->second.cursor = clock.running_at(p_now_us);
			it->second.fresh = true;
			++it;
		}
	}

	// A new loop changes what lies ahead on the song: the song parts plan again from now.
	void replan_song_parts(int64_t p_now_us) {
		for (auto &[id, part] : parts) {
			if (part.on_song) {
				unplan_part(id, true);
				part.cursor = clock.running_at(p_now_us);
				part.fresh = true;
			}
		}
	}

	// One clip event of part p_id, planned at running position p_r. A note ends at its length, or
	// at p_clamp (a loop's end) if that comes first.
	void emit(int32_t p_id, const Part &p_part, const ClipEvent &p_e, double p_r, double p_clamp) {
		if (p_part.muted || (p_e.track < p_part.track_muted.size() && p_part.track_muted[p_e.track])) {
			return;
		}
		const uint8_t ch = p_part.channel_map[(p_e.channel - 1) & 15];
		if (p_e.kind == Kind::NOTE_ON) {
			const int64_t id = next_note_id++;
			Note n{ ch, p_e.index, 0, NoteState::SCHEDULED, p_e.value };
			n.part = p_id;
			notes[id] = n;
			plan(Event{ Kind::NOTE_ON, ch, p_e.index, p_e.value, id, p_id }, p_r);
			plan(Event{ Kind::NOTE_OFF, ch, p_e.index, NOTE_OFF_VELOCITY, id, p_id }, std::max(p_r, std::min(p_r + p_e.length, p_clamp)));
			return;
		}
		plan(Event{ p_e.kind, ch, p_e.index, p_e.value, -1, p_id }, p_r);
	}

	void emit_tempo(int32_t p_id, uint32_t p_ten_ns, double p_r) {
		plan(Event{ Kind::TEMPO, 1, 0, p_ten_ns, -1, p_id }, p_r);
	}

	// Plans what part p_id holds between positions p_x0 and p_x1 on its own axis (song positions
	// for a song part, running positions otherwise), the stretch that starts at running position
	// p_r0: position x is due at p_r0 + (x - p_x0).
	void emit_span(int32_t p_id, const Part &p_part, double p_x0, double p_x1, double p_r0, double p_clamp) {
		const Clip &c = *p_part.clip;
		const double length = part_length(p_part);
		const double lo = std::max(p_x0, p_part.start), hi = std::min(p_x1, part_end(p_part));
		if (!(hi > lo)) {
			return;
		}
		for (int64_t k = length == FOREVER ? 0 : int64_t(std::floor((lo - p_part.start) / length));; k++) {
			const double base = length == FOREVER ? p_part.start : p_part.start + double(k) * length;
			if (base >= hi) {
				break;
			}
			const double from = std::max(lo - base, 0.0), to = std::min(hi - base, length);
			if (p_part.follow_tempo) {
				for (const ClipTempo &t : c.tempos) {
					if (t.beat >= from && t.beat < to) {
						emit_tempo(p_id, t.ten_ns_per_beat, p_r0 + (base + t.beat - p_x0));
					}
				}
			}
			for (size_t i = c.first_at(from); i < c.events.size() && c.events[i].beat < to; i++) {
				emit(p_id, p_part, c.events[i], p_r0 + (base + c.events[i].beat - p_x0), p_clamp);
			}
			if (length == FOREVER) {
				break;
			}
		}
	}

	// Starting part p_id at position p_x on its axis (after a seek, a loop jump, or placed in the
	// past): what the clip set before p_x is sent first, at running position p_r: per channel the
	// program, controllers, pitch bend and pressure, and the tempo. Right at the part's start, or
	// outside it, there is nothing to chase.
	void chase_part(int32_t p_id, const Part &p_part, double p_x, double p_r) {
		const Clip &c = *p_part.clip;
		if (!(p_x > p_part.start) || p_x >= part_end(p_part)) {
			return;
		}
		const double length = part_length(p_part);
		const double rel = p_x - p_part.start;
		const int64_t k = length == FOREVER ? 0 : int64_t(std::floor(rel / length));
		const double q = rel - (length == FOREVER ? 0.0 : double(k) * length);
		struct Last {
			std::map<uint16_t, const ClipEvent *> controls;
			const ClipEvent *program = nullptr, *bend = nullptr, *pressure = nullptr;
		} last[16];
		auto take = [&](const ClipEvent &e) {
			Last &l = last[(e.channel - 1) & 15];
			switch (e.kind) {
				case Kind::CONTROL: l.controls[e.index] = &e; break;
				case Kind::PROGRAM: l.program = &e; break;
				case Kind::PITCH_BEND: l.bend = &e; break;
				case Kind::CHANNEL_PRESSURE: l.pressure = &e; break;
				default: break;
			}
		};
		if (k > 0) {
			for (const ClipEvent &e : c.events) {
				take(e); // a repeat starts with what the previous time round left
			}
		}
		for (size_t i = 0; i < c.events.size() && c.events[i].beat < q; i++) {
			take(c.events[i]);
		}
		if (p_part.follow_tempo && !c.tempos.empty()) {
			uint32_t tempo = k > 0 ? c.tempos.back().ten_ns_per_beat : 50000000;
			for (const ClipTempo &t : c.tempos) {
				if (t.beat < q) {
					tempo = t.ten_ns_per_beat;
				}
			}
			emit_tempo(p_id, tempo, p_r);
		}
		for (const Last &l : last) { // program first: it may reset the rest
			if (l.program) {
				emit(p_id, p_part, *l.program, p_r, FOREVER);
			}
			for (const auto &entry : l.controls) {
				emit(p_id, p_part, *entry.second, p_r, FOREVER);
			}
			if (l.bend) {
				emit(p_id, p_part, *l.bend, p_r, FOREVER);
			}
			if (l.pressure) {
				emit(p_id, p_part, *l.pressure, p_r, FOREVER);
			}
		}
	}

	// MAIN THREAD, in tick: every part plans its events up to running position p_horizon.
	void plan_parts(int64_t p_now_us, double p_horizon) {
		for (auto it = parts.begin(); it != parts.end();) {
			const int32_t id = it->first;
			Part &part = it->second;
			if (part.on_song) {
				plan_song_part(id, part, p_horizon);
				++it;
				continue;
			}
			// From a running position: no loop folds it.
			const double from = std::max(part.cursor, part.start), to = std::min(p_horizon, part_end(part));
			if (to > from) {
				if (part.fresh) {
					chase_part(id, part, from, from);
					part.fresh = false;
				}
				emit_span(id, part, from, to, from, FOREVER);
				part.cursor = to;
			}
			// Done once it has planned everything and the music has passed its end.
			const double end = part_end(part);
			if (end != FOREVER && part.cursor >= end && clock.running_at(p_now_us) >= end) {
				it = parts.erase(it);
				continue;
			}
			++it;
		}
	}

	// A song part, from its cursor to p_horizon, one stretch per loop round: a stretch ends at the
	// loop's end (its notes end there too), and the next starts at the loop's start, chasing.
	void plan_song_part(int32_t p_id, Part &p_part, double p_horizon) {
		double a = p_part.cursor;
		while (a < p_horizon) {
			const double jump = clock.next_loop_jump_after(a);
			const bool jumps = !std::isnan(jump) && jump <= p_horizon;
			const double b = jumps ? jump : p_horizon;
			const double song = clock.song_at(a);
			if (p_part.fresh) {
				chase_part(p_id, p_part, song, a);
				p_part.fresh = false;
			}
			emit_span(p_id, p_part, song, song + (b - a), a, std::isnan(jump) ? FOREVER : jump);
			p_part.fresh = jumps;
			a = b;
		}
		p_part.cursor = std::max(p_part.cursor, a);
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
