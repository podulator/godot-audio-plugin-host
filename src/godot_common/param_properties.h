#pragma once

// Godot glue shared by ClapPluginEffect and Vst3PluginEffect: plugin parameters as inspector
// properties, and the stored {id: value} dictionary.
//
// These are plain functions, not a shared base class. A base class Godot knows about would have
// to be registered in ClassDB by both extensions, under one name, and that is the very clash the
// per-format class names avoid. Functions carry no such registration, so each library simply
// compiles in its own copy (`inline` makes that legal for a header).
//
// Two views of the same values:
//   param_values          ONE stored property, {id: value}. This is what goes into the .tres or
//                         .tscn file. Keyed by id, which the plugin promises is stable.
//   parameters/<name>     one editor-only property per parameter, created on the fly. The
//                         inspector turns the "parameters/" prefix into a foldable group.
// Saving names would break as soon as a plugin update renamed a parameter; saving ids would make
// an unreadable inspector. So each view gets the job it is good at.

#include "core/hosted_effect.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <string>

namespace gaph_godot {

using namespace godot;

// Godot strings are UTF-32 internally; the plugin APIs want UTF-8 char*.
inline std::string to_std(const String &p_s) {
	return std::string(p_s.utf8().get_data());
}

inline String to_godot(const std::string &p_s) {
	return String::utf8(p_s.c_str());
}

inline String property_name_for(const gaph::HostedEffect &p_hosted, const gaph::ParamInfo &p_info) {
	// Characters the inspector or file format would misread are replaced. Two parameters may
	// share a display name (a plugin with two "Mix" knobs), so later ones get their id appended.
	String name = to_godot(p_info.name).replace("/", "-").replace(":", "-").replace("\"", "'");
	if (name.is_empty()) {
		name = "param";
	}
	for (const gaph::ParamInfo &other : p_hosted.get_params()) {
		if (&other == &p_info) {
			break;
		}
		if (other.name == p_info.name) {
			name += " #" + String::num_uint64(p_info.id);
			break;
		}
	}
	return "parameters/" + name;
}

inline bool param_id_from_property(const gaph::HostedEffect &p_hosted, const StringName &p_name, uint32_t &r_id) {
	const String name = p_name;
	if (!name.begins_with("parameters/")) {
		return false;
	}
	for (const gaph::ParamInfo &p : p_hosted.get_params()) {
		if (property_name_for(p_hosted, p) == name) {
			r_id = p.id;
			return true;
		}
	}
	return false;
}

// One slider per visible parameter, for _get_property_list().
inline void append_param_properties(const gaph::HostedEffect &p_hosted, List<PropertyInfo> *p_list) {
	for (const gaph::ParamInfo &p : p_hosted.get_params()) {
		if (p.flags & gaph::PARAM_HIDDEN) {
			continue;
		}
		// PROPERTY_HINT_RANGE takes "min,max,step". A stepped parameter snaps to its steps; a
		// continuous one gets 10000 steps across its range, fine enough for any knob.
		const double range = p.max_value - p.min_value;
		const double step = (p.flags & gaph::PARAM_STEPPED) && p.step_count > 0 ? range / p.step_count : range / 10000.0;
		const String hint = String::num(p.min_value) + "," + String::num(p.max_value) + "," + String::num(step, 8);
		// USAGE_EDITOR without USAGE_STORAGE: shown in the inspector, never written to the file.
		uint32_t usage = PROPERTY_USAGE_EDITOR;
		if (p.flags & gaph::PARAM_READ_ONLY) {
			usage |= PROPERTY_USAGE_READ_ONLY;
		}
		p_list->push_back(PropertyInfo(Variant::FLOAT, property_name_for(p_hosted, p), PROPERTY_HINT_RANGE, hint, usage));
	}
}

inline Dictionary values_to_dictionary(const gaph::HostedEffect &p_hosted) {
	Dictionary d;
	for (const auto &kv : p_hosted.get_values()) {
		d[int64_t(kv.first)] = kv.second;
	}
	return d;
}

inline void values_from_dictionary(gaph::HostedEffect &p_hosted, const Dictionary &p_values) {
	p_hosted.clear_values();
	const Array keys = p_values.keys();
	for (int i = 0; i < keys.size(); i++) {
		p_hosted.set_param(uint32_t(int64_t(keys[i])), double(p_values[keys[i]]));
	}
}

// The parameter list for scripts. `p_text` is the plugin's own display text, if it has any.
inline TypedArray<Dictionary> parameters_to_array(gaph::HostedEffect &p_hosted, const gaph::PluginProcessor *p_probe) {
	TypedArray<Dictionary> out;
	for (const gaph::ParamInfo &p : p_hosted.get_params()) {
		Dictionary d;
		d["id"] = int64_t(p.id);
		d["name"] = to_godot(p.name);
		d["module"] = to_godot(p.module);
		d["min"] = p.min_value;
		d["max"] = p.max_value;
		d["default"] = p.default_value;
		d["value"] = p_hosted.get_param(p.id);
		d["step_count"] = int64_t(p.step_count);
		d["stepped"] = (p.flags & gaph::PARAM_STEPPED) != 0;
		d["hidden"] = (p.flags & gaph::PARAM_HIDDEN) != 0;
		d["read_only"] = (p.flags & gaph::PARAM_READ_ONLY) != 0;
		d["property"] = property_name_for(p_hosted, p);
		std::string text;
		if (p_probe && p_probe->value_to_text(p.id, p_hosted.get_param(p.id), text)) {
			d["text"] = to_godot(text);
		}
		out.push_back(d);
	}
	return out;
}

inline int64_t find_parameter(const gaph::HostedEffect &p_hosted, const String &p_name) {
	const std::string name = to_std(p_name);
	for (const gaph::ParamInfo &p : p_hosted.get_params()) {
		if (p.name == name) {
			return p.id;
		}
	}
	return -1;
}

inline String parameter_text(gaph::HostedEffect &p_hosted, const gaph::PluginProcessor *p_probe, int64_t p_id) {
	const double v = p_hosted.get_param(uint32_t(p_id));
	std::string text;
	if (p_probe && p_probe->value_to_text(uint32_t(p_id), v, text)) {
		return to_godot(text);
	}
	return String::num(v);
}

} // namespace gaph_godot
