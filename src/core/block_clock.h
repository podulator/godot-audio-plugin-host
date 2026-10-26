#pragma once

// BlockClock: where in the audio does "now" fall? Makes parameter changes sample-accurate.
//
// THE PROBLEM
// A script sets a parameter at some moment; the audio thread picks the change up at the start of
// its next block. Dropped at frame 0, changes made 5 ms apart can come out 0 or 11.6 ms apart,
// depending on where block boundaries happen to fall. A filter swept from _process() at 60 fps
// then moves in uneven steps.
//
// THE IDEA: timestamps plus a fixed delay
// Every change carries the time it was made (host_time_us()). The audio thread works out which
// sample of which block corresponds to that time *plus a fixed delay D*, and puts it there.
// Everything comes out D late, but the spacing between changes is preserved exactly.
//
// MAPPING TIME TO SAMPLES
// Sample n of this runtime is played at  anchor + n / rate. The anchor is learned from when
// blocks arrive: a block starting at sample n that is processed at wall time `now` says
// anchor <= now - n / rate. Blocks can be late, never early, so the anchor is the SMALLEST such
// value seen. That matters because Godot's driver often asks for 1024 frames at once, which
// AudioServer mixes as two 512-frame blocks back to back: the second block of each pair arrives
// "early" relative to the first, and the minimum follows it.
//
// The CPU clock and the sound card's clock drift apart by a few dozen parts per million. So the
// anchor also creeps forward by 1 us per block (~86 us/s at 44.1 kHz, far more than any drift),
// and the minimum pulls it back down. That keeps it tracking without ever running ahead for long.
//
// GAPS
// Sometimes no blocks come at all for a while: an Android app in the background, a suspended
// laptop, a switched audio device. The stream then carries on, but its sample 0 is now further in
// the past than the anchor says, by the length of the gap. The minimum cannot follow (it only
// moves down) and the creep would take hours, so every change made afterwards would be heard that
// much later. A block arriving more than GAP_US later than the anchor predicts is therefore taken
// as a restart: the anchor is set from it afresh, and the minimum carries on from there. Normal
// lateness (one driver callback, scheduling jitter) stays far below GAP_US.
//
// HOW BIG D MUST BE
// A change made just after block k-1 was processed must still land at or after block k's first
// sample. Working through the anchor bound above gives D >= one block. One block plus 1 ms of
// slack for the creep is used: changes are heard one block (~11.6 ms) after they are made, the
// same delay as before, but now evenly spaced.

#include <chrono>
#include <cstdint>

namespace gaph {

// Microseconds on a monotonic clock, the same on the main and audio threads.
// steady_clock never jumps (unlike the wall clock, which NTP may adjust), and on Linux reading it
// is a vDSO call with no system call or lock, so the audio thread may use it.
inline int64_t host_time_us() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

class BlockClock {
public:
	static constexpr int64_t SLACK_US = 1000;
	static constexpr int64_t CREEP_US = 1;
	static constexpr int64_t GAP_US = 200000;

	// MAIN THREAD, before audio runs.
	void set_sample_rate(double p_rate) { rate = p_rate > 0 ? p_rate : 48000.0; }

	// AUDIO THREAD, once at the start of every block.
	void begin_block(int64_t p_now_us, uint32_t p_frames) {
		block_start = position;
		const double block_start_us = double(block_start) * 1e6 / rate;
		const int64_t candidate = p_now_us - int64_t(block_start_us);
		if (started && candidate - anchor_us > GAP_US) {
			anchor_us = candidate; // a restart after a gap (see GAPS above)
			restarts++;
		} else if (!started || candidate < anchor_us) {
			anchor_us = candidate;
			started = true;
		} else {
			anchor_us += CREEP_US;
		}
		frames = p_frames;
		delay_us = int64_t(double(p_frames) * 1e6 / rate) + SLACK_US;
		position += p_frames;
	}

	// AUDIO THREAD. Frame offset within the current block for a change made (or due) at
	// p_due_us: negative if it belongs before this block, >= the block's frame count if after.
	// p_due_us == 0 means "no timestamp": frame 0.
	int64_t frame_for(int64_t p_due_us) const {
		if (p_due_us == 0) {
			return 0;
		}
		return absolute_frame_for(p_due_us) - int64_t(block_start);
	}

	// AUDIO THREAD. The same, as a position in the whole stream (frames since the runtime
	// started), not within this block. EffectRuntime works an event's position out ONCE, when the
	// event arrives, and keeps it: the anchor still moves while the clock settles (a block arriving
	// earlier than any before lowers it), and an event re-mapped in a later block would then land
	// a few samples off. Mapped once, events that arrive together keep their spacing exactly
	// (seen on CI's Windows runner: a scheduled 100 ms dip, re-mapped each block, came out 4 and 22
	// samples long instead of 4410 exactly).
	int64_t absolute_frame_for(int64_t p_due_us) const {
		const double target_s = double(p_due_us + delay_us - anchor_us) * 1e-6;
		return int64_t(target_s * rate);
	}
	int64_t current_block_start() const { return int64_t(block_start); }

	int64_t get_delay_us() const { return delay_us; }

	// How often the stream has restarted after a gap. Stream positions worked out before a restart
	// (absolute_frame_for) are off by the gap: whoever keeps them works them out again when this
	// changes.
	uint32_t get_restarts() const { return restarts; }

private:
	double rate = 48000.0;
	bool started = false;
	int64_t anchor_us = 0;
	int64_t delay_us = 0;
	uint64_t position = 0; // Frames processed so far, i.e. the first frame of the next block.
	uint64_t block_start = 0;
	uint32_t frames = 0;
	uint32_t restarts = 0;
};

} // namespace gaph
