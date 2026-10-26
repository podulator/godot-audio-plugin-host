#pragma once

// Transport: the MIDI driver's clock. Where in the music is "now", and when is beat b heard?
// Plain C++, no Godot. MAIN THREAD only: the driver plans on the main thread and hands each event
// to its instrument with a time; nothing here runs on the audio thread.
//
// TWO POSITIONS
// - The RUNNING position r: beats since play(), at the current tempo. It runs while playing,
//   stands still while paused, and never jumps. Scheduled events sit at running positions, which
//   is why pause can keep them: they simply play once r reaches them again.
// - The SONG position m: what a musician calls "bar 5, beat 1". It follows r, except that a seek
//   moves it, and a loop folds it back: m = origin + r until the loop's end, then round the loop.
//
// TIME
// While playing, running position r is due at  t0 + (r - r0) * 60e6 / bpm  microseconds on the
// host clock (gaph::host_time_us()), and is heard one audio block after that, like every
// timestamped change in this host (block_clock.h). Play, resume and tempo changes re-anchor
// (r0, t0) at "now"; nothing else touches the anchor, so times stay exact.

#include <cmath>
#include <cstdint>
#include <limits>

namespace gaph::midi {

class Transport {
public:
	// "Now", as an `at`: the event goes out at once, one block ahead, whatever the transport does.
	static constexpr double NOW = -1.0;
	// A position the music never reaches (outside a running loop, say): such an event is refused.
	static double never() { return std::numeric_limits<double>::quiet_NaN(); }

	bool is_playing() const { return playing; }
	double get_bpm() const { return bpm; }
	double us_per_beat() const { return 60e6 / bpm; }

	// The running position at host time p_now_us (where the transport stands, if not playing).
	double running_at(int64_t p_now_us) const {
		if (!playing) {
			return r0;
		}
		return r0 + double(p_now_us - t0) / us_per_beat();
	}

	// The host time at which running position p_r is due (only meaningful while playing).
	int64_t time_of(double p_r) const {
		return t0 + int64_t(std::llround((p_r - r0) * us_per_beat()));
	}

	// The song position for running position p_r.
	double song_at(double p_r) const {
		const double u = origin + p_r;
		if (loop_armed && u >= loop_end) {
			return loop_start + std::fmod(u - loop_end, loop_end - loop_start);
		}
		return u;
	}

	void play(int64_t p_now_us) {
		if (playing) {
			return;
		}
		t0 = p_now_us; // r0 stays: play after pause resumes, after stop starts from 0
		playing = true;
	}

	void pause(int64_t p_now_us) {
		if (!playing) {
			return;
		}
		r0 = running_at(p_now_us);
		playing = false;
	}

	// Back to song position 0, not playing.
	void stop() {
		playing = false;
		r0 = 0;
		origin = 0;
		update_loop_armed(0);
	}

	// The song continues at p_song from now on. The running position does not jump (schedules
	// keep their meaning); the song position is moved under it.
	void seek(double p_song, int64_t p_now_us) {
		origin = p_song - running_at(p_now_us);
		update_loop_armed(p_song);
	}

	void set_bpm(double p_bpm, int64_t p_now_us) {
		if (!(p_bpm > 0) || p_bpm == bpm) {
			return;
		}
		r0 = running_at(p_now_us);
		t0 = p_now_us;
		bpm = p_bpm;
	}

	// A loop from p_start to p_end (song positions); p_end <= p_start turns looping off.
	void set_loop(double p_start, double p_end, int64_t p_now_us) {
		looping = p_end > p_start;
		loop_start = p_start;
		loop_end = p_end;
		update_loop_armed(origin + running_at(p_now_us));
	}
	bool is_looping() const { return looping; }
	double get_loop_start() const { return loop_start; }
	double get_loop_end() const { return loop_end; }

	// --- The `at` helpers. Each returns a running position, or NOW while stopped. ---------------

	// p_beats from now.
	double after(double p_beats, int64_t p_now_us) const {
		if (!playing) {
			return NOW;
		}
		return running_at(p_now_us) + (p_beats > 0 ? p_beats : 0);
	}

	// The next time the song reaches position p_song: with a loop, the next time round. Already
	// passed and not coming again: now (late beats never). Outside a running loop: never().
	double at_song(double p_song, int64_t p_now_us) const {
		if (!playing) {
			return NOW;
		}
		const double r = running_at(p_now_us);
		const double u = origin + r; // the unfolded song position now
		if (!loop_armed) {
			return p_song >= u ? r + (p_song - u) : r;
		}
		// Before the loop's end comes round the first time, the song simply runs up to it.
		if (u < loop_end && p_song >= u && p_song < loop_end) {
			return r + (p_song - u);
		}
		if (p_song < loop_start || p_song >= loop_end) {
			return p_song < u && u < loop_end ? r : never();
		}
		// Round the loop: song position p_song recurs at unfolded loop_end + (p_song - loop_start)
		// + k * length. The first of those at or after now.
		const double length = loop_end - loop_start;
		double first = loop_end + (p_song - loop_start);
		if (first < u) {
			first += std::ceil((u - first) / length - 1e-12) * length;
		}
		return r + (first - u);
	}

	// The next multiple of p_grid beats on the song's grid: next(1.0) is the next beat, next(4.0)
	// the next bar in 4/4. Exactly on the grid already: now.
	double next(double p_grid, int64_t p_now_us) const {
		if (!playing || !(p_grid > 0)) {
			return playing ? running_at(p_now_us) : NOW;
		}
		const double m = song_at(running_at(p_now_us));
		double target = std::ceil(m / p_grid - 1e-9) * p_grid;
		if (loop_armed && target >= loop_end) {
			target = loop_start; // the grid point after the loop's end is where it folds back to
		}
		return at_song(target, p_now_us);
	}

	// The running position at which the song next passes the loop's end (and folds back to its
	// start), after running position p_r; NaN if it never does (no loop, or already past it).
	double next_loop_jump_after(double p_r) const {
		if (!loop_armed) {
			return never();
		}
		const double u = origin + p_r;
		if (u < loop_end) {
			return p_r + (loop_end - u);
		}
		const double length = loop_end - loop_start;
		const double into = std::fmod(u - loop_end, length); // how far into the current time round
		return p_r + (length - into);
	}

private:
	void update_loop_armed(double p_song_now) { loop_armed = looping && p_song_now < loop_end; }

	double bpm = 120.0;
	bool playing = false;
	double r0 = 0; // running position at t0 (or where it stands, when not playing)
	int64_t t0 = 0;
	double origin = 0; // song position = origin + running position (before any loop folding)
	bool looping = false, loop_armed = false;
	double loop_start = 0, loop_end = 0;
};

} // namespace gaph::midi
