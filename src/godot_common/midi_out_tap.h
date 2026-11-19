#pragma once

// MidiOutTap: MIDI a plugin sends out (an arpeggiator's notes, a chord generator's), on the MAIN
// thread, once a frame. The audio thread queued each with the host time it is heard at
// (EffectRuntime::pop_output_midi); here it becomes the `midi_output` signal, and, if a MidiDriver
// is set (set_midi_output_driver), plays through it (MidiDriver.receive_midi, in midi_host): an
// arpeggiator on one instrument playing another. Only the first running instance is read, so an
// effect Godot runs per channel pair does not send each note twice.
//
// latency_ms (forward_latency): the forwarded events are due their own time plus this, so they
// keep their spacing exactly; 0 (the default) plays each as soon as possible, a frame late at most.

#include "core/hosted_effect.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <vector>

namespace gaph_godot {

using namespace godot;

class MidiOutTap {
public:
	void set_driver(Object *p_driver) {
		ERR_FAIL_COND_MSG(p_driver && !p_driver->has_method("receive_midi"), "set_midi_output_driver() takes a MidiDriver.");
		driver = p_driver ? p_driver->get_instance_id() : ObjectID();
	}
	Object *get_driver() const { return ObjectDB::get_instance(driver); }
	void set_latency_ms(double p_ms) { latency_ms = p_ms > 0 ? p_ms : 0.0; }
	double get_latency_ms() const { return latency_ms; }

	// MAIN THREAD, once a frame (the class's own idle step). p_owner emits the signal.
	void drain(gaph::HostedEffect &p_hosted, Object *p_owner) {
		std::vector<gaph::MidiOut> got; // collected with the registry locked, acted on after
		bool first = true;
		p_hosted.for_each_runtime([&](gaph::EffectRuntime &r) {
			gaph::MidiOut m;
			while (r.pop_output_midi(m)) {
				if (first) {
					got.push_back(m); // the others' are copies of the same notes
				}
			}
			first = false;
		});
		Object *d = get_driver();
		const int64_t latency = int64_t(latency_ms * 1000.0);
		for (const gaph::MidiOut &m : got) {
			p_owner->emit_signal("midi_output", int(m.kind), int(m.channel), int(m.index), int64_t(m.value), m.time_us);
			if (d) {
				d->call("receive_midi", int(m.kind), int(m.channel), int(m.index), int64_t(m.value), latency > 0 ? m.time_us + latency : int64_t(0));
			}
		}
	}

private:
	ObjectID driver;
	double latency_ms = 0.0;
};

} // namespace gaph_godot
