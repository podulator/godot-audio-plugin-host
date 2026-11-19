// Standalone tests for the MIDI driver's core (src/core/midi_*.h), with made-up times, so every
// result is exact to the microsecond. Build and run: tests/core/run.sh
#include "core/midi_expression.h"
#include "core/midi_file.h"
#include "core/midi_sequencer.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <cstdio>
#include <string>
#include <vector>

using namespace gaph::midi;

static int failures = 0;
#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                 \
		}                                                               \
	} while (0)

// Everything the sequencer sends, in order.
struct Recorder : Sink {
	struct Sent {
		uint64_t target;
		int64_t time_us;
		Event event;
		uint64_t order = 0;
	};
	struct Stopped {
		uint64_t target;
		int64_t time_us;
		StopMode mode;
		uint64_t order = 0;
	};
	uint64_t order = 0;
	std::vector<Sent> sent;
	std::vector<Stopped> stops;
	std::vector<std::pair<int64_t, double>> beats;
	void send(uint64_t p_target, int64_t p_time_us, const Event &p_event) override { sent.push_back({ p_target, p_time_us, p_event, order++ }); }
	void stop(uint64_t p_target, int64_t p_time_us, StopMode p_mode) override { stops.push_back({ p_target, p_time_us, p_mode, order++ }); }
	void beat(int64_t p_running, double p_song) override { beats.push_back({ p_running, p_song }); }
	std::vector<std::pair<double, double>> jumps;
	void looped(double p_running, double p_song) override { jumps.push_back({ p_running, p_song }); }
	struct Snapshot {
		uint64_t target;
		int64_t time_us;
		TransportInfo info;
	};
	std::vector<Snapshot> transports;
	void transport(uint64_t p_target, int64_t p_time_us, const TransportInfo &p_info) override { transports.push_back({ p_target, p_time_us, p_info }); }
	// What a target really heard of a kind: sent, and not withdrawn by a later stop (a stop drops
	// what its target holds from the stop's time on).
	std::vector<Sent> heard(Kind p_kind) const {
		std::vector<Sent> out;
		for (const Sent &s : sent) {
			bool withdrawn = false;
			for (const Stopped &st : stops) {
				withdrawn = withdrawn || (st.target == s.target && st.order > s.order && st.time_us <= s.time_us);
			}
			if (s.event.kind == p_kind && !withdrawn) {
				out.push_back(s);
			}
		}
		return out;
	}
	// What was sent of a kind; with p_before, only what is due before that time (the lookahead
	// hands events out about 100 ms early).
	std::vector<Sent> of(Kind p_kind, int64_t p_before = INT64_MAX) const {
		std::vector<Sent> out;
		for (const Sent &s : sent) {
			if (s.event.kind == p_kind && s.time_us < p_before) {
				out.push_back(s);
			}
		}
		return out;
	}
};

static const int64_t T0 = 10000000; // play() at 10 s on the host clock
static const int64_t BEAT = 500000; // 120 bpm: 500 ms a beat
static const uint64_t PIANO = 11, DRUMS = 22;

// Runs the driver's frame loop from p_from to p_to, one tick per 60 fps frame.
static void run(Sequencer &p_seq, int64_t p_from, int64_t p_to) {
	for (int64_t t = p_from; t <= p_to; t += 16667) {
		p_seq.tick(t);
	}
}

static void test_values() {
	CHECK(scale_up(127, 7, 16) == 0xFFFF && scale_up(64, 7, 16) == 0x8000 && scale_up(0, 7, 16) == 0);
	CHECK(scale_up(8192, 14, 32) == 0x80000000u && scale_up(16383, 14, 32) == 0xFFFFFFFFu);
	bool ok = false;
	CHECK(to_inside(Kind::PITCH_BEND, 0, 1, ok) == 0x80000000u && ok); // no bend stays no bend
	to_inside(Kind::NOTE_ON, 128, 1, ok);
	CHECK(!ok);
	for (uint32_t v = 0; v < 128; v++) {
		CHECK(scale_down(scale_up(v, 7, 32), 32, 7) == v);
	}
}

// A note on the next beat, half a beat long: on and off land exactly on the grid, and only the
// lookahead ahead of time is handed out.
static void test_grid_and_length() {
	Recorder rec;
	Sequencer seq(rec);
	seq.set_channel_target(1, PIANO);
	seq.play(T0);
	const int64_t now = T0 + 100000; // 0.2 beats in
	seq.tick(now);
	const double at = seq.transport().next(1.0, now);
	CHECK(at == 1.0);
	seq.note_on(1, 60, 0x8000, 0.5, at, now);
	CHECK(rec.of(Kind::NOTE_ON).empty()); // 400 ms ahead: not handed out yet
	run(seq, now, T0 + 2 * BEAT);
	auto on = rec.of(Kind::NOTE_ON), off = rec.of(Kind::NOTE_OFF);
	CHECK(on.size() == 1 && on[0].time_us == T0 + BEAT && on[0].target == PIANO);
	CHECK(off.size() == 1 && off[0].time_us == T0 + BEAT + BEAT / 2);
	CHECK(seq.in_flight_count() == 0 && seq.planned_count() == 0);
}

