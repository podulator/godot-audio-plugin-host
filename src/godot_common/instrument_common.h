#pragma once

// What a CLAP and a VST3 instrument have in common, so ClapInstrument and Vst3Instrument stay thin
// (each only loads its format's plugin). Both are AudioStreams fed by a MidiDriver: see
// src/clap/clap_instrument.h for how an instrument fits Godot, and midi_host's MidiDriver for the
// target protocol these implement.
//
//   InstrumentInput   the MidiDriver target methods, on the MAIN thread: events and transport
//                     queued to every running instance (HostedEffect), and midi_stop's
//                     bookkeeping (which notes still sound at the stop, core/midi_target.h).
//   InstrumentVoice   one playback's sound, on the AUDIO thread: the plugin run with silence as
//                     its input, through the same EffectRuntime effects use.

#include "core/effect_runtime.h"
#include "core/hosted_effect.h"
#include "core/midi_target.h"
#include "core/midi_values.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

namespace gaph_godot {

using namespace godot;

class InstrumentInput {
public:
	// midi_event(time_us, source, kind, channel, index, value, note_id)
	void event(gaph::HostedEffect &p_hosted, int64_t p_time_us, int64_t p_source, int p_kind, int p_channel, int p_index, int64_t p_value,
			int64_t p_note_id) {
		using gaph::midi::Kind;
		ERR_FAIL_COND_MSG(p_kind < int(Kind::NOTE_ON) || p_kind > int(Kind::PROGRAM), vformat("Instrument: unknown MIDI event kind %d.", p_kind));
		ERR_FAIL_COND_MSG(p_channel < 1 || p_channel > 16, vformat("Instrument: channel %d is not 1-16.", p_channel));
		const uint64_t source = uint64_t(p_source);
		if (p_kind == int(Kind::NOTE_ON)) {
			notes.note_on(source, p_note_id, uint8_t(p_channel), uint16_t(p_index), p_time_us);
		} else if (p_kind == int(Kind::NOTE_OFF)) {
			notes.note_off(source, p_note_id, uint8_t(p_channel), uint16_t(p_index), p_time_us);
		}
		queue(p_hosted, p_kind, p_channel, p_index, uint32_t(p_value), p_note_id, source, p_time_us);
	}

	// midi_stop(time_us, source, mode): the audio side drops that source's events from time_us on;
	// the notes still sounding then are ended there, by a note-off that lets their tails ring
	// (RELEASE) or a choke that silences them (CUT). WITHDRAW leaves sounding notes alone. The
	// DROP is queued before the note-offs, so it does not catch them (the queue keeps order).
	void stop(gaph::HostedEffect &p_hosted, int64_t p_time_us, int64_t p_source, int p_mode) {
		using gaph::midi::Kind;
		const uint64_t source = uint64_t(p_source);
		p_hosted.queue_drop(source, p_time_us);
		if (p_mode == 0) {
			notes.withdraw(source, p_time_us);
			return;
		}
		const bool cut = p_mode == 2;
		for (const gaph::midi::NoteBook::Sounding &n : notes.stop(source, p_time_us)) {
			queue(p_hosted, int(cut ? Kind::CHOKE : Kind::NOTE_OFF), n.channel, n.key, cut ? 0u : 0x8000u, n.note_id, source, p_time_us);
		}
	}

	// midi_transport(time_us, source, playing, bpm, song_beat, loop_start, loop_end): travels with
	// the notes, on the same exact grid, and reaches the plugin as its format's transport.
	void transport(gaph::HostedEffect &p_hosted, int64_t p_time_us, int64_t p_source, bool p_playing, double p_bpm, double p_song_beat,
			double p_loop_start, double p_loop_end) {
		gaph::ParamEvent ev;
		ev.type = gaph::ParamEvent::TRANSPORT;
		ev.midi_value = p_playing ? 1 : 0;
		ev.value = p_bpm;
		ev.transport_beat = p_song_beat;
		ev.loop_start = p_loop_start;
		ev.loop_end = p_loop_end;
		ev.source = uint64_t(p_source);
		ev.due_us = p_time_us;
		p_hosted.queue_midi(ev);
	}

