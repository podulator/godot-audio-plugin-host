#pragma once

// The contract between the shared core and a plugin format (CLAP or VST3).
//
// The core knows nothing about CLAP or VST3. It talks to "a PluginProcessor", and each format
// provides a subclass that translates these calls into its own API. This is plain C++ runtime
// polymorphism: `virtual` functions, called through a base-class pointer, dispatch to whichever
// subclass the object really is. VST3 support then reuses everything in src/core unchanged.
//
// THREADING: the most important thing in a plugin host
// Plugin APIs split their functions into "main thread" and "audio thread" calls, and calling one
// from the wrong thread is undefined behaviour that a plugin may or may not survive. The comment on
// each function below says which thread it runs on, and that is a promise the host must keep.
//   main thread:  Godot's own thread (scripts, the editor, _process). Allowed to be slow.
//   audio thread: the thread Godot's AudioServer mixes on. Real-time: see rt_queue.h.

#include <cstdint>
#include <string>
#include <vector>

namespace gaph {

// Format-neutral parameter flags. Each format adapter maps its own flags onto these.
enum ParamFlags : uint32_t {
	PARAM_STEPPED = 1 << 0, // Integer steps only, such as a mode switch.
	PARAM_AUTOMATABLE = 1 << 1,
	PARAM_READ_ONLY = 1 << 2, // The plugin reports it (a meter, say); the host must not set it.
	PARAM_HIDDEN = 1 << 3, // Not meant for the user. Kept, but not shown in the inspector.
	PARAM_BYPASS = 1 << 4,
};

// One parameter, as the plugin describes it. Values are in the plugin's own units: CLAP uses the
// real range ("plain" values, -60..0 dB, say), VST3 always uses 0..1 ("normalised"). Each format
// adapter deals with its own convention.
//
// `id` is the plugin's stable identifier, and the one thing to save in a scene file. Names change
// between plugin versions, and so does the order (index), but ids are promised not to.
struct ParamInfo {
	uint32_t id = 0;
	std::string name;
	std::string module; // Grouping path such as "Filter/Envelope". Display only.
	double min_value = 0.0;
	double max_value = 1.0;
	double default_value = 0.0;
	uint32_t flags = 0; // ParamFlags bits.
	// For PARAM_STEPPED: how many steps the range has. A VST3 switch with 4 positions is 0..1
	// with step_count 3 (0, 1/3, 2/3, 1). 0 means continuous.
	uint32_t step_count = 0;
};

// "Set parameter `param_id` to `value`, starting `frame_offset` frames into this block." The
// offset is what makes automation sample-accurate. Events in one block are sorted by offset.
struct ParamEvent {
	uint32_t param_id = 0;
	uint32_t frame_offset = 0;
	double value = 0.0;
	// host_time_us() when the change should happen; 0 for "as soon as possible". Set on the
	// main thread, turned into frame_offset on the audio thread (see block_clock.h).
	int64_t due_us = 0;
	// AUDIO THREAD only: the event's position in the whole stream, worked out once when the audio
	// thread first sees it (-1 until then). See BlockClock::absolute_frame_for.
	int64_t due_frame = -1;
};

// One block of audio, as handed to a plugin.
//
// PLANAR vs INTERLEAVED
// Godot stores stereo as interleaved pairs, L R L R L R. Every plugin API wants planar: one array
// per channel, LLLL... and RRRR... . `inputs` is therefore an array of channel pointers:
// inputs[0] points at `frames` left samples, inputs[1] at `frames` right samples. The type
// `const float *const *` reads right to left: a pointer to const pointers to const floats. The
// plugin may change neither the samples nor which arrays they live in.
struct AudioBlock {
	uint32_t frames = 0;
	uint32_t channels = 0;
	const float *const *inputs = nullptr;
	float *const *outputs = nullptr; // The plugin writes the samples; the array list is fixed.
	// The sidechain, planar like `inputs`, or nullptr when none is assigned. The format layer
	// feeds it to the plugin's first non-main input (a vocoder's "Source", say), and silence to any
	// other extra inputs.
	uint32_t aux_channels = 0;
	const float *const *aux_inputs = nullptr;
	const ParamEvent *param_events = nullptr;
	uint32_t param_event_count = 0;
};

// One running plugin instance, already activated by the format adapter that created it.
// Created and destroyed on the main thread. Only process() runs on the audio thread.
class PluginProcessor {
public:
	// A base class used through pointers needs a virtual destructor. Without it, deleting a
	// ClapProcessor through a PluginProcessor* would run only this (empty) destructor, and the
	// plugin would never be shut down.
	virtual ~PluginProcessor() = default;

