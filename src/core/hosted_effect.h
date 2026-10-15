#pragma once

// HostedEffect: everything an effect *resource* does that is the same for CLAP and VST3.
//
//   resource (one, in the inspector)        instances (Godot makes as many as it needs)
//   ┌───────────────────────────┐           ┌──────────────────────────────────┐
//   │ HostedEffect              │──────────>│ EffectRuntime ── PluginProcessor │  bus A, L/R
//   │  - factory: how to create │           ├──────────────────────────────────┤
//   │    one plugin instance    │──────────>│ EffectRuntime ── PluginProcessor │  bus B, L/R
//   │  - parameter list         │           └──────────────────────────────────┘
//   │  - current values         │
//   └───────────────────────────┘
//
// The format-specific Godot class (ClapPluginEffect, later Vst3PluginEffect) holds one of these
// and supplies the "factory": a function that makes one plugin instance. Everything else lives
// here, written once.
//
// No Godot code: the audio lock arrives as two plain callbacks, so this class could be tested
// without an engine.

#include "effect_runtime.h"
#include "plugin_processor.h"
#include "runtime_registry.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace gaph {

class HostedEffect {
public:
	// std::function holds any callable with this signature: a lambda, a function pointer, a
	// functor. The format code passes a lambda that captures what it needs (the loaded library,
	// the plugin id, the sample rate), so this class never has to know what those are.
	using Factory = std::function<std::unique_ptr<PluginProcessor>(std::string &r_error)>;

	struct AudioLock {
		std::function<void()> lock;
		std::function<void()> unlock;
	};

	explicit HostedEffect(AudioLock p_lock) :
			audio_lock(std::move(p_lock)) {}

	// MAIN THREAD. Switches every live instance to a new plugin, and every instance made later.
	// An empty factory unloads. Stored values for ids that the new plugin lacks are dropped.
	// Returns the first error, if any; instances whose plugin failed to start pass audio through.
	std::string set_plugin(Factory p_factory, std::vector<ParamInfo> p_params);
	bool has_plugin() const { return bool(factory); }

	// A Godot instance was created (attach) or is being destroyed (detach). Godot calls both
	// with the AUDIO LOCK HELD, so neither may do anything slow: no plugin is created or destroyed
	// here. attach() hands over a ready-made spare if there is one; otherwise the runtime passes
	// audio through until the next idle() builds its plugin outside the lock. detach() moves the
	// old plugin to a graveyard that idle() empties, also outside the lock.
	void attach(EffectRuntime &p_runtime);
	void detach(EffectRuntime &p_runtime);

	// How many ready plugin instances to keep for attach(). Each is a whole extra instance in
	// memory; 0 means every new instance waits a frame or two for its plugin.
	void set_spare_count(uint32_t p_count) { spare_count = p_count; spares.clear(); }
	uint32_t get_spare_count() const { return spare_count; }
	// Runtimes currently running a plugin (the rest pass audio through, waiting for one).
	size_t active_instance_count();

	// MAIN THREAD. Every instance, current and future, reads this sidechain channel; nullptr
	// for none. Takes the audio lock to swap it into running instances.
	void set_sidechain(std::shared_ptr<SidechainChannel> p_channel);

	// PLUGIN STATE. MAIN THREAD.
	// The reference processor is an instance that exists even when no bus is running (the Godot
	// classes' probe). It answers questions when there is no live instance, and is kept in step
	// with the state, so saving from it gives the right bytes.
	void set_reference(PluginProcessor *p_reference) {
		if (gui_owner && gui_owner == reference && p_reference != reference) {
			gui_close(); // Main thread, before the old reference is destroyed by the caller.
			gui_lost = true;
		}
		reference = p_reference;
	}
	// The state of a live instance if there is one (it has heard every parameter change), else
	// of the reference. False if the plugin keeps no state.
	bool save_state(std::vector<uint8_t> &r_data);
	// Applies to every instance, now and later. Clears the values set by hand: a state is a
	// whole preset. get_param() then reports what the state set, by asking the plugin.
	void load_state(const std::vector<uint8_t> &p_data);
	// Keeps the bytes to apply to the plugin about to be loaded; see set_plugin(). Values set by
	// hand (a saved scene's param_values) are sent after it, so they take precedence.
	void set_pending_state(std::vector<uint8_t> p_data) { state = std::move(p_data); }
	void clear_state() { state.clear(); }

