#pragma once

// DspMonitor: what a plugin costs, for Godot's Debugger > Monitors panel and for scripts. The
// runtime measures each process() call (EffectRuntime::get_dsp_load); here it becomes a
// percentage of the audio's real time, summed over the plugin's running instances: 100 % is the
// whole of it (the plugin alone would make the audio miss its deadline).
//
// register_monitor(name) adds "Plugins/<name> (DSP %)" to Performance's custom monitors, which the
// editor's Monitors panel shows for a running game; it is removed again with the resource (or
// unregister_monitor()), so the panel never calls a freed object. MAIN THREAD.

#include "core/hosted_effect.h"

#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string_name.hpp>

namespace gaph_godot {

using namespace godot;

class DspMonitor {
public:
	~DspMonitor() { unregister(); }

	static double load_percent(gaph::HostedEffect &p_hosted) { return p_hosted.dsp_load() * 100.0; }
	static double peak_percent(gaph::HostedEffect &p_hosted) { return p_hosted.take_dsp_peak() * 100.0; }

	// p_owner's get_dsp_load is what the panel polls.
	void register_as(Object *p_owner, const String &p_name) {
		unregister();
		id = StringName(String("Plugins/") + p_name + String(" (DSP %)"));
		Performance *perf = Performance::get_singleton();
		if (perf->has_custom_monitor(id)) {
			perf->remove_custom_monitor(id); // another resource of that name: the newest wins
		}
		perf->add_custom_monitor(id, Callable(p_owner, "get_dsp_load"));
	}

	void unregister() {
		if (id.is_empty()) {
			return;
		}
		Performance *perf = Performance::get_singleton();
		if (perf && perf->has_custom_monitor(id)) {
			perf->remove_custom_monitor(id);
		}
		id = StringName();
	}

	bool is_registered() const { return !id.is_empty(); }

private:
	StringName id;
};

} // namespace gaph_godot