	// AUDIO THREAD. Must not allocate, lock or log. Returning false means "I could not process",
	// and the host then passes the input through untouched, as it does while no plugin is loaded.
	// `= 0` makes it "pure virtual": every subclass must implement it.
	virtual bool process(const AudioBlock &p_block) = 0;

	// MAIN THREAD, with the audio thread held off (AudioServer locked). Called just before the
	// processor stops being reachable from the audio thread. CLAP needs this: stop_processing()
	// is an audio-thread call, so it has to happen while the audio thread is guaranteed not to be
	// inside process() at the same moment.
	virtual void detach_from_audio() {}

	// MAIN THREAD, once per Godot frame. CLAP plugins can ask "please call me back on the main
	// thread" (request_callback); this is where that call happens.
	virtual void idle() {}

	// MAIN THREAD. Parameter changes the plugin made itself (its own GUI, say), one at a time.
	// The parameter is unnamed because the default implementation never uses it.
	virtual bool pop_output_param(ParamEvent & /*r_event*/) { return false; }

	// MAIN THREAD. The host just set a parameter (the change is also on its way to process()).
	// VST3 needs this: its edit controller is a separate object from its processor and keeps its
	// own copy of every value, which the host must keep in step.
	virtual void on_param_set(uint32_t /*p_id*/, double /*p_value*/) {}

	// MAIN THREAD. The plugin's own text for a value, such as "-12.0 dB". False if it has none.
	virtual bool value_to_text(uint32_t /*p_id*/, double /*p_value*/, std::string & /*r_text*/) const { return false; }

	// MAIN THREAD. Plugin state: everything the plugin needs to come back exactly as it was,
	// which is more than its parameter values (a loaded sample, a preset name, hidden settings).
	// The bytes are opaque: only the same plugin can read them back. False if it has no state.
	virtual bool save_state(std::vector<uint8_t> & /*r_data*/) { return false; }
	virtual bool load_state(const std::vector<uint8_t> & /*p_data*/) { return false; }

	// MAIN THREAD. The plugin's current value for a parameter, after load_state() changed it, say.
	virtual bool get_param_value(uint32_t /*p_id*/, double & /*r_value*/) const { return false; }

	// PLUGIN GUI. MAIN THREAD, and only ever on one instance at a time (HostedEffect picks it).
	// The host owns the window; the plugin draws inside it. p_parent is the native window id to
	// embed into: an X11 Window, an HWND or an NSView*, by platform (native_window.h).
	virtual bool gui_available() { return false; }
	// Creates the GUI inside p_parent and shows it; r_width/r_height get the size it wants, in
	// physical pixels, after scaling. p_scale is the UI scale to draw at (1.0 = unscaled): the host
	// passes Godot's editor display scale, so plugin windows match the size of the editor's UI.
	virtual bool gui_open(uint64_t /*p_parent*/, double /*p_scale*/, uint32_t & /*r_width*/, uint32_t & /*r_height*/, std::string &r_error) {
		r_error = "this plugin has no GUI";
		return false;
	}
	virtual void gui_close() {}
	virtual bool gui_is_open() const { return false; }
	// A new size the GUI asked for since the last call (the host then resizes its window).
	virtual bool gui_take_resize(uint32_t & /*r_width*/, uint32_t & /*r_height*/) { return false; }
	// True once after the GUI closed itself (CLAP's gui.closed), so the host closes its window.
	virtual bool gui_take_closed() { return false; }

	// A plugin may ask to be deactivated and reactivated, for example after its port layout
	// changed. needs_restart() is polled on the main thread, and restart() then runs with the
	// audio thread held off.
	virtual bool needs_restart() { return false; }
	virtual void restart() {}

	// MAIN THREAD. The parameter list. It can change while the plugin runs (a synth loading a
	// patch with different controls, say): the plugin then flags it, take_params_changed()
	// returns true once, and the host calls read_params() again.
	virtual std::vector<ParamInfo> read_params() const { return {}; }
	virtual bool take_params_changed() { return false; }
};

} // namespace gaph
