#pragma once

// MidiLearn: hardware controllers mapped to plugin parameters ("MIDI learn"), for effects and
// instruments alike. MAIN THREAD.
//
// A mapping is (channel 1-16, controller 0-127) -> parameter id; a controller value 0..127 moves the
// parameter over its whole range, as a short ramp (RAMP_SECONDS), so a 7-bit knob glides rather
// than steps. learn(id) waits for the next controller that moves and maps it to id (replacing any
// mapping of that controller or that parameter). A MidiInput feeds it (MidiInput.add_learner), as
// any script can (midi_controller()). Saved with the resource as `midi_map`:
// { "channel:controller": parameter id }.

#include "core/hosted_effect.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/string.hpp>

#include <map>
#include <utility>

namespace gaph_godot {

using namespace godot;

class MidiLearn {
public:
	static constexpr double RAMP_SECONDS = 0.01;

	void learn(int64_t p_param_id) { learning = p_param_id; }
	void cancel_learn() { learning = -1; }
	bool is_learning() const { return learning >= 0; }

	void map(int p_channel, int p_controller, int64_t p_param_id) {
		forget_param(p_param_id);
		mappings[{ p_channel, p_controller }] = p_param_id;
	}
	void unmap(int64_t p_param_id) { forget_param(p_param_id); }
	void clear() { mappings.clear(); }

	// A controller moved: learned if learning, then applied. Returns the parameter moved, or -1.
	int64_t controller(gaph::HostedEffect &p_hosted, int p_channel, int p_controller, int p_value) {
		if (learning >= 0) {
			map(p_channel, p_controller, learning);
			learning = -1;
		}
		auto it = mappings.find({ p_channel, p_controller });
		if (it == mappings.end()) {
			return -1;
		}
		const gaph::ParamInfo *info = p_hosted.find_param(uint32_t(it->second));
		if (!info) {
			return -1;
		}
		const double t = double(p_value < 0 ? 0 : (p_value > 127 ? 127 : p_value)) / 127.0;
		double v = info->min_value + t * (info->max_value - info->min_value);
		if (info->flags & gaph::PARAM_STEPPED) {
			v = double(int64_t(v + 0.5));
		}
		p_hosted.set_param_ramp(uint32_t(it->second), v, RAMP_SECONDS, 0);
		return it->second;
	}

	Dictionary to_dictionary() const {
		Dictionary d;
		for (const auto &entry : mappings) {
			d[vformat("%d:%d", entry.first.first, entry.first.second)] = entry.second;
		}
		return d;
	}
	void from_dictionary(const Dictionary &p_d) {
		mappings.clear();
		const Array keys = p_d.keys();
		for (int i = 0; i < keys.size(); i++) {
			const PackedStringArray parts = String(keys[i]).split(":");
			if (parts.size() == 2) {
				mappings[{ int(parts[0].to_int()), int(parts[1].to_int()) }] = int64_t(p_d[keys[i]]);
			}
		}
	}

private:
	void forget_param(int64_t p_param_id) {
		for (auto it = mappings.begin(); it != mappings.end();) {
			it = it->second == p_param_id ? mappings.erase(it) : std::next(it);
		}
	}

	std::map<std::pair<int, int>, int64_t> mappings;
	int64_t learning = -1;
};

} // namespace gaph_godot
