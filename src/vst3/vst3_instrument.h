#pragma once

// Vst3Instrument: a VST3 instrument plugin as an AudioStream, fed by a MidiDriver. The twin of
// ClapInstrument (src/clap/clap_instrument.h explains how an instrument fits Godot, its threads and
// lifetimes); what both share is in godot_common/instrument_common.h. Only loading differs, and
// what VST3 itself does differently (src/vst3/vst3_processor.h, INSTRUMENTS): notes as VST3
// events, controllers through the plugin's IMidiMapping, the transport per process() call
// (the runtime splits a block where it changes).

#include "core/effect_runtime.h"
#include "core/hosted_effect.h"
#include "godot_common/editor_window.h"
#include "godot_common/instrument_common.h"
#include "vst3_module.h"
#include "vst3_processor.h"

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>

#include <memory>

namespace godot {

class Vst3Instrument;

class Vst3InstrumentPlayback : public AudioStreamPlayback {
	GDCLASS(Vst3InstrumentPlayback, AudioStreamPlayback);

public:
	~Vst3InstrumentPlayback() override;
	// p_output: which of the instrument's outputs this playback plays (0, the main one, unless it
	// was made by a Vst3InstrumentOutput).
	void setup(const Ref<Vst3Instrument> &p_instrument, uint32_t p_output = 0);

	void _start(double) override { playing = true; }
	void _stop() override { playing = false; }
	bool _is_playing() const override { return playing; }
	double _get_playback_position() const override { return 0.0; }
	int32_t _mix(AudioFrame *p_buffer, float, int32_t p_frames) override;

protected:
	static void _bind_methods() {}

private:
	Ref<Vst3Instrument> instrument;
	gaph_godot::VoiceTap tap; // which mix step this playback is in (shared_voice.h)
	uint32_t output = 0;
	bool playing = false;
};

class Vst3Instrument : public AudioStream {
	GDCLASS(Vst3Instrument, AudioStream);

public:
	Vst3Instrument();
	~Vst3Instrument() override;

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
	int64_t get_frames_processed();
	static TypedArray<Dictionary> scan_plugins(const PackedStringArray &p_extra_dirs);
	bool is_plugin_sleeping(); // see ClapProcessor's SLEEP: an idle plugin is not called

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
	String _get_stream_name() const override { return plugin_path.get_file(); }
	double _get_length() const override { return 0.0; }
	bool _is_monophonic() const override { return true; }

	gaph::HostedEffect &get_hosted() { return hosted; }
	gaph_godot::SharedVoice &get_voice() { return voice; }
	uint32_t output_count() const { return output_names.size() > 0 ? uint32_t(output_names.size()) : 1; }

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
	VST3::Hosting::Module::Ptr module;
	std::unique_ptr<gaph::Vst3Processor> probe;
	mutable gaph::HostedEffect hosted; // mutable: see ClapInstrument
	gaph_godot::InstrumentInput input;
	gaph_godot::SharedVoice voice; // the one plugin instance all playbacks hear
	PackedStringArray output_names; // from the plugin, when it loads; the main output first
	gaph_godot::EditorWindow editor_window;
};

// One of an instrument's outputs beyond the main one, as a stream of its own (get_output()).
// It holds the instrument; the instrument does not hold it, so no reference cycle keeps either.
class Vst3InstrumentOutput : public AudioStream {
	GDCLASS(Vst3InstrumentOutput, AudioStream);

public:
	void setup(const Ref<Vst3Instrument> &p_instrument, int p_index) {
		instrument = p_instrument;
		index = p_index;
	}
	Ref<Vst3Instrument> get_instrument() const { return instrument; }
	int get_index() const { return index; }

	Ref<AudioStreamPlayback> _instantiate_playback() const override;
	String _get_stream_name() const override;
	double _get_length() const override { return 0.0; }
	bool _is_monophonic() const override { return true; }

protected:
	static void _bind_methods();

private:
	Ref<Vst3Instrument> instrument;
	int index = 0;
};

} // namespace godot
