#pragma once

// EffectTempo: the tempo and song position an EFFECT's plugin hears, for tempo-synced delays, LFOs,
// gates and beat repeats. Two ways in, both ending as the plugin's transport (CLAP transport,
// VST3 ProcessContext), exact to the sample:
//   - a plain float: fx.bpm = 140. No driver needed; any source (GDSiON, gameplay) can set it.
//     The position runs on from the last sync_beat(), so a beat-locked effect can be put in phase
//     with the music at a downbeat. 0 (the default) gives the plugin no transport, as before.
//   - a MidiDriver: midi.add_transport_target(fx). Then the effect follows that driver's transport
//     exactly as instruments do (play, pause, seek, loop, tempo maps), through midi_transport and
//     midi_stop.
// Whichever spoke last is what the plugin hears. MAIN THREAD only.

#include "core/block_clock.h"
#include "core/hosted_effect.h"

#include <cstdint>

namespace gaph_godot {

class EffectTempo {
public:
	// The source id the float uses (a driver's is its instance id, never this small).
	static constexpr uint64_t OWN = 1;

	double get_bpm() const { return bpm; }

	void set_bpm(gaph::HostedEffect &p_hosted, double p_bpm) {
		const int64_t now = gaph::host_time_us();
		beat = beat_at(now);
		since = now;
		bpm = p_bpm > 0 ? p_bpm : 0.0;
		send(p_hosted, now);
	}

	// The song position is p_beat now (a downbeat, say), and runs on at bpm.
	void sync_beat(gaph::HostedEffect &p_hosted, double p_beat) {
		const int64_t now = gaph::host_time_us();
		beat = p_beat;
		since = now;
		send(p_hosted, now);
	}

	// A driver's midi_transport and midi_stop (see godot_common/instrument_common.h).
	void transport(gaph::HostedEffect &p_hosted, int64_t p_time_us, int64_t p_source, bool p_playing, double p_bpm, double p_song_beat, double p_loop_start, double p_loop_end) {
		gaph::ParamEvent ev;
		ev.type = gaph::ParamEvent::TRANSPORT;
		ev.midi_value = p_playing ? 1 : 0;
		ev.value = p_bpm;
		ev.transport_beat = p_song_beat;
		ev.loop_start = p_loop_start;
		ev.loop_end = p_loop_end;
		ev.source = uint64_t(p_source);
		ev.due_us = p_time_us;
		p_hosted.queue_transport(ev);
	}
	void stop(gaph::HostedEffect &p_hosted, int64_t p_time_us, int64_t p_source) { p_hosted.queue_drop(uint64_t(p_source), p_time_us); }

private:
	double beat_at(int64_t p_now) const { return bpm > 0 ? beat + double(p_now - since) / 1e6 * bpm / 60.0 : beat; }

	void send(gaph::HostedEffect &p_hosted, int64_t p_now) {
		if (bpm <= 0 && !sent) {
			return; // never had a tempo: the plugin keeps hearing no transport
		}
		sent = true;
		transport(p_hosted, p_now, int64_t(OWN), bpm > 0, bpm > 0 ? bpm : 120.0, beat, 0.0, 0.0);
	}

	double bpm = 0.0;
	double beat = 0.0;
	int64_t since = 0;
	bool sent = false;
};

} // namespace gaph_godot