// Pause at beat 2 for 3 s, resume. A note planned for beat 4 plays 2 beats after the resume; one
// at beat 2.1, already handed out when the pause came, is taken back (the instrument is told to
// drop it) and handed out again; the mod wheel heard before the pause goes out again on resume.
static void test_pause_resume() {
	Recorder rec;
	Sequencer seq(rec);
	seq.set_channel_target(1, PIANO);
	seq.play(T0);
	seq.post(Event{ Kind::CONTROL, 1, 1, 0x40000000, -1 }, Transport::NOW, T0);
	const int64_t far = seq.note_on(1, 64, 0x8000, 0, 4.0, T0);
	const int64_t near = seq.note_on(1, 62, 0x8000, 0, 2.1, T0);
	const int64_t pause_at = T0 + 2 * BEAT;
	run(seq, T0, pause_at - 1);
	CHECK(rec.of(Kind::NOTE_ON).size() == 1); // beat 2.1 is within the lookahead now
	seq.pause(pause_at);
	CHECK(rec.stops.size() == 1 && rec.stops[0].time_us == pause_at && rec.stops[0].mode == StopMode::RELEASE);
	CHECK(seq.note_state(near) == NoteState::SCHEDULED && seq.note_state(far) == NoteState::SCHEDULED);
	run(seq, pause_at, pause_at + 3000000); // paused: nothing goes out
	CHECK(rec.of(Kind::NOTE_ON).size() == 1 && rec.of(Kind::CONTROL).size() == 1);

	const int64_t resume_at = pause_at + 3000000;
	seq.play(resume_at);
	auto cc = rec.of(Kind::CONTROL);
	CHECK(cc.size() == 2 && cc[1].time_us == resume_at && cc[1].event.value == 0x40000000); // chased
	run(seq, resume_at, resume_at + 3 * BEAT);
	auto on = rec.of(Kind::NOTE_ON);
	CHECK(on.size() == 3);
	if (on.size() == 3) {
		CHECK(on[1].event.note_id == near && on[1].time_us == resume_at + BEAT / 10);
		CHECK(on[2].event.note_id == far && on[2].time_us == resume_at + 2 * BEAT);
	}
	CHECK(seq.note_state(far) == NoteState::SOUNDING);
}

// Stop and seek drop the plan and end every note; a sounding note gets its note-off through the
// stop (RELEASE), not as an event of its own.
static void test_stop_and_seek() {
	Recorder rec;
	Sequencer seq(rec);
	seq.set_channel_target(1, PIANO);
	seq.play(T0);
	const int64_t held = seq.note_on(1, 60, 0x8000, 0, Transport::NOW, T0);
	const int64_t later = seq.note_on(1, 67, 0x8000, 0, 8.0, T0);
	run(seq, T0, T0 + BEAT);
	CHECK(seq.note_state(held) == NoteState::SOUNDING);
	seq.seek(16.0, T0 + BEAT);
	CHECK(rec.stops.size() == 1 && rec.stops[0].mode == StopMode::RELEASE);
	CHECK(seq.note_state(held) == NoteState::ENDED && seq.note_state(later) == NoteState::ENDED);
	CHECK(seq.planned_count() == 0);
	CHECK(seq.transport().is_playing());
	CHECK(std::abs(seq.transport().song_at(seq.transport().running_at(T0 + BEAT)) - 16.0) < 1e-9);
	seq.panic(T0 + 2 * BEAT);
	CHECK(rec.stops.back().mode == StopMode::CUT && !seq.transport().is_playing());
}

// A tempo change re-times what was handed out (WITHDRAW: the sounding note goes on) and
// everything after it. 120 -> 60 bpm at beat 1: beat 2 is then 1 s later, not 0.5 s.
static void test_tempo_change() {
	Recorder rec;
	Sequencer seq(rec);
	seq.set_channel_target(1, PIANO);
	seq.play(T0);
	const int64_t held = seq.note_on(1, 60, 0x8000, 0, Transport::NOW, T0);
	seq.note_on(1, 62, 0x8000, 0, 1.1, T0);
	seq.note_on(1, 64, 0x8000, 0, 2.0, T0);
	run(seq, T0, T0 + BEAT - 1); // beat 1.1 already handed out
	seq.set_bpm(60, T0 + BEAT);
	CHECK(rec.stops.size() == 1 && rec.stops[0].mode == StopMode::WITHDRAW);
	run(seq, T0 + BEAT, T0 + 3 * BEAT);
	auto on = rec.of(Kind::NOTE_ON); // the held one, 1.1 handed out twice (old time, new time), 2.0
	CHECK(on.size() == 4);
	if (on.size() == 4) {
		CHECK(on[2].time_us == T0 + BEAT + 100000); // 0.1 beat at 60 bpm
		CHECK(on[3].time_us == T0 + BEAT + 1000000);
	}
	CHECK(seq.note_state(held) == NoteState::SOUNDING);
}