	const std::vector<ParamInfo> &get_params() const { return params; }
	const ParamInfo *find_param(uint32_t p_id) const;

	// MAIN THREAD. Stores the value and forwards it to every instance. Values are kept even while
	// no plugin is loaded: when Godot loads a saved scene it may set parameter values before it
	// sets the plugin path, depending on property order.
	void set_param(uint32_t p_id, double p_value);
	// The same, but heard p_delay_us from now, to the sample (block_clock.h). The stored value,
	// and so the inspector, changes at once.
	void set_param_at(uint32_t p_id, double p_value, int64_t p_delay_us);
	double get_param(uint32_t p_id) const;
	bool is_param_set(uint32_t p_id) const { return values.count(p_id) != 0; }
	const std::map<uint32_t, double> &get_values() const { return values; }
	void clear_values() { values.clear(); }

	// MAIN THREAD, once per frame. Runs each plugin's main-thread work, applies restart requests,
	// and collects parameter changes the plugins made themselves (already stored). With several
	// instances the same change can arrive more than once; the last one wins.
	std::vector<ParamEvent> idle();

	// True once after idle() found the parameter list changed: rebuild the inspector.
	bool take_params_list_changed() {
		const bool changed = params_list_changed;
		params_list_changed = false;
		return changed;
	}

	// The first plugin-creation error since the last call, if any. idle() creates plugins in the
	// background, so this is where its failures surface.
	std::string take_error() {
		std::string e;
		e.swap(last_error);
		return e;
	}

	// PLUGIN GUI. MAIN THREAD.
	// Which instance shows it: a live one on a bus if there is any, so meters and analysers see
	// real audio (a spectrum display, say), otherwise the reference. When that instance
	// goes away (Godot rebuilt the bus, the plugin changed), the GUI is lost, take_gui_lost()
	// says so once, and the caller reopens it in the same window, on whichever instance is now
	// the best choice.
	bool gui_open(uint64_t p_parent, double p_scale, uint32_t &r_width, uint32_t &r_height, std::string &r_error);
	void gui_close();
	PluginProcessor *get_gui_owner() const { return gui_owner; }
	bool take_gui_lost() {
		const bool lost = gui_lost;
		gui_lost = false;
		return lost;
	}
	// The GUI shows on the reference although a live instance now exists: worth moving.
	bool gui_wants_live_instance();

	size_t instance_count() { return registry.size(); }
	uint64_t frames_processed();

private:
	std::unique_ptr<PluginProcessor> create(std::string &r_error);
	void queue_all_values(EffectRuntime &p_runtime);
	void give_processor(EffectRuntime &p_runtime, std::unique_ptr<PluginProcessor> p_processor, bool p_take_lock);
	void refresh_params(PluginProcessor *p_source);
	bool params_list_changed = false;
	std::string last_error;
	void note_error(const std::string &p_err) {
		if (!p_err.empty() && last_error.empty()) {
			last_error = p_err;
		}
	}

	AudioLock audio_lock;
	Factory factory;
	std::vector<ParamInfo> params;
	// Only values someone actually set. Anything absent is whatever the plugin holds (its default,
	// or what a loaded state set), so a saved scene records only what was changed. std::map keeps the keys sorted, which keeps saved files
	// stable from one save to the next.
	std::map<uint32_t, double> values;
	std::shared_ptr<SidechainChannel> sidechain;
	uint32_t spare_count = 1;
	std::vector<std::unique_ptr<PluginProcessor>> spares; // Ready for attach(); same plugin and state.
	std::vector<EffectRuntime *> waiting; // Attached without a spare; built in idle().
	std::vector<std::unique_ptr<PluginProcessor>> graveyard; // Detached; destroyed in idle().
	PluginProcessor *gui_owner = nullptr; // The instance whose GUI is open, if any.
	bool gui_lost = false;
	PluginProcessor *first_live_processor();
	// A processor is about to be destroyed: if it shows the GUI, the GUI is lost. Its own
	// destructor closes the plugin side (gui.destroy / view removed).
	void forget_gui_of(PluginProcessor *p_dying) {
		if (p_dying && p_dying == gui_owner) {
			gui_owner = nullptr;
			gui_lost = true;
		}
	}
	// The last state loaded or restored, applied to every processor create() makes.
	std::vector<uint8_t> state;
	PluginProcessor *reference = nullptr;
	RuntimeRegistry registry;

};

} // namespace gaph