	// MAIN THREAD, every frame: forgets notes that ended a while ago, so the book stays small.
	void prune() { notes.prune(gaph::host_time_us() - 1000000); }

private:
	void queue(gaph::HostedEffect &p_hosted, int p_kind, int p_channel, int p_index, uint32_t p_value, int64_t p_note_id, uint64_t p_source,
			int64_t p_time_us) {
		gaph::ParamEvent ev;
		ev.type = gaph::ParamEvent::MIDI;
		ev.midi_kind = uint8_t(p_kind);
		ev.midi_channel = uint8_t(p_channel);
		ev.midi_index = uint16_t(p_index);
		ev.midi_value = p_value;
		ev.note_id = p_note_id;
		ev.source = p_source;
		ev.due_us = p_time_us;
		p_hosted.queue_midi(ev);
	}

	gaph::midi::NoteBook notes;
};

// SAVING, as for effects: `plugin_state`, the plugin's own complete state as bytes, and
// `param_values`, the parameters set by hand (godot_common/param_properties.h). Godot sets them
// in property order when it loads a scene, after plugin_path, and before the deferred plugin load:
// the state is then kept and applied as the plugin comes up (HostedEffect::set_plugin), and
// values set by hand, applied after it, win where the two disagree.
inline void set_state_bytes(gaph::HostedEffect &p_hosted, bool p_load_pending, const PackedByteArray &p_state) {
	std::vector<uint8_t> bytes(p_state.ptr(), p_state.ptr() + p_state.size());
	if (p_load_pending || !p_hosted.has_plugin()) {
		p_hosted.set_pending_state(std::move(bytes));
	} else {
		p_hosted.load_state(bytes);
	}
}

inline PackedByteArray get_state_bytes(gaph::HostedEffect &p_hosted) {
	std::vector<uint8_t> bytes;
	PackedByteArray out;
	if (p_hosted.save_state(bytes)) {
		out.resize(int64_t(bytes.size()));
		std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	}
	return out;
}

// One playback's sound. The format's AudioStreamPlayback subclass owns one, and forwards
// setup (MAIN thread, before Godot mixes the playback: no lock needed to hand the runtime a plugin
// instance), detach (MAIN thread, after Godot has taken the playback out of the mix) and mix
// (AUDIO thread) to it.
class InstrumentVoice {
public:
	void setup(gaph::HostedEffect &p_hosted) {
		runtime.set_sample_rate(AudioServer::get_singleton()->get_mix_rate());
		silence.assign(size_t(gaph::EffectRuntime::MAX_BLOCK) * 2, 0.0f);
		p_hosted.attach(runtime);
		hosted = &p_hosted;
	}

	void detach() {
		if (hosted) {
			hosted->detach(runtime);
			hosted = nullptr;
		}
	}

	// AUDIO THREAD. The plugin's output for p_frames frames, in slices of at most MAX_BLOCK (Godot
	// asks for 512 at a time; the loop is insurance). Silence goes in: an instrument has no input.
	int32_t mix(AudioFrame *p_buffer, int32_t p_frames) {
		float *out = reinterpret_cast<float *>(p_buffer);
		int32_t done = 0;
		while (done < p_frames) {
			const uint32_t n = uint32_t(std::min<int32_t>(p_frames - done, int32_t(gaph::EffectRuntime::MAX_BLOCK)));
			runtime.process(silence.data(), out + size_t(done) * 2, n);
			done += int32_t(n);
		}
		return p_frames;
	}

private:
	gaph::EffectRuntime runtime;
	std::vector<float> silence; // the plugin's (unused) input: zeros, allocated once
	gaph::HostedEffect *hosted = nullptr; // the instrument's, which outlives every playback (they hold a reference to it)
};

} // namespace gaph_godot