// One target on two channels; a note's end follows its note-on even after the channel is
// re-routed; a note ended before it starts never goes out; no target, no note.
static void test_routing() {
	Recorder rec;
	Sequencer seq(rec);
	seq.set_channel_target(10, DRUMS);
	seq.set_channel_target(11, DRUMS);
	seq.play(T0);
	seq.note_on(10, 36, 0xFFFF, 0, Transport::NOW, T0);
	const int64_t snare = seq.note_on(11, 38, 0xFFFF, 0, Transport::NOW, T0);
	CHECK(rec.sent.size() == 2 && rec.sent[0].target == DRUMS && rec.sent[1].target == DRUMS && rec.sent[1].event.channel == 11);
	seq.set_channel_target(11, PIANO);
	seq.end_note(snare, Transport::NOW, T0 + 1000);
	CHECK(rec.sent.size() == 3 && rec.sent[2].event.kind == Kind::NOTE_OFF && rec.sent[2].target == DRUMS);

	const int64_t never_heard = seq.note_on(10, 42, 0xFFFF, 1.0, 4.0, T0);
	seq.end_note(never_heard, Transport::NOW, T0 + 2000);
	run(seq, T0, T0 + 6 * BEAT);
	CHECK(rec.sent.size() == 3 && seq.note_state(never_heard) == NoteState::ENDED);

	const int64_t nobody = seq.note_on(5, 60, 0x8000, 0, Transport::NOW, T0 + 6 * BEAT);
	CHECK(rec.sent.size() == 3 && seq.note_state(nobody) == NoteState::ENDED);
}

// A loop over beats 4 to 8. Before it, at_song(5) is simply beat 5; inside, the next time round;
// outside it, never. Beat signals carry the song position, folded back.
static void test_loop() {
	Recorder rec;
	Sequencer seq(rec);
	seq.play(T0);
	seq.set_loop(4.0, 8.0, T0);
	Transport &tr = seq.transport();
	CHECK(tr.at_song(5.0, T0) == 5.0);
	const int64_t at_9 = T0 + 9 * BEAT; // running 9 = song 5, on the second time round
	CHECK(std::abs(tr.song_at(tr.running_at(at_9)) - 5.0) < 1e-9);
	CHECK(std::abs(tr.at_song(5.0, at_9 + 1) - 13.0) < 1e-9); // just passed: the next time round
	CHECK(std::abs(tr.at_song(4.0, at_9) - 12.0) < 1e-9);
	CHECK(std::isnan(tr.at_song(2.0, at_9))); // before the loop: never again
	CHECK(std::abs(tr.next(4.0, at_9) - 12.0) < 1e-9); // the next bar is where the loop folds back
	run(seq, T0, T0 + 10 * BEAT); // frames up to 4.983 s: running beats 0 to 9
	CHECK(rec.beats.size() == 10);
	if (rec.beats.size() == 10) {
		CHECK(rec.beats[7].second == 7.0 && rec.beats[8].second == 4.0 && rec.beats[9].second == 5.0);
	}
	// One loop jump so far: at running beat 8, back to song beat 4. The next is at 12.
	CHECK(rec.jumps.size() == 1 && rec.jumps[0].first == 8.0 && rec.jumps[0].second == 4.0);
	CHECK(std::abs(tr.next_loop_jump_after(9.0) - 12.0) < 1e-9 && std::abs(tr.next_loop_jump_after(12.0) - 16.0) < 1e-9);
}

// The bytes and words plugins receive, as the MIDI 1.0 and 2.0 specifications define them.
static void test_wire_encoding() {
	uint8_t b[3];
	encode_midi1(Kind::NOTE_ON, 1, 60, 0xFFFF, b);
	CHECK(b[0] == 0x90 && b[1] == 60 && b[2] == 127);
	encode_midi1(Kind::NOTE_OFF, 10, 36, 0x8000, b);
	CHECK(b[0] == 0x89 && b[1] == 36 && b[2] == 64);
	encode_midi1(Kind::PITCH_BEND, 1, 0, 0x80000000u, b);
	CHECK(b[0] == 0xE0 && b[1] == 0x00 && b[2] == 0x40); // the centre: LSB 0, MSB 64
	encode_midi1(Kind::PITCH_BEND, 16, 0, 0xFFFFFFFFu, b);
	CHECK(b[0] == 0xEF && b[1] == 0x7F && b[2] == 0x7F);
	encode_midi1(Kind::CONTROL, 1, 7, scale_up(100, 7, 32), b);
	CHECK(b[0] == 0xB0 && b[1] == 7 && b[2] == 100);
	encode_midi1(Kind::CHOKE, 3, 60, 0, b);
	CHECK(b[0] == 0xB2 && b[1] == 120 && b[2] == 0); // All Sound Off
	uint32_t w[2];
	encode_midi2(Kind::NOTE_ON, 1, 60, 0xFFFF, w);
	CHECK(w[0] == 0x40903C00u && w[1] == 0xFFFF0000u);
	encode_midi2(Kind::CONTROL, 2, 74, 0x80000000u, w);
	CHECK(w[0] == 0x40B14A00u && w[1] == 0x80000000u);
}

