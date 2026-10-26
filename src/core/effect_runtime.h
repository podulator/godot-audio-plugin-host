#pragma once

// EffectRuntime: the format-independent half of ONE Godot effect instance.
//
// HOW GODOT RUNS AN EFFECT (verified in servers/audio_server.cpp, Godot 4.5)
// You add an AudioEffect *resource* to a bus. The resource is only a description: the settings.
// For the actual processing Godot calls resource->instantiate(), which returns an
// AudioEffectInstance, and then calls instance->process(src, dst, frames) on the audio thread for
// every block. Three facts about this matter a lot to a plugin host:
//
//   1. Blocks are always 512 frames (AudioServer's buffer_size is hard-coded).
//   2. Godot makes one instance per stereo channel pair: a 7.1 bus gets 4 instances of the same
//      resource, so we run 4 copies of the plugin.
//   3. Instances are created and destroyed on the main thread *while Godot holds the audio lock*,
//      and Godot recreates all of a bus's instances whenever the bus layout changes. So nothing
//      that has to last (parameter values, plugin state) may live only in an instance.
//
// WHAT THIS CLASS DOES
//   - Converts Godot's interleaved stereo (LRLR) into planar buffers (LLLL, RRRR) and back.
//   - Splits blocks larger than MAX_BLOCK, so a plugin never gets more frames than it was
//     activated for. Godot never sends more today; this is insurance.
//   - Drains parameter changes from the main thread (an RtQueue) and hands them to the plugin.
//   - Passes audio through untouched when there is no plugin, or when the plugin fails.

#include "block_clock.h"
#include "plugin_processor.h"
#include "rt_queue.h"
#include "sidechain.h"

#include <atomic>
#include <memory>
#include <vector>

namespace gaph {

class EffectRuntime {
public:
	// `static constexpr` is a compile-time constant that belongs to the class rather than to each
	// object.
	static constexpr uint32_t CHANNELS = 2;
	static constexpr uint32_t MAX_BLOCK = 512;
	static constexpr uint32_t MAX_EVENTS_PER_BLOCK = 256;
	static constexpr uint32_t MAX_PENDING_EVENTS = 1024; // Scheduled for later blocks.

	// Allocates every buffer up front. After construction, process() never allocates.
	EffectRuntime();

	// MAIN THREAD, with the audio thread held off. Installs a new processor (or none, with
	// nullptr) and hands the old one back.
	//
	// WHY NO ATOMICS ARE NEEDED HERE
	// The obvious worry: the audio thread is reading `processor` in process() while the main
	// thread replaces it. But the caller holds Godot's audio lock (AudioServer.lock()), and the
	// audio thread takes that same lock around each whole mix. So while we hold it, the audio
	// thread is guaranteed to be outside process(), and a plain pointer swap is safe.
	//
	// The old processor is returned rather than deleted here because shutting a plugin down can
	// be slow, and the caller should do it after releasing the lock, so audio is not held up.
	// std::unique_ptr is a pointer that owns its object: when the unique_ptr goes away, the
	// object is deleted. Returning it transfers that ownership to the caller.
	std::unique_ptr<PluginProcessor> exchange_processor(std::unique_ptr<PluginProcessor> p_processor);

	bool has_processor() const { return processor != nullptr; }
	// MAIN THREAD only, and only for the calls PluginProcessor marks as main-thread.
	PluginProcessor *get_processor() { return processor.get(); }

	// MAIN THREAD, with the audio thread held off (same reason as exchange_processor). Listen to
	// a sidechain channel, or to none with nullptr.
	void set_sidechain(std::shared_ptr<SidechainChannel> p_channel) {
		sidechain.channel = std::move(p_channel);
		sidechain.last_seen = 0;
	}

	// MAIN THREAD. A change due at p_due_us (host_time_us(); 0 = as soon as possible).
	// Returns false if the queue is full and the change was dropped.
	bool queue_param(uint32_t p_param_id, double p_value, int64_t p_due_us = 0);

	// MAIN THREAD, before audio runs. Needed to turn timestamps into sample positions.
	void set_sample_rate(double p_rate) { clock.set_sample_rate(p_rate); }

	// AUDIO THREAD. Both buffers are interleaved stereo (Godot's AudioFrame: {float left, right})
	// and may not overlap.
	void process(const float *p_src, float *p_dst, uint32_t p_frames) { process_at(p_src, p_dst, p_frames, host_time_us()); }
	// The same, with the clock reading passed in: lets tests replay exact timings.
	void process_at(const float *p_src, float *p_dst, uint32_t p_frames, int64_t p_now_us);

	// ANY THREAD. Total frames seen by process(), with or without a processor. A cheap way to
	// answer "is this bus running at all?" when debugging.
	uint64_t get_frames_processed() const { return frames_processed.load(std::memory_order_relaxed); }

private:
	void gather_events(uint32_t p_frames);
	void process_chunk(const float *p_src, float *p_dst, uint32_t p_frames, uint32_t p_offset);

	std::atomic<uint64_t> frames_processed{ 0 };
	std::unique_ptr<PluginProcessor> processor;
	RtQueue<ParamEvent> param_queue;
	BlockClock clock;
	// This Godot block's events, sorted by frame_offset (relative to the block); sized once.
	std::vector<ParamEvent> events;
	uint32_t event_count = 0;
	uint32_t event_next = 0; // The first event not yet handed to a chunk.
	std::vector<ParamEvent> chunk_events; // One chunk's slice, offsets relative to the chunk.
	std::vector<ParamEvent> pending; // Due after this block; capacity MAX_PENDING_EVENTS.
	uint32_t clock_restarts_seen = 0; // BlockClock::get_restarts() when `pending` was last placed

	// Planar buffers: one allocation per direction, holding both channels back to back,
	// [L0..L511][R0..R511]. in_ptrs[c] points at the start of channel c, which is the shape
	// AudioBlock::inputs wants.
	std::vector<float> in_storage;
	std::vector<float> out_storage;
	float *in_ptrs[CHANNELS] = {};
	float *out_ptrs[CHANNELS] = {};

	// The sidechain, read once per Godot block (not per chunk) into planar storage big enough for
	// any block a channel can hold. Each chunk then gets pointers into its own slice of it.
	SidechainReader sidechain;
	std::vector<float> aux_storage;
	float *aux_base[CHANNELS] = {};
	const float *aux_ptrs[CHANNELS] = {};
};

} // namespace gaph
