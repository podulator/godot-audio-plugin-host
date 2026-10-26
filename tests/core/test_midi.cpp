// Standalone tests for the MIDI driver's core (src/core/midi_*.h), with made-up times, so every
// result is exact to the microsecond. Build and run: tests/core/run.sh
#include "core/midi_sequencer.h"

#include <cmath>
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
	};
	struct Stopped {
		uint64_t target;
		int64_t time_us;
		StopMode mode;
	};
	std::vector<Sent> sent;
	std::vector<Stopped> stops;
	std::vector<std::pair<int64_t, double>> beats;
	void send(uint64_t p_target, int64_t p_time_us, const Event &p_event) override { sent.push_back({ p_target, p_time_us, p_event }); }
	void stop(uint64_t p_target, int64_t p_time_us, StopMode p_mode) override { stops.push_back({ p_target, p_time_us, p_mode }); }
	void beat(int64_t p_running, double p_song) override { beats.push_back({ p_running, p_song }); }
	std::vector<std::pair<double, double>> jumps;
	void looped(double p_running, double p_song) override { jumps.push_back({ p_running, p_song }); }
	std::vector<Sent> of(Kind p_kind) const {
		std::vector<Sent> out;
		for (const Sent &s : sent) {
			if (s.event.kind == p_kind) {
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

int main() {
	test_values();
	test_wire_encoding();
	test_grid_and_length();
	test_pause_resume();
	test_stop_and_seek();
	test_tempo_change();
	test_routing();
	test_loop();
	std::printf(failures ? "%d MIDI FAILURES\n" : "all MIDI core tests passed\n", failures);
	return failures ? 1 : 0;
}