// What targets hear of the transport: a snapshot at play, each loop jump ahead of time with its
// exact time, pause and tempo changes, and the state at once for a target bound later.
static void test_transport_to_targets() {
	Recorder rec;
	Sequencer seq(rec);
	seq.set_channel_target(1, PIANO, T0);
	CHECK(rec.transports.size() == 1 && !rec.transports[0].info.playing); // told at once
	seq.set_loop(4.0, 8.0, T0);
	seq.play(T0);
	run(seq, T0, T0 + 8 * BEAT - 50000); // 50 ms before the loop's end: within the lookahead
	const Recorder::Snapshot *jump = nullptr;
	for (const auto &t : rec.transports) {
		if (t.time_us == T0 + 8 * BEAT) {
			jump = &t;
		}
	}
	CHECK(jump && jump->info.playing && jump->info.song_beat == 4.0 && jump->info.looping);
	const size_t before = rec.transports.size();
	seq.pause(T0 + 8 * BEAT + 100000);
	CHECK(rec.transports.size() == before + 1 && !rec.transports.back().info.playing);
	CHECK(std::abs(rec.transports.back().info.song_beat - 4.2) < 1e-9); // 0.2 beats after folding back
	seq.set_bpm(90, T0 + 9 * BEAT);
	CHECK(rec.transports.back().info.bpm == 90);
	seq.set_channel_target(2, DRUMS, T0 + 9 * BEAT);
	CHECK(rec.transports.back().target == DRUMS && rec.transports.back().info.bpm == 90);
}

// chase_notes: a note sounding at a pause starts again on resume, the same note (id, velocity),
// and still ends at its planned beat; a held one starts again and sounds on. Without it, neither
// starts again.
static void test_chase_notes() {
	for (bool chase : { true, false }) {
		Recorder rec;
		Sequencer seq(rec);
		seq.set_channel_target(1, PIANO);
		seq.set_chase_notes(chase);
		seq.play(T0);
		const int64_t long_note = seq.note_on(1, 60, 0x9000, 4.0, Transport::NOW, T0);
		const int64_t held = seq.note_on(1, 64, 0x7000, 0, Transport::NOW, T0);
		run(seq, T0, T0 + BEAT);
		const int64_t pause_at = T0 + BEAT;
		seq.pause(pause_at);
		const int64_t resume_at = pause_at + 1000000;
		seq.play(resume_at);
		run(seq, resume_at, resume_at + 4 * BEAT);
		auto on = rec.of(Kind::NOTE_ON);
		auto off = rec.of(Kind::NOTE_OFF);
		if (chase) {
			CHECK(on.size() == 4);
			if (on.size() == 4) {
				CHECK(on[2].time_us == resume_at && on[2].event.note_id == long_note && on[2].event.value == 0x9000);
				CHECK(on[3].time_us == resume_at && on[3].event.note_id == held && on[3].event.value == 0x7000);
			}
			// The long note's end: beat 4, 3 beats after the resume (paused at beat 1).
			CHECK(off.size() == 1 && off[0].event.note_id == long_note && off[0].time_us == resume_at + 3 * BEAT);
			CHECK(seq.note_state(held) == NoteState::SOUNDING && seq.note_state(long_note) == NoteState::ENDED);
		} else {
			CHECK(on.size() == 2 && off.empty());
			CHECK(seq.note_state(held) == NoteState::ENDED && seq.note_state(long_note) == NoteState::ENDED);
		}
	}
	// A chased note ended while paused is simply forgotten: nothing starts again.
	Recorder rec;
	Sequencer seq(rec);
	seq.set_channel_target(1, PIANO);
	seq.set_chase_notes(true);
	seq.play(T0);
	const int64_t n = seq.note_on(1, 60, 0x9000, 0, Transport::NOW, T0);
	run(seq, T0, T0 + BEAT);
	seq.pause(T0 + BEAT);
	seq.end_note(n, Transport::NOW, T0 + BEAT + 1000);
	seq.play(T0 + 2 * BEAT);
	CHECK(rec.of(Kind::NOTE_ON).size() == 1 && seq.note_state(n) == NoteState::ENDED);
}

// --- Clips ----------------------------------------------------------------------------------------

static void vlq(std::vector<uint8_t> &r, uint32_t v) {
	uint8_t b[4];
	int n = 0;
	do {
		b[n++] = uint8_t(v & 0x7F);
		v >>= 7;
	} while (v);
	while (n--) {
		r.push_back(uint8_t(b[n] | (n ? 0x80 : 0)));
	}
}

static void chunk(std::vector<uint8_t> &r, const char *p_id, const std::vector<uint8_t> &p_body) {
	r.insert(r.end(), p_id, p_id + 4);
	const uint32_t n = uint32_t(p_body.size());
	r.push_back(uint8_t(n >> 24)), r.push_back(uint8_t(n >> 16)), r.push_back(uint8_t(n >> 8)), r.push_back(uint8_t(n));
	r.insert(r.end(), p_body.begin(), p_body.end());
}

