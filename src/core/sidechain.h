#pragma once

// Sidechain: feeding one bus's audio into a plugin on another bus.
//
// THE PROBLEM
// A Godot bus effect only ever sees its own bus. There is no way to ask "what is on the Music
// bus right now?" from an effect on the Voice bus. So a sidechain needs two halves:
//   a SEND effect on the source bus, which copies each block into a named SidechainChannel;
//   the plugin effect, which reads that channel into the plugin's aux input (a vocoder's "Source", say).
//
// TIMING, from how AudioServer mixes (servers/audio_server.cpp): buses are processed one after
// another on the audio thread, from the highest index down to Master (index 0).
//   source bus index > plugin bus index: the send runs first, the plugin hears this very block.
//   source bus index < plugin bus index: the plugin runs first and hears the previous block,
//                                        512 frames (~11 ms at 44.1 kHz) late.
// Both work. For a ducking compressor or a vocoder the 11 ms rarely matters.
//
// STALE DATA
// When a bus goes quiet, Godot stops calling its effects. The send then stops writing, and a
// reader that kept re-reading the last block would loop it forever: a buzz. So every write bumps
// a counter, and a reader that sees no new write since its last read feeds silence instead.
//
// THREADING
// Sends and readers all run on Godot's one mixing thread, so a read and a write never overlap.
// The counter is still atomic, with release/acquire (see rt_queue.h), so that the buffer
// contents are guaranteed visible should Godot ever spread buses across threads.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace gaph {

struct SidechainChannel {
	static constexpr uint32_t MAX_FRAMES = 4096;

	float left[MAX_FRAMES] = {};
	float right[MAX_FRAMES] = {};
	uint32_t frames = 0;
	std::atomic<uint64_t> writes{ 0 };

	// AUDIO THREAD (the send). p_src is Godot's interleaved stereo.
	void write(const float *p_src, uint32_t p_frames) {
		const uint32_t n = p_frames < MAX_FRAMES ? p_frames : MAX_FRAMES;
		for (uint32_t i = 0; i < n; i++) {
			left[i] = p_src[i * 2];
			right[i] = p_src[i * 2 + 1];
		}
		frames = n;
		writes.fetch_add(1, std::memory_order_release); // Publishes the samples written above.
	}
};

// Where a reader keeps its place. One per reading runtime.
struct SidechainReader {
	std::shared_ptr<SidechainChannel> channel;
	uint64_t last_seen = 0;

	// AUDIO THREAD. Fills p_frames planar samples into p_l and p_r: the newest block if there
	// is one, otherwise silence. Returns true if real audio was copied.
	bool read(float *p_l, float *p_r, uint32_t p_frames) {
		const size_t bytes = sizeof(float) * p_frames;
		SidechainChannel *ch = channel.get();
		const uint64_t w = ch ? ch->writes.load(std::memory_order_acquire) : 0;
		if (!ch || w == last_seen || ch->frames < p_frames) {
			std::memset(p_l, 0, bytes);
			std::memset(p_r, 0, bytes);
			return false;
		}
		last_seen = w;
		std::memcpy(p_l, ch->left, bytes);
		std::memcpy(p_r, ch->right, bytes);
		return true;
	}
};

// Name -> channel. MAIN THREAD only. Channels are never removed: one is ~32 KB and there are
// only ever a handful, and never deleting means a pointer handed out stays valid forever.
//
// One registry per *library*: the CLAP and VST3 extensions are separate .so files, each with its
// own copy of this static. That is why each extension has its own send class.
inline std::shared_ptr<SidechainChannel> sidechain_channel(const std::string &p_name) {
	static std::mutex mutex;
	static std::map<std::string, std::shared_ptr<SidechainChannel>> channels;
	if (p_name.empty()) {
		return nullptr;
	}
	std::lock_guard<std::mutex> guard(mutex);
	std::shared_ptr<SidechainChannel> &ch = channels[p_name];
	if (!ch) {
		ch = std::make_shared<SidechainChannel>();
	}
	return ch;
}

} // namespace gaph
