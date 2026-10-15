#pragma once

// Vst3SidechainSend: put this on the bus whose sound should drive a plugin's sidechain. It
// passes its bus through unchanged and copies each block into the named channel, which any
// Vst3PluginEffect with a matching `sidechain` name then hears on its aux input.
// (How the timing and stale data work: core/sidechain.h.)
//
// Why not one send class for both formats: each extension is its own library with its own
// channel registry, and a class name may only be registered once. Mixing formats on one source
// bus means one Vst3SidechainSend and one Vst3SidechainSend.

#include "core/sidechain.h"

#include <godot_cpp/classes/audio_effect.hpp>
#include <godot_cpp/classes/audio_effect_instance.hpp>

#include <memory>

namespace godot {

class Vst3SidechainSend;

class Vst3SidechainSendInstance : public AudioEffectInstance {
	GDCLASS(Vst3SidechainSendInstance, AudioEffectInstance);

public:
	Ref<Vst3SidechainSend> send; // Read every block, so a renamed channel applies at once.
	bool writer = false;

	void _process(const void *p_src_buffer, AudioFrame *p_dst_buffer, int32_t p_frame_count) override;
	// False: on a silent bus Godot stops calling us, the channel goes quiet, and readers notice.
	bool _process_silence() const override { return false; }

protected:
	static void _bind_methods() {}
};

class Vst3SidechainSend : public AudioEffect {
	GDCLASS(Vst3SidechainSend, AudioEffect);

public:
	Ref<AudioEffectInstance> _instantiate() override;

	void set_channel(const String &p_channel);
	String get_channel() const;

	// AUDIO THREAD (during a mix, which runs with the audio lock held). Only ever swapped by
	// set_channel() under that same lock, so a plain pointer read is safe. The channel itself
	// is never freed (see sidechain_channel()), so the pointer cannot dangle.
	gaph::SidechainChannel *get_channel_ptr() const { return channel.get(); }

protected:
	static void _bind_methods();

private:
	String channel_name;
	std::shared_ptr<gaph::SidechainChannel> channel;
	uint64_t instances_made = 0;
};

} // namespace godot
