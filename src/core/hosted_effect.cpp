#include "hosted_effect.h"

namespace gaph {

std::unique_ptr<PluginProcessor> HostedEffect::create(std::string &r_error) {
	if (!factory) {
		return nullptr; // No plugin loaded: the runtime passes audio through.
	}
	std::unique_ptr<PluginProcessor> p = factory(r_error);
	// State first, then the queued values (queue_all_values) on top in the first block. Both
	// come from the same save, so they agree, and values set since then win, as they should.
	if (p && !state.empty()) {
		p->load_state(state);
	}
	return p;
}

bool HostedEffect::save_state(std::vector<uint8_t> &r_data) {
	PluginProcessor *source = nullptr;
	registry.for_each([&](EffectRuntime &r) {
		if (!source) {
			source = r.get_processor();
		}
	});
	if (!source) {
		source = reference;
	}
	return source && source->save_state(r_data);
}

void HostedEffect::load_state(const std::vector<uint8_t> &p_data) {
	state = p_data;
	spares.clear(); // They carry the previous state. Rebuilt by idle().
	if (reference) {
		reference->load_state(state);
	}
	registry.for_each([&](EffectRuntime &r) {
		if (PluginProcessor *p = r.get_processor()) {
			p->load_state(state);
		}
	});
	// A state is a whole preset: what it says replaces every value set by hand so far.
	values.clear();
}

void HostedEffect::queue_all_values(EffectRuntime &p_runtime) {
	for (const auto &kv : values) { // kv is a std::pair: .first is the id, .second the value.
		p_runtime.queue_param(kv.first, kv.second);
	}
}

std::string HostedEffect::set_plugin(Factory p_factory, std::vector<ParamInfo> p_params) {
	factory = std::move(p_factory);
	params = std::move(p_params);
	spares.clear(); // Made for the previous plugin, and never GUI owners. Rebuilt by idle().
	waiting.clear(); // Every registered runtime gets a processor below.
	// Drop values the new plugin has no parameter for. map::erase returns the next element,
	// which is the safe way to remove items while looping over a map.
	for (auto it = values.begin(); it != values.end();) {
		it = find_param(it->first) ? std::next(it) : values.erase(it);
	}
	// A restored state goes into the reference too, so get_param() can report what it set.
	if (reference && !state.empty()) {
		reference->load_state(state);
	}

	// SWAPPING A PLUGIN WHILE AUDIO IS PLAYING, in three phases, to keep the audio thread
	// blocked for as little time as possible:
	//   1. Create and activate the new plugins with no lock held. This is the slow part: it can
	//      load samples, allocate, build lookup tables.
	//   2. Lock audio, swap the pointers, unlock. Microseconds.
	//   3. Destroy the old plugins with no lock held. Also potentially slow.
	// Doing everything under the lock would be simpler, but every millisecond spent there is a
	// millisecond Godot cannot mix, which is how you get a click.
	std::string first_error;
	std::vector<EffectRuntime *> targets;
	std::vector<std::unique_ptr<PluginProcessor>> fresh;
	registry.for_each([&](EffectRuntime &r) { // [&]: the lambda can use local variables by reference.
		std::string err;
		targets.push_back(&r);
		fresh.push_back(create(err));
		if (!err.empty() && first_error.empty()) {
			first_error = err;
		}
	});

	std::vector<std::unique_ptr<PluginProcessor>> old;
	audio_lock.lock();
	for (size_t i = 0; i < targets.size(); i++) {
		old.push_back(targets[i]->exchange_processor(std::move(fresh[i])));
	}
	audio_lock.unlock();
	for (const std::unique_ptr<PluginProcessor> &p : old) {
		forget_gui_of(p.get());
	}
	old.clear(); // Phase 3: each unique_ptr deletes its processor here, so each plugin shuts down.

	for (EffectRuntime *r : targets) {
		queue_all_values(*r);
	}
	return first_error;
}

// Installs a processor and sends it every value set so far. Values only ever go out together
// with a processor: a runtime without one throws its queue away (see EffectRuntime::process).
void HostedEffect::give_processor(EffectRuntime &p_runtime, std::unique_ptr<PluginProcessor> p_processor, bool p_take_lock) {
	if (p_take_lock) {
		audio_lock.lock();
	}
	std::unique_ptr<PluginProcessor> old = p_runtime.exchange_processor(std::move(p_processor));
	if (p_take_lock) {
		audio_lock.unlock();
	}
	if (old) {
		forget_gui_of(old.get());
		graveyard.push_back(std::move(old));
	}
	queue_all_values(p_runtime);
}

void HostedEffect::attach(EffectRuntime &p_runtime) {
	registry.add(&p_runtime);
	p_runtime.set_sidechain(sidechain);
	if (!factory) {
		return; // Nothing to run yet; set_plugin() will reach this runtime through the registry.
	}
	if (!spares.empty()) {
		std::unique_ptr<PluginProcessor> p = std::move(spares.back());
		spares.pop_back();
		give_processor(p_runtime, std::move(p), false); // Godot already holds the lock.
	} else {
		waiting.push_back(&p_runtime);
	}
}

void HostedEffect::detach(EffectRuntime &p_runtime) {
	registry.remove(&p_runtime);
	waiting.erase(std::remove(waiting.begin(), waiting.end(), &p_runtime), waiting.end());
	// Godot holds the lock, so the swap is safe; the plugin itself is destroyed later, in idle().
	if (std::unique_ptr<PluginProcessor> old = p_runtime.exchange_processor(nullptr)) {
		forget_gui_of(old.get()); // Just a flag: the plugin's GUI closes with it, in idle().
		graveyard.push_back(std::move(old));
	}
}

PluginProcessor *HostedEffect::first_live_processor() {
	PluginProcessor *found = nullptr;
	registry.for_each([&](EffectRuntime &r) {
		if (!found) {
			found = r.get_processor();
		}
	});
	return found;
}

bool HostedEffect::gui_open(uint64_t p_parent, double p_scale, uint32_t &r_width, uint32_t &r_height, std::string &r_error) {
	gui_close();
	PluginProcessor *target = first_live_processor();
	if (!target) {
		target = reference;
	}
	if (!target) {
		r_error = "no plugin loaded";
		return false;
	}
	if (!target->gui_open(p_parent, p_scale, r_width, r_height, r_error)) {
		return false;
	}
	gui_owner = target;
	gui_lost = false;
	return true;
}

void HostedEffect::gui_close() {
	if (gui_owner) {
		gui_owner->gui_close();
		gui_owner = nullptr;
	}
}

bool HostedEffect::gui_wants_live_instance() {
	return gui_owner && gui_owner == reference && first_live_processor() != nullptr;
}

size_t HostedEffect::active_instance_count() {
	size_t n = 0;
	registry.for_each([&](EffectRuntime &r) { n += r.has_processor() ? 1 : 0; });
	return n;
}

void HostedEffect::set_sidechain(std::shared_ptr<SidechainChannel> p_channel) {
	sidechain = std::move(p_channel);
	audio_lock.lock();
	registry.for_each([&](EffectRuntime &r) { r.set_sidechain(sidechain); });
	audio_lock.unlock();
}


const ParamInfo *HostedEffect::find_param(uint32_t p_id) const {
	// A linear search, fine for tens or hundreds of parameters on the main thread. If a plugin
	// with thousands made this show up in a profile, an id -> index map would fix it.
	for (const ParamInfo &p : params) {
		if (p.id == p_id) {
			return &p;
		}
	}
	return nullptr;
}

void HostedEffect::set_param(uint32_t p_id, double p_value) {
	set_param_at(p_id, p_value, 0);
}

void HostedEffect::set_param_at(uint32_t p_id, double p_value, int64_t p_delay_us) {
	// The timestamp is taken here, on the main thread, as close to the script's call as possible.
	const int64_t due = host_time_us() + (p_delay_us > 0 ? p_delay_us : 0);
	// Clamp to the plugin's range. Plugins are supposed to cope with out-of-range values, but
	// not all of them do, and a host should not rely on it.
	if (const ParamInfo *p = find_param(p_id)) {
		p_value = p_value < p->min_value ? p->min_value : (p_value > p->max_value ? p->max_value : p_value);
	}
	values[p_id] = p_value;
	if (reference) {
		reference->on_param_set(p_id, p_value); // Keeps a state saved from it current.
	}
	registry.for_each([&](EffectRuntime &r) {
		r.queue_param(p_id, p_value, due);
		if (PluginProcessor *p = r.get_processor()) {
			p->on_param_set(p_id, p_value);
		}
	});
}

// A value set by hand; otherwise what the plugin itself says (after a state or preset load, it
// differs from the default); otherwise the default. Asking the plugin, rather than copying every
// value it holds into `values`, keeps a saved scene down to what was really changed.
double HostedEffect::get_param(uint32_t p_id) const {
	auto it = values.find(p_id);
	if (it != values.end()) {
		return it->second;
	}
	double v;
	if (reference && reference->get_param_value(p_id, v)) {
		return v;
	}
	const ParamInfo *p = find_param(p_id);
	return p ? p->default_value : 0.0;
}

std::vector<ParamEvent> HostedEffect::idle() {
	// Housekeeping first, all of it outside the audio lock: the slow plugin calls that attach()
	// and detach() had to put off.
	graveyard.clear();
	if (factory) {
		std::vector<EffectRuntime *> todo;
		todo.swap(waiting);
		for (EffectRuntime *r : todo) {
			std::string err;
			std::unique_ptr<PluginProcessor> p = create(err);
			note_error(err);
			if (p) {
				give_processor(*r, std::move(p), true);
			}
		}
		while (spares.size() < spare_count) {
			std::string err;
			std::unique_ptr<PluginProcessor> p = create(err);
			note_error(err);
			if (!p) {
				break;
			}
			spares.push_back(std::move(p));
		}
	}

	std::vector<ParamEvent> changes;
	std::vector<PluginProcessor *> restarts;
	PluginProcessor *list_source = nullptr;

	// The reference never processes audio, but it is a plugin instance all the same: it may ask
	// for main-thread time, or change its parameter list after a state load.
	if (reference) {
		reference->idle();
		if (reference->take_params_changed()) {
			list_source = reference;
		}
	}
	registry.for_each([&](EffectRuntime &r) {
		PluginProcessor *p = r.get_processor();
		if (!p) {
			return; // Inside a lambda, `return` just moves on to the next runtime.
		}
		p->idle();
		if (p->take_params_changed() && !list_source) {
			list_source = p;
		}
		ParamEvent ev;
		while (p->pop_output_param(ev)) {
			values[ev.param_id] = ev.value;
			changes.push_back(ev);
		}
		if (p->needs_restart()) {
			restarts.push_back(p);
		}
	});
	// Restarts are batched so the audio lock is taken at most once per frame.
	if (!restarts.empty()) {
		audio_lock.lock();
		for (PluginProcessor *p : restarts) {
			p->restart();
		}
		audio_lock.unlock();
	}
	if (list_source) {
		refresh_params(list_source);
	}
	// Forward every reported change to every instance, the reporting one included:
	//   - VST3: a change made in the plugin's window reaches only its edit controller. The
	//     processor that makes the sound hears of it only if the host sends it on.
	//   - Any format: a surround bus runs several copies of the plugin, and turning a knob on one
	//     copy should turn it on all of them.
	// For a CLAP plugin the reporting instance gets its own value back, which is harmless.
	for (const ParamEvent &ev : changes) {
		if (reference) {
			reference->on_param_set(ev.param_id, ev.value);
		}
		registry.for_each([&](EffectRuntime &r) { r.queue_param(ev.param_id, ev.value); });
	}
	return changes;
}

// The parameter list changed. Values for parameters that are gone are dropped, so they are not
// saved or sent to a plugin that would not know them; everything else stays.
void HostedEffect::refresh_params(PluginProcessor *p_source) {
	params = p_source->read_params();
	for (auto it = values.begin(); it != values.end();) {
		it = find_param(it->first) ? std::next(it) : values.erase(it);
	}
	params_list_changed = true;
}

uint64_t HostedEffect::frames_processed() {
	uint64_t total = 0;
	registry.for_each([&](EffectRuntime &r) { total += r.get_frames_processed(); });
	return total;
}

} // namespace gaph
