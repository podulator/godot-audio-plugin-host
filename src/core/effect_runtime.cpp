#include "effect_runtime.h"

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

// AUDIO THREAD. Builds this block's sorted event list from the pending list and the queue.
// An event due within the block gets its frame; one due later waits in `pending`; one whose
// moment has already passed goes at frame 0 (it can only be late, never early). When either
// list is full, events are delivered at frame 0 rather than dropped: a change heard slightly
// early beats a change never heard.
void EffectRuntime::gather_events(uint32_t p_frames) {
	event_count = 0;
	event_next = 0;
	const int64_t block_start = clock.current_block_start();
	// An event's frame within this block, from the stream position it was given when it first
	// arrived (see BlockClock::absolute_frame_for for why once, not every block).
	auto frame_in_block = [&](ParamEvent &ev) -> int64_t {
		if (ev.due_us == 0) {
			return 0;
		}
		if (ev.due_frame < 0) {
			ev.due_frame = clock.absolute_frame_for(ev.due_us);
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
		place(ev);
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

	if (!processor->process(block)) {
		// The plugin refused or failed: pass the input through rather than output garbage, or
		// silence that would sound like a broken bus.
		std::memcpy(p_dst, p_src, sizeof(float) * CHANNELS * p_frames);
		return;
	}

	// 4. Reinterleave into Godot's buffer.
	for (uint32_t i = 0; i < p_frames; i++) {
		p_dst[i * 2] = out_ptrs[0][i];
		p_dst[i * 2 + 1] = out_ptrs[1][i];
	}
}

} // namespace gaph