// A format 1 file, 480 ticks a beat. Track 1: name, 3/4, 120 bpm, then 240 bpm at beat 3. Track
// 2: program 5, CC 7 = 100, a note at 0 (1 beat, ended by note-on velocity 0, in running status),
// a SysEx, a note at 1.5 (half a beat, note-off), pitch bend centre at 2.5, end of track at 4.5.
static std::vector<uint8_t> make_smf() {
	std::vector<uint8_t> t1, t2, f;
	auto ev = [](std::vector<uint8_t> &r, uint32_t d, std::initializer_list<uint8_t> b) { vlq(r, d), r.insert(r.end(), b); };
	ev(t1, 0, { 0xFF, 0x03, 4, 'L', 'e', 'a', 'd' });
	ev(t1, 0, { 0xFF, 0x58, 4, 3, 2, 24, 8 });
	ev(t1, 0, { 0xFF, 0x51, 3, 0x07, 0xA1, 0x20 }); // 500000 us
	ev(t1, 1440, { 0xFF, 0x51, 3, 0x03, 0xD0, 0x90 }); // 250000 us, at beat 3
	ev(t1, 0, { 0xFF, 0x2F, 0 });
	ev(t2, 0, { 0xC1, 5 });
	ev(t2, 0, { 0xB1, 7, 100 });
	ev(t2, 0, { 0x91, 60, 127 });
	ev(t2, 480, { 60, 0 }); // running status, velocity 0: the note-off
	ev(t2, 0, { 0xF0, 3, 1, 2, 0xF7 });
	ev(t2, 240, { 0x91, 64, 64 });
	ev(t2, 240, { 0x81, 64, 0 });
	ev(t2, 240, { 0xE1, 0x00, 0x40 }); // pitch bend 8192: the centre
	ev(t2, 960, { 0xFF, 0x2F, 0 }); // end at beat 4.5
	chunk(f, "MThd", { 0, 1, 0, 2, 0x01, 0xE0 }); // format 1, 2 tracks, 480 ticks a beat
	chunk(f, "MTrk", t1);
	chunk(f, "MTrk", t2);
	return f;
}

static void test_read_smf() {
	const std::vector<uint8_t> bytes = make_smf();
	Clip c;
	std::string err;
	CHECK(read_midi_file(bytes.data(), bytes.size(), c, err) && err.empty());
	CHECK(c.midi_version == 1 && c.track_names.size() == 2 && c.track_names[0] == "Lead");
	CHECK(c.tempos.size() == 2 && c.tempos[0].beat == 0 && std::fabs(c.tempos[0].bpm() - 120) < 1e-9 && c.tempos[1].beat == 3 && std::fabs(c.tempos[1].bpm() - 240) < 1e-9);
	CHECK(c.meters.size() == 1 && c.meters[0].numerator == 3 && c.meters[0].denominator == 4);
	CHECK(c.length == 6.0); // ends at beat 4.5: rounded up to two bars of 3/4
	std::vector<ClipEvent> n;
	for (const ClipEvent &e : c.events) {
		if (e.kind == Kind::NOTE_ON) {
			n.push_back(e);
		}
	}
	CHECK(n.size() == 2);
	if (n.size() == 2) {
		CHECK(n[0].beat == 0 && n[0].length == 1.0 && n[0].index == 60 && n[0].value == 0xFFFF && n[0].channel == 2 && n[0].track == 1);
		CHECK(n[1].beat == 1.5 && n[1].length == 0.5 && n[1].index == 64 && n[1].value == 0x8000);
	}
	bool program = false, volume = false, bend = false;
	for (const ClipEvent &e : c.events) {
		program = program || (e.kind == Kind::PROGRAM && e.value == 5 && e.beat == 0);
		volume = volume || (e.kind == Kind::CONTROL && e.index == 7 && e.value == scale_up(100, 7, 32));
		bend = bend || (e.kind == Kind::PITCH_BEND && e.value == 0x80000000u && e.beat == 2.5);
	}
	CHECK(program && volume && bend);
	// Damaged files are refused with a reason, never read past their end.
	for (size_t cut : { size_t(3), size_t(13), size_t(30), bytes.size() - 2 }) {
		Clip d;
		std::string why;
		CHECK(!read_midi_file(bytes.data(), cut, d, why) && !why.empty());
	}
}

static void words(std::vector<uint8_t> &r, std::initializer_list<uint32_t> p_words) {
	for (uint32_t w : p_words) {
		r.push_back(uint8_t(w >> 24)), r.push_back(uint8_t(w >> 16)), r.push_back(uint8_t(w >> 8)), r.push_back(uint8_t(w));
	}
}

