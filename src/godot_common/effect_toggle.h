#pragma once

// Was that click on the effect's enable checkbox?
//
// In the Audio panel's effect list, clicking an effect's checkbox also selects its row, and the
// selection is what makes the editor call EditorPlugin::_edit(), which opens the plugin window.
// Both happen while one click is handled (editor_audio_buses.cpp: cell_selected, then
// item_edited, which toggles the effect through set_bus_effect_enabled). So _edit() takes a
// snapshot of where the effect sits and whether it is enabled, and the open is put off with
// call_deferred, to run once the click is fully handled. If the enabled state changed in between,
// the click was on the checkbox, and no window opens. Nor does one open for an
// effect that is switched off in every slot it occupies.

#include <godot_cpp/classes/audio_effect.hpp>
#include <godot_cpp/classes/audio_server.hpp>

#include <vector>

namespace gaph_godot {

using namespace godot;

// The enabled flag of every slot holding p_effect, in bus and slot order.
inline std::vector<bool> effect_enabled_snapshot(const Ref<AudioEffect> &p_effect) {
	std::vector<bool> flags;
	AudioServer *as = AudioServer::get_singleton();
	for (int bus = 0; bus < as->get_bus_count(); bus++) {
		for (int i = 0; i < as->get_bus_effect_count(bus); i++) {
			if (as->get_bus_effect(bus, i) == p_effect) {
				flags.push_back(as->is_bus_effect_enabled(bus, i));
			}
		}
	}
	return flags;
}

// True if p_effect is enabled in at least one slot. An effect on no bus at all (a script's, say)
// counts as not enabled: the bus panel is the only place this applies.
inline bool any_enabled(const std::vector<bool> &p_flags) {
	for (bool f : p_flags) {
		if (f) {
			return true;
		}
	}
	return false;
}

} // namespace gaph_godot
