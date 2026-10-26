#pragma once

// SharedVoice: one instrument's plugin instance, heard through every playback of it.
//
// An instrument resource is an AudioStream; each player that plays it gets a playback. MIDI goes
// to the instrument, not to a playback, so all its playbacks sound the SAME plugin instance: one
// synth, heard wherever its players are (two 3D players are two speakers), at one plugin's CPU.
// A second, independent synth is a second instrument resource.
//
// ONE BLOCK PER MIX STEP
// Godot mixes every playing playback once per mix step (512 frames). The first playback mixed in
// a step makes the plugin render that step's block; the others copy it, so all of them sound the
// same block in the same step, in whatever order Godot mixes them. That needs to know which mix
// step a call belongs to, and Godot keeps that count to itself. What it does tell extensions is
// when the sound card last asked for audio (AudioServer.get_time_since_last_mix()); one request
// covers one or more mix steps, back to back. So a step is "request time + which step of that
// request", and each playback counts its own calls within a request (VoiceTap). A playback that
// starts between two steps of one request is off by a block until the next request, about 23 ms
// at most, and then in step for good.
//
// THREADS
// join()/leave() run on the MAIN thread; the first join attaches the plugin runtime, the last
// leave detaches it, and in both cases no playback of this instrument is being mixed (a new
// playback is not in the mix yet; a leaving one is out of it already, see clap_instrument.h), so
// resetting the audio-side state there is safe. mix() runs on the AUDIO thread only. Calling
// get_time_since_last_mix() there is safe too: it takes the audio driver's lock, which the mixing
// thread already holds, and every driver's lock is Godot's recursive Mutex (checked: ALSA,
// PulseAudio, WASAPI, CoreAudio, OpenSL, web, dummy).

#include "core/effect_runtime.h"
#include "core/hosted_effect.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/classes/time.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace gaph_godot {

using namespace godot;

// Which mix step a playback's call belongs to: one per playback, AUDIO thread only.
struct VoiceTap {
	int64_t request_us = -1; // when the sound card's current request came, in Godot's usec ticks
	uint32_t step = 0; // which step of that request this playback is in
};

class SharedVoice {
public:
	static constexpr uint32_t MAX_FRAMES = 4096; // more than any mix step asks for (512)
	// Requests closer than this are one request: the clock readings behind it differ by
	// microseconds within one, and requests come at least 5 ms apart (256 frames at 48 kHz).
	static constexpr int64_t SAME_REQUEST_US = 2000;

	// MAIN THREAD. A playback starts using the voice (p_tap is its own); the first one plugs the
	// plugin runtime into the instrument.
	void join(gaph::HostedEffect &p_hosted, VoiceTap &p_tap) {
		p_tap = VoiceTap{};
		if (users++ == 0) {
			runtime.set_sample_rate(AudioServer::get_singleton()->get_mix_rate());
			silence.assign(size_t(MAX_FRAMES) * 2, 0.0f);
			block.assign(size_t(MAX_FRAMES) * 2, 0.0f);
			block_frames = 0;
			block_request_us = -1;
			block_step = 0;
			p_hosted.attach(runtime);
			hosted = &p_hosted;
		}
	}

	// MAIN THREAD. A playback is done with it; the last one unplugs the runtime.
	void leave() {
		if (users > 0 && --users == 0 && hosted) {
			hosted->detach(runtime);
			hosted = nullptr;
		}
	}

	// AUDIO THREAD. This step's sound for one playback: the block is rendered by the first
	// playback mixed in a step, and copied by the others.
	int32_t mix(VoiceTap &p_tap, AudioFrame *p_buffer, int32_t p_frames) {
		const uint32_t n = uint32_t(std::min<int32_t>(p_frames, int32_t(MAX_FRAMES)));
		const int64_t now = int64_t(Time::get_singleton()->get_ticks_usec());
		const int64_t request = now - int64_t(std::llround(AudioServer::get_singleton()->get_time_since_last_mix() * 1e6));
		if (p_tap.request_us >= 0 && std::llabs(request - p_tap.request_us) < SAME_REQUEST_US) {
			p_tap.step++;
		} else {
			p_tap.request_us = request;
			p_tap.step = 0;
		}
		const bool same_step = block_request_us >= 0 && std::llabs(p_tap.request_us - block_request_us) < SAME_REQUEST_US && p_tap.step == block_step;
		if (!same_step || n > block_frames) {
			render(n);
			block_request_us = p_tap.request_us;
			block_step = p_tap.step;
		}
		std::memcpy(p_buffer, block.data(), sizeof(float) * 2 * n);
		return p_frames;
	}

private:
	// AUDIO THREAD. The next block of the plugin's output, in slices of at most MAX_BLOCK.
	void render(uint32_t p_frames) {
		uint32_t done = 0;
		while (done < p_frames) {
			const uint32_t n = std::min(p_frames - done, gaph::EffectRuntime::MAX_BLOCK);
			runtime.process(silence.data(), block.data() + size_t(done) * 2, n);
			done += n;
		}
		block_frames = p_frames;
	}

	gaph::EffectRuntime runtime;
	gaph::HostedEffect *hosted = nullptr; // the instrument's own, which outlives its playbacks
	uint32_t users = 0; // MAIN thread: playbacks joined
	std::vector<float> silence; // the plugin's input: an instrument has none
	std::vector<float> block; // this step's output, interleaved stereo
	uint32_t block_frames = 0;
	int64_t block_request_us = -1; // which step `block` is: request time and step within it
	uint32_t block_step = 0;
};

} // namespace gaph_godot