// A MIDI Clip File: 480 ticks a beat; at 0 a MIDI 2.0 note (velocity 0x1234, a beat long) and
// 240 bpm; at beat 1 a MIDI 1.0 CC 7 = 100 on channel 2 (a type 2 packet) and a MIDI 2.0 program 5
// with bank 1/2; the clip ends at beat 2.
static std::vector<uint8_t> make_clip_file() {
	std::vector<uint8_t> f = { 'S', 'M', 'F', '2', 'C', 'L', 'I', 'P' };
	words(f, { 0x003001E0 }); // DCTPQ: 480
	words(f, { 0x00400000, 0xF0200000, 0, 0, 0 }); // Start of Clip
	words(f, { 0x00400000, 0x40903C00, 0x12340000 }); // note-on, channel 1, key 60
	words(f, { 0x00400000, 0xD0100000, 25000000, 0, 0 }); // Flex Data: tempo, 240 bpm
	words(f, { 0x004001E0, 0x40803C00, 0 }); // a beat later: note-off
	words(f, { 0x00400000, 0x20B10764 }); // MIDI 1.0 in a packet: channel 2, CC 7 = 100
	words(f, { 0x00400000, 0x40C00001, (5u << 24) | (1u << 8) | 2u }); // program 5, bank 1/2
	words(f, { 0x004001E0, 0xF0210000, 0, 0, 0 }); // End of Clip, at beat 2
	return f;
}

static void test_read_clip_file() {
	const std::vector<uint8_t> bytes = make_clip_file();
	Clip c;
	std::string err;
	CHECK(read_midi_file(bytes.data(), bytes.size(), c, err) && err.empty());
	CHECK(c.midi_version == 2 && c.length == 4.0); // ends at beat 2: rounded up to a bar of 4/4
	CHECK(c.tempos.size() == 1 && c.tempos[0].beat == 0 && std::fabs(c.tempos[0].bpm() - 240) < 1e-9);
	int found = 0;
	for (const ClipEvent &e : c.events) {
		found += e.kind == Kind::NOTE_ON && e.beat == 0 && e.length == 1.0 && e.index == 60 && e.value == 0x1234 && e.channel == 1;
		found += e.kind == Kind::CONTROL && e.beat == 1 && e.channel == 2 && e.index == 7 && e.value == scale_up(100, 7, 32);
		found += e.kind == Kind::CONTROL && e.beat == 1 && e.index == 0 && e.value == scale_up(1, 7, 32);
		found += e.kind == Kind::CONTROL && e.beat == 1 && e.index == 32 && e.value == scale_up(2, 7, 32);
		found += e.kind == Kind::PROGRAM && e.beat == 1 && e.value == 5;
	}
	CHECK(found == 5 && c.events.size() == 5);
	Clip d;
	std::string why;
	CHECK(!read_midi_file(bytes.data(), bytes.size() - 3, d, why) && !why.empty());
}

// A pattern made by hand: notes on beats 0-3, half a beat long, 4 beats, channel 1.
static std::shared_ptr<Clip> four_notes() {
	auto c = std::make_shared<Clip>();
	c->length = 4;
	c->track_names.push_back("");
	for (int i = 0; i < 4; i++) {
		ClipEvent e;
		e.beat = i, e.length = 0.5, e.kind = Kind::NOTE_ON, e.channel = 1, e.index = uint16_t(60 + i), e.value = 0x8000;
		c->events.push_back(e);
	}
	return c;
}

// A song part repeating for ever: each note exactly on its beat, half a beat long, round after
// round. Removing it ends the sounding note at once and plans nothing more.
static void test_song_part() {
	Recorder rec;
	Sequencer seq(rec);
	seq.set_channel_target(1, PIANO);
	const int32_t part = seq.add_part(four_notes(), true, 0.0, 0, true, T0);
	seq.play(T0);
	run(seq, T0, T0 + 8 * BEAT - 1000);
	auto on = rec.of(Kind::NOTE_ON, T0 + 8 * BEAT);
	auto off = rec.of(Kind::NOTE_OFF, T0 + 8 * BEAT);
	CHECK(on.size() == 8 && off.size() == 8);
	for (size_t i = 0; i < on.size() && i < off.size(); i++) {
		CHECK(on[i].time_us == T0 + int64_t(i) * BEAT && on[i].event.index == 60 + i % 4);
		CHECK(off[i].time_us == T0 + int64_t(i) * BEAT + BEAT / 2);
	}
	run(seq, T0 + 8 * BEAT, T0 + 8 * BEAT + 100000); // into the 9th note
	seq.remove_part(part, T0 + 8 * BEAT + 100000);
	CHECK(!seq.has_part(part));
	const size_t ons = rec.of(Kind::NOTE_ON).size(), offs = rec.of(Kind::NOTE_OFF).size();
	run(seq, T0 + 8 * BEAT + 100000, T0 + 12 * BEAT);
	CHECK(rec.of(Kind::NOTE_ON).size() == ons && rec.of(Kind::NOTE_OFF).size() == offs);
	CHECK(ons == offs); // every note that started has ended
}

