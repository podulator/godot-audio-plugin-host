#include "effect_runtime.h"

#include <algorithm>
#include <cmath>

#include <cstring>

namespace gaph {

// The `: param_queue(4096)` part is a member initialiser list: it constructs the queue with that
// capacity before the constructor body runs. RtQueue has no default constructor, so this is the
// only way to build it. 4096 leaves room for a whole preset's worth of parameters at once, sent
// when a plugin is first loaded.
EffectRuntime::EffectRuntime() :
		param_queue(4096) {
	events.resize(MAX_EVENTS_PER_BLOCK);
	chunk_events.resize(MAX_EVENTS_PER_BLOCK);
	pending.reserve(MAX_PENDING_EVENTS); // reserve: capacity now, so push_back never allocates.
	in_storage.resize(CHANNELS * MAX_BLOCK);
	out_storage.resize(CHANNELS * MAX_BLOCK);
	for (uint32_t c = 0; c < CHANNELS; c++) {
		in_ptrs[c] = in_storage.data() + c * MAX_BLOCK;
		out_ptrs[c] = out_storage.data() + c * MAX_BLOCK;
	}
	aux_storage.resize(CHANNELS * SidechainChannel::MAX_FRAMES);
	for (uint32_t c = 0; c < CHANNELS; c++) {
		aux_base[c] = aux_storage.data() + c * SidechainChannel::MAX_FRAMES;
	}
	extra_storage.resize(size_t(MAX_EXTRA_OUTPUTS) * CHANNELS * MAX_BLOCK);
	for (uint32_t c = 0; c < MAX_EXTRA_OUTPUTS * CHANNELS; c++) {
		extra_ptrs[c] = extra_storage.data() + size_t(c) * MAX_BLOCK;
	}
}

void EffectRuntime::process_with_outputs(const float *p_src, float *p_dst, uint32_t p_frames, float *const *p_extra, uint32_t p_extra_count) {
	extra_dst = p_extra;
	extra_dst_count = p_extra_count < MAX_EXTRA_OUTPUTS ? p_extra_count : MAX_EXTRA_OUTPUTS;
	// Silence first: a plugin with fewer outputs, a failed block, or no plugin leaves them so.
	for (uint32_t k = 0; k < extra_dst_count; k++) {
		std::memset(extra_dst[k], 0, sizeof(float) * CHANNELS * p_frames);
	}
	process_at(p_src, p_dst, p_frames, host_time_us());
	extra_dst = nullptr;
	extra_dst_count = 0;
}

std::unique_ptr<PluginProcessor> EffectRuntime::exchange_processor(std::unique_ptr<PluginProcessor> p_processor) {
	if (processor) {
		processor->detach_from_audio();
	}
	// std::move turns ownership over: `old` now owns the previous processor and `processor` is
	// empty, until it is given the new one on the next line.
	std::unique_ptr<PluginProcessor> old = std::move(processor);
	processor = std::move(p_processor);
	return old;
}

bool EffectRuntime::queue_param(uint32_t p_param_id, double p_value, int64_t p_due_us) {
	ParamEvent ev;
	ev.param_id = p_param_id;
	ev.value = p_value;
	ev.due_us = p_due_us;
	return param_queue.push(ev);
}

// MIDI and TRANSPORT events (the caller sets the type; anything else counts as MIDI).
bool EffectRuntime::queue_midi(const ParamEvent &p_midi) {
	ParamEvent ev = p_midi;
	if (ev.type != ParamEvent::TRANSPORT) {
		ev.type = ParamEvent::MIDI;
	}
	ev.due_frame = -1;
	return param_queue.push(ev);
}

bool EffectRuntime::queue_drop(uint64_t p_source, int64_t p_from_us) {
	ParamEvent ev;
	ev.type = ParamEvent::DROP;
	ev.source = p_source;
	ev.due_us = p_from_us;
	return param_queue.push(ev);
}

// AUDIO THREAD. A DROP: every MIDI event from its source due at or after its time goes, both those
// waiting for later blocks and those already placed in this one. Everything queued before the DROP
// has been placed by now (the queue keeps order), so nothing it should catch is still in transit.
void EffectRuntime::apply_drop(const ParamEvent &p_drop) {
	auto doomed = [&](const ParamEvent &e) {
		return (e.type == ParamEvent::MIDI || e.type == ParamEvent::TRANSPORT) && e.source == p_drop.source && e.due_us != 0 &&
				e.due_us >= p_drop.due_us;
	};
	pending.erase(std::remove_if(pending.begin(), pending.end(), doomed), pending.end());
	uint32_t keep = 0;
	for (uint32_t i = 0; i < event_count; i++) {
		if (!doomed(events[i])) {
			events[keep++] = events[i];
		}
	}
	event_count = keep;
}

// AUDIO THREAD. Builds this block's sorted event list from the pending list and the queue.
// An event due within the block gets its frame; one due later waits in `pending`; one whose
// moment has already passed goes at frame 0 (it can only be late, never early). When either
// list is full, events are delivered at frame 0 rather than dropped: a change heard slightly
// early beats a change never heard.
// AUDIO THREAD. A MIDI event's stream position: from its source's mapping (see MIDI SPACING in the
// header), set up or renewed from the clock when needed.
int64_t EffectRuntime::midi_frame_for(const ParamEvent &p_event) {
	const int64_t from_clock = clock.absolute_frame_for(p_event.due_us);
	MidiAnchor *anchor = nullptr;
	MidiAnchor *oldest = &midi_anchors[0];
	for (MidiAnchor &a : midi_anchors) {
		if (a.used != 0 && a.source == p_event.source) {
			anchor = &a;
			break;
		}
		if (a.used < oldest->used) {
			oldest = &a;
		}
	}
	const double rate = clock.get_rate();
	const int64_t tolerance = int64_t(double(MIDI_REANCHOR_US) * rate / 1e6);
	if (anchor) {
		const int64_t spaced = anchor->base_frame + int64_t(std::llround(double(p_event.due_us - anchor->base_us) * rate / 1e6));
		if (std::llabs(spaced - from_clock) <= tolerance) {
			anchor->used = ++midi_anchor_uses;
			return spaced;
		}
	} else {
		anchor = oldest;
	}
	*anchor = MidiAnchor{ p_event.source, p_event.due_us, from_clock, ++midi_anchor_uses };
	return from_clock;
}

void EffectRuntime::gather_events(uint32_t p_frames) {
	event_count = 0;
	event_next = 0;
	const int64_t block_start = clock.current_block_start();
	// After a gap in the audio the clock has a new anchor, and the stream positions worked out for
	// waiting events assume none: work them out again (from their due time, which is still right).
	if (clock.get_restarts() != clock_restarts_seen) {
		clock_restarts_seen = clock.get_restarts();
		for (ParamEvent &waiting : pending) {
			waiting.due_frame = -1;
		}
		for (MidiAnchor &a : midi_anchors) {
			a = MidiAnchor{};
		}
	}
	// An event's frame within this block, from the stream position it was given when it first
	// arrived (see BlockClock::absolute_frame_for for why once, not every block).
	auto frame_in_block = [&](ParamEvent &ev) -> int64_t {
		if (ev.due_us == 0) {
			return 0;
		}
		if (ev.due_frame < 0) {
			const bool from_driver = ev.type == ParamEvent::MIDI || ev.type == ParamEvent::TRANSPORT;
			ev.due_frame = from_driver ? midi_frame_for(ev) : clock.absolute_frame_for(ev.due_us);
		}
		return ev.due_frame - block_start;
	};
	auto place = [&](ParamEvent ev) {
		const int64_t f = frame_in_block(ev);
		if (f >= int64_t(p_frames) && pending.size() < MAX_PENDING_EVENTS) {
			pending.push_back(ev);
			return;
		}
		if (event_count == MAX_EVENTS_PER_BLOCK) {
			if (pending.size() < MAX_PENDING_EVENTS) {
				pending.push_back(ev); // No room this block: retried next block.
			}
			return;
		}
		ev.frame_offset = f < 0 ? 0 : (f >= int64_t(p_frames) ? 0 : uint32_t(f));
		// Insertion sort, keeping arrival order among equal offsets. At most a few hundred
		// events, already nearly in order, so this is quick and allocation-free.
		uint32_t i = event_count++;
		while (i > 0 && events[i - 1].frame_offset > ev.frame_offset) {
			events[i] = events[i - 1];
			i--;
		}
		events[i] = ev;
	};

	// Pending first: they are older. Taken out by swapping with the last element, which is
	// O(1); their order does not matter because place() sorts.
	const size_t waiting = pending.size();
	for (size_t i = waiting; i-- > 0;) {
		ParamEvent ev = pending[i];
		if (frame_in_block(ev) < int64_t(p_frames)) {
			pending[i] = pending.back();
			pending.pop_back();
			place(ev);
		}
	}
	ParamEvent ev;
	while (param_queue.pop(ev)) {
		if (ev.type == ParamEvent::DROP) {
			apply_drop(ev);
		} else {
			place(ev);
		}
	}
}

void EffectRuntime::process_at(const float *p_src, float *p_dst, uint32_t p_frames, int64_t p_now_us) {
	frames_processed.fetch_add(p_frames, std::memory_order_relaxed);
	clock.begin_block(p_now_us, p_frames); // Always, so the clock is settled when a plugin arrives.
	if (!processor) {
		// Values queued while no plugin runs are thrown away: whoever installs a plugin sends it
		// every value again (HostedEffect::give_processor), so nothing is lost, and the queue
		// cannot fill up during a long wait.
		ParamEvent dropped;
		while (param_queue.pop(dropped)) {
		}
		pending.clear();
		// No plugin: an exact copy. src and dst are separate buffers in Godot (it swaps them after
		// each effect), so something has to be written to dst even when nothing changes.
		std::memcpy(p_dst, p_src, sizeof(float) * CHANNELS * p_frames);
		return;
	}
	gather_events(p_frames);

	// The sidechain block covers the whole Godot block, so it is read before splitting. A block
	// longer than a channel can hold (never in Godot today) gets no sidechain at all, rather than
	// a partial one.
	if (sidechain.channel) {
		if (p_frames <= SidechainChannel::MAX_FRAMES) {
			sidechain.read(aux_base[0], aux_base[1], p_frames);
		} else {
			std::memset(aux_storage.data(), 0, sizeof(float) * aux_storage.size());
		}
	}
	// Split into chunks of at most MAX_BLOCK. Each frame is CHANNELS floats, hence the
	// `done * CHANNELS` when stepping through the interleaved buffers.
	uint32_t done = 0;
	while (done < p_frames) {
		const uint32_t n = (p_frames - done < MAX_BLOCK) ? p_frames - done : MAX_BLOCK;
		process_chunk(p_src + done * CHANNELS, p_dst + done * CHANNELS, n, done);
		done += n;
	}
}

void EffectRuntime::process_chunk(const float *p_src, float *p_dst, uint32_t p_frames, uint32_t p_offset) {
	// 1. This chunk's slice of the block's sorted events, re-based to the chunk's first frame.
	uint32_t count = 0;
	while (event_next < event_count && events[event_next].frame_offset < p_offset + p_frames) {
		chunk_events[count] = events[event_next++];
		chunk_events[count].frame_offset -= p_offset;
		count++;
	}

	// 2. Deinterleave: frame i is at p_src[2i] (left) and p_src[2i+1] (right).
	for (uint32_t i = 0; i < p_frames; i++) {
		in_ptrs[0][i] = p_src[i * 2];
		in_ptrs[1][i] = p_src[i * 2 + 1];
	}

	// 3. Run the plugin.
	AudioBlock block;
	block.frames = p_frames;
	block.channels = CHANNELS;
	block.inputs = in_ptrs;
	block.outputs = out_ptrs;
	block.param_events = chunk_events.data();
	block.param_event_count = count;
	if (sidechain.channel && p_offset + p_frames <= SidechainChannel::MAX_FRAMES) {
		aux_ptrs[0] = aux_base[0] + p_offset;
		aux_ptrs[1] = aux_base[1] + p_offset;
		block.aux_inputs = aux_ptrs;
		block.aux_channels = CHANNELS;
	}
	if (extra_dst_count > 0) {
		block.extra_outputs = extra_ptrs;
		block.extra_output_count = extra_dst_count;
	}

	if (!processor->process(block)) {
		// The plugin refused or failed: pass the input through rather than output garbage, or
		// silence that would sound like a broken bus.
		std::memcpy(p_dst, p_src, sizeof(float) * CHANNELS * p_frames);
		return;
	}

	// 4. Reinterleave into Godot's buffer, and any extra outputs into theirs.
	for (uint32_t i = 0; i < p_frames; i++) {
		p_dst[i * 2] = out_ptrs[0][i];
		p_dst[i * 2 + 1] = out_ptrs[1][i];
	}
	for (uint32_t k = 0; k < extra_dst_count; k++) {
		float *dst = extra_dst[k] + size_t(p_offset) * CHANNELS;
		const float *left = extra_ptrs[k * CHANNELS];
		const float *right = extra_ptrs[k * CHANNELS + 1];
		for (uint32_t i = 0; i < p_frames; i++) {
			dst[i * 2] = left[i];
			dst[i * 2 + 1] = right[i];
		}
	}
}

} // namespace gaph
