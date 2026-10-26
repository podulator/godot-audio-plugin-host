#pragma once

// ClapInstrument: a CLAP instrument plugin as an AudioStream. Played by any AudioStreamPlayer
// (2D and 3D too), on any bus, and fed by a MidiDriver: the instrument resource is the driver's
// target, midi.set_channel_target(1, $Piano.stream).
//
// HOW IT FITS GODOT
// An AudioStream is a description; playing it makes an AudioStreamPlayback, whose _mix() Godot
// calls on the AUDIO thread to fill each block. Here a playback runs one plugin instance through
// the same EffectRuntime the effects use, with silence as its input. That runtime already places
// timed events on the exact sample, so MIDI simply travels along with parameter changes.
//
// THREADS AND LIFETIMES (checked in Godot 4.7's servers/audio/audio_server.cpp)
// A playback is made on the MAIN thread (AudioStreamPlayer::play), before Godot adds it to the
// mix, and Godot drops its own reference on the main thread too, after taking it out of the mix
// (AudioServer::update). So the plugin instance is created and destroyed on the main thread,
// never while the audio thread can reach it, as for effects. All playbacks of one instrument hear
// its ONE plugin instance (godot_common/shared_voice.h): two players are one synth in two places.
//
// MIDI IN (the MidiDriver target protocol, see midi_host's MidiDriver)
//   midi_event(time_us, source, kind, channel, index, value, note_id): queued to the plugin.
//   midi_stop(time_us, source, mode): the audio side drops that source's events from time_us on;
//   which notes still sound then is worked out here, on the main thread (core/midi_target.h), and
//   they get a note-off (RELEASE) or a choke (CUT) at time_us.
//
// THE PLUGIN WINDOW works exactly as an effect's (godot_common/editor_window.h): editor only,
// opened by open_editor() or the inspector's "Open Plugin Window" button (clap_editor_plugin.h).

#include "clap_library.h"
#include "clap_processor.h"
#include "core/effect_runtime.h"
#include "core/hosted_effect.h"
#include "godot_common/editor_window.h"
#include "godot_common/instrument_common.h"

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>

#include <memory>
#include <vector>

namespace godot {

class ClapInstrument;

class ClapInstrumentPlayback : public AudioStreamPlayback {
	GDCLASS(ClapInstrumentPlayback, AudioStreamPlayback);

public:
	~ClapInstrumentPlayback() override;
	// p_output: which of the instrument's outputs this playback plays (0, the main one, unless it
	// was made by a ClapInstrumentOutput).
	void setup(const Ref<ClapInstrument> &p_instrument, uint32_t p_output = 0);

	void _start(double p_from_pos) override;
	void _stop() override;
	bool _is_playing() const override;
	double _get_playback_position() const override { return 0.0; }
	int32_t _mix(AudioFrame *p_buffer, float p_rate_scale, int32_t p_frames) override;

protected:
	static void _bind_methods() {}

private:
	Ref<ClapInstrument> instrument;
	gaph_godot::VoiceTap tap; // which mix step this playback is in (shared_voice.h)
	uint32_t output = 0;
	bool playing = false;
};

class ClapInstrument : public AudioStream {
	GDCLASS(ClapInstrument, AudioStream);

public:
	ClapInstrument();
	~ClapInstrument() override;

	void set_plugin_path(const String &p_path);
	String get_plugin_path() const { return plugin_path; }
	void set_plugin_id(const String &p_id);
	String get_plugin_id() const { return plugin_id; }
	bool is_plugin_loaded();
	String get_load_error();
	TypedArray<Dictionary> get_parameters();
	void set_parameter(int64_t p_id, double p_value);
	double get_parameter(int64_t p_id);
	Dictionary get_plugin_info();

	// Saving, as for effects (godot_common/instrument_common.h): the plugin's own state, and the
	// parameters set by hand. Storage properties: saved with the scene, not shown.
	void set_plugin_state(const PackedByteArray &p_state);
	PackedByteArray get_plugin_state();
	void set_param_values(const Dictionary &p_values);
	Dictionary get_param_values();

	// The plugin's own window (editor only), as for effects: godot_common/editor_window.h. The
	// GUI shows on the running playback if there is one, else on the probe.
	// Outputs: a plugin with several (a drum machine's kick, snare, ...) plays each through a
	// stream of its own, in its own player, on any bus. Output 0 is the main one: the instrument
	// itself. All come from the one plugin instance, in step to the sample (shared_voice.h).
	int get_output_count();
	PackedStringArray get_output_names();
	Ref<AudioStream> get_output(int p_index);
	void bind_outputs(const Array &p_players);

	bool open_editor();
	void close_editor();
	bool is_editor_open() const;

	// The MidiDriver target protocol.
	void midi_event(int64_t p_time_us, int64_t p_source, int p_kind, int p_channel, int p_index, int64_t p_value, int64_t p_note_id);
	void midi_stop(int64_t p_time_us, int64_t p_source, int p_mode);
	void midi_transport(int64_t p_time_us, int64_t p_source, bool p_playing, double p_bpm, double p_song_beat, double p_loop_start, double p_loop_end);

	Ref<AudioStreamPlayback> _instantiate_playback() const override;
	String _get_stream_name() const override;
	double _get_length() const override { return 0.0; }
	bool _is_monophonic() const override { return true; }

	gaph::HostedEffect &get_hosted() { return hosted; }
	gaph_godot::SharedVoice &get_voice() { return voice; }
	uint32_t output_count() const { return output_names.size() > 0 ? uint32_t(output_names.size()) : 1; }
	int64_t get_frames_processed();
	static TypedArray<Dictionary> scan_plugins(const PackedStringArray &p_extra_dirs);
	bool is_plugin_sleeping(); // see ClapProcessor's SLEEP: an idle plugin is not called

protected:
	static void _bind_methods();
	// One inspector slider per plugin parameter (godot_common/param_properties.h), as for effects.
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;

private:
	void ensure_loaded();
	void load_plugin();
	void connect_idle();
	void on_process_frame();

	String plugin_path;
	String plugin_id;
	String load_error;
	bool load_pending = false;
	bool idle_connected = false;
	std::shared_ptr<gaph::ClapLibrary> library;
	std::unique_ptr<gaph::ClapProcessor> probe;
	// mutable: Godot's _instantiate_playback() is const, but a new playback joins the instrument's
	// running instances (HostedEffect's registry).
	mutable gaph::HostedEffect hosted;
	gaph_godot::InstrumentInput input;
	gaph_godot::SharedVoice voice; // the one plugin instance all playbacks hear
	PackedStringArray output_names; // from the plugin, when it loads; the main output first
	gaph_godot::EditorWindow editor_window;
};

// One of an instrument's outputs beyond the main one, as a stream of its own (get_output()).
// It holds the instrument; the instrument does not hold it, so no reference cycle keeps either.
class ClapInstrumentOutput : public AudioStream {
	GDCLASS(ClapInstrumentOutput, AudioStream);

public:
	void setup(const Ref<ClapInstrument> &p_instrument, int p_index) {
		instrument = p_instrument;
		index = p_index;
	}
	Ref<ClapInstrument> get_instrument() const { return instrument; }
	int get_index() const { return index; }

	Ref<AudioStreamPlayback> _instantiate_playback() const override;
	String _get_stream_name() const override;
	double _get_length() const override { return 0.0; }
	bool _is_monophonic() const override { return true; }

protected:
	static void _bind_methods();

private:
	Ref<ClapInstrument> instrument;
	int index = 0;
};

} // namespace godot