// A loop over beats 1-3 of a song part whose note at beat 2 is 1.5 beats long: at the loop's end
// that note ends (beat 3), and beat 1 comes again, then beat 2, a beat apart, exactly.
static void test_part_in_loop() {
	auto c = std::make_shared<Clip>(*four_notes());
	c->events[2].length = 1.5;
	Recorder rec;
	Sequencer seq(rec);
	seq.set_channel_target(1, PIANO);
	seq.add_part(c, true, 0.0, 0, true, T0);
	seq.set_loop(1.0, 3.0, T0);
	seq.play(T0);
	run(seq, T0, T0 + 5 * BEAT - 1000);
	auto on = rec.of(Kind::NOTE_ON, T0 + 5 * BEAT);
	auto off = rec.of(Kind::NOTE_OFF, T0 + 5 * BEAT);
	// beats 0, 1, 2, then round: 1 (at 3), 2 (at 4)
	CHECK(on.size() == 5);
	const int keys[5] = { 60, 61, 62, 61, 62 };
	for (size_t i = 0; i < on.size() && i < 5; i++) {
		CHECK(on[i].time_us == T0 + int64_t(i) * BEAT && on[i].event.index == keys[i]);
	}
	bool clamped = false;
	for (const auto &o : off) {
		clamped = clamped || (o.event.index == 62 && o.time_us == T0 + 3 * BEAT);
	}
	CHECK(clamped);
}

// Seeking into a part: the program and controller it set before that point go out first, at the
// seek, then the notes from there on.
static void test_part_chase_on_seek() {
	auto c = std::make_shared<Clip>(*four_notes());
	ClipEvent program, volume;
	program.kind = Kind::PROGRAM, program.channel = 1, program.value = 9;
	volume.kind = Kind::CONTROL, volume.channel = 1, volume.index = 7, volume.value = 0x40000000;
	c->events.insert(c->events.begin(), { program, volume });
	Recorder rec;
	Sequencer seq(rec);
	seq.set_channel_target(1, PIANO);
	seq.add_part(c, true, 0.0, 1, true, T0);
	seq.seek(1.5, T0);
	seq.play(T0);
	run(seq, T0, T0 + 3 * BEAT);
	CHECK(!rec.sent.empty() && rec.sent[0].event.kind == Kind::PROGRAM && rec.sent[0].event.value == 9 && rec.sent[0].time_us == T0);
	CHECK(rec.sent.size() > 1 && rec.sent[1].event.kind == Kind::CONTROL && rec.sent[1].event.value == 0x40000000 && rec.sent[1].time_us == T0);
	auto on = rec.of(Kind::NOTE_ON, T0 + 3 * BEAT);
	CHECK(on.size() == 2 && on[0].event.index == 62 && on[0].time_us == T0 + BEAT / 2); // beat 2, half a beat after 1.5
}

// The clip's tempo map: 120 bpm, then 60 from beat 2. Beats 0-2 are 500 ms apart, then 1 s; the
// transport reports 60 once there; a target hears the change as a transport snapshot at beat 2.
// Pausing just before the change takes it back with everything else, and resuming hands it out
// again: still exactly at beat 2.
static void test_part_tempo_map() {
	for (bool pause_before_change : { false, true }) {
		auto c = std::make_shared<Clip>(*four_notes());
		c->tempos.push_back(ClipTempo{ 0.0, 50000000 });
		c->tempos.push_back(ClipTempo{ 2.0, 100000000 });
		Recorder rec;
		Sequencer seq(rec);
		seq.set_channel_target(1, PIANO);
		seq.add_part(c, true, 0.0, 1, true, T0);
		seq.play(T0);
		int64_t gap = 0;
		if (pause_before_change) {
			const int64_t at = T0 + 2 * BEAT - 50000; // the change is in flight now
			run(seq, T0, at);
			seq.pause(at);
			gap = 700000;
			seq.play(at + gap);
			run(seq, at + gap, T0 + gap + 5 * BEAT);
		} else {
			run(seq, T0, T0 + 5 * BEAT);
		}
		auto on = rec.heard(Kind::NOTE_ON);
		CHECK(on.size() == 4);
		const int64_t want[4] = { T0, T0 + BEAT, T0 + gap + 2 * BEAT, T0 + gap + 2 * BEAT + 2 * BEAT };
		for (size_t i = 0; i < on.size() && i < 4; i++) {
			CHECK(on[i].time_us == want[i]);
		}
		CHECK(std::fabs(seq.transport().get_bpm() - 60.0) < 1e-9);
		bool told = false;
		for (const auto &t : rec.transports) {
			told = told || (t.target == PIANO && t.time_us == T0 + gap + 2 * BEAT && std::fabs(t.info.bpm - 60) < 1e-9 && std::fabs(t.info.song_beat - 2.0) < 1e-9);
		}
		CHECK(told);
	}
}

// A stinger: two rounds from the next bar, whatever the song loop does, then gone. Stop drops
// such a part (like notes planned ahead), and keeps a song part, which plays again on play().
static void test_running_part() {
	Recorder rec;
	Sequencer seq(rec);
	seq.set_channel_target(1, PIANO);
	seq.set_loop(0.0, 1.0, T0); // a one-beat song loop: does not fold the stinger
	seq.play(T0);
	run(seq, T0, T0 + BEAT / 2);
	const int32_t part = seq.add_part(four_notes(), false, seq.transport().next(4.0, T0 + BEAT / 2), 2, false, T0 + BEAT / 2);
	run(seq, T0 + BEAT / 2, T0 + 13 * BEAT);
	auto on = rec.of(Kind::NOTE_ON);
	CHECK(on.size() == 8);
	for (size_t i = 0; i < on.size() && i < 8; i++) {
		CHECK(on[i].time_us == T0 + BEAT + int64_t(i) * BEAT); // the next bar of a 1-beat loop: beat 1
	}
	CHECK(!seq.has_part(part)); // done
	const int32_t song = seq.add_part(four_notes(), true, 0.0, 1, true, T0 + 13 * BEAT);
	const int32_t sting = seq.add_part(four_notes(), false, Transport::NOW, 0, false, T0 + 13 * BEAT);
	seq.stop(T0 + 13 * BEAT + 1000);
	CHECK(seq.has_part(song) && !seq.has_part(sting));
	seq.set_loop(0, 0, T0 + 14 * BEAT);
	const size_t before = rec.of(Kind::NOTE_ON).size();
	seq.play(T0 + 14 * BEAT);
	run(seq, T0 + 14 * BEAT, T0 + 18 * BEAT - 1000);
	auto again = rec.of(Kind::NOTE_ON, T0 + 18 * BEAT);
	CHECK(again.size() == before + 4 && again[before].time_us == T0 + 14 * BEAT && again[before].event.index == 60);
}

// Muting a part, or one of its tracks, stops what it plans from then on; another channel.
static void test_part_mute_and_channel() {
	Recorder rec;
	Sequencer seq(rec);
	seq.set_channel_target(1, PIANO);
	seq.set_channel_target(10, DRUMS);
	const int32_t part = seq.add_part(four_notes(), true, 0.0, 0, true, T0);
	seq.set_part_channel(part, 1, 10);
	seq.play(T0);
	run(seq, T0, T0 + 2 * BEAT - 200000);
	seq.set_part_muted(part, true);
	run(seq, T0 + 2 * BEAT - 200000, T0 + 6 * BEAT);
	auto on = rec.of(Kind::NOTE_ON);
	CHECK(on.size() == 2 && on[0].target == DRUMS && on[0].event.channel == 10);
	CHECK(rec.of(Kind::NOTE_ON).size() == rec.of(Kind::NOTE_OFF).size());
}

// Note expressions: values packed and back without loss worth hearing; stand-in channel messages.
static void test_expression_values() {
	CHECK(unpack_expression(Expression::TUNING, pack_expression(Expression::TUNING, -0.5)) == -0.5);
	CHECK(unpack_expression(Expression::TUNING, pack_expression(Expression::TUNING, 12.25)) == 12.25);
	CHECK(std::fabs(unpack_expression(Expression::VOLUME, pack_expression(Expression::VOLUME, 2.0)) - 2.0) < 1e-8);
	CHECK(pack_expression(Expression::PAN, 0.0) == 0 && pack_expression(Expression::PAN, 1.0) == 0xFFFFFFFFu && pack_expression(Expression::PAN, 7.0) == 0xFFFFFFFFu);
	const uint16_t i = expression_index(60, Expression::BRIGHTNESS);
	CHECK(expression_key(i) == 60 && expression_of(i) == Expression::BRIGHTNESS);
	Stand_in s;
	CHECK(channel_stand_in(Expression::TUNING, 0.0, 2.0, s) && s.kind == Kind::PITCH_BEND && s.value == 0x80000000u); // no tuning: no bend
	CHECK(channel_stand_in(Expression::TUNING, 2.0, 2.0, s) && s.value == 0xFFFFFFFFu && channel_stand_in(Expression::TUNING, -9.0, 2.0, s) && s.value == 0);
	CHECK(channel_stand_in(Expression::BRIGHTNESS, 1.0, 2.0, s) && s.kind == Kind::CONTROL && s.index == 74);
	CHECK(!channel_stand_in(Expression::PRESSURE, 0.5, 2.0, s)); // pressure has its own per-note message
}

int main() {
	test_values();
	test_wire_encoding();
	test_grid_and_length();
	test_pause_resume();
	test_stop_and_seek();
	test_tempo_change();
	test_routing();
	test_loop();
	test_transport_to_targets();
	test_chase_notes();
	test_read_smf();
	test_read_clip_file();
	test_song_part();
	test_part_in_loop();
	test_part_chase_on_seek();
	test_part_tempo_map();
	test_running_part();
	test_part_mute_and_channel();
	test_expression_values();
	std::printf(failures ? "%d MIDI FAILURES\n" : "all MIDI core tests passed\n", failures);
	return failures ? 1 : 0;
}
