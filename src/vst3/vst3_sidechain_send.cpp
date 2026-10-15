#include "vst3_sidechain_send.h"
#include "godot_common/param_properties.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <cstring>

namespace godot {

// AUDIO THREAD. A plain copy (src and dst are different buffers), plus the write.
void Vst3SidechainSendInstance::_process(const void *p_src_buffer, AudioFrame *p_dst_buffer, int32_t p_frame_count) {
	std::memcpy(p_dst_buffer, p_src_buffer, sizeof(AudioFrame) * size_t(p_frame_count));
	gaph::SidechainChannel *ch = send.is_valid() ? send->get_channel_ptr() : nullptr;
	if (writer && ch) {
		ch->write(static_cast<const float *>(p_src_buffer), uint32_t(p_frame_count));
	}
}

// WHICH INSTANCE WRITES
// On a surround bus Godot makes one instance per channel pair (front, then centre/LFE, ...), and
// only the front pair should feed the sidechain: several writers would put several blocks into
// the channel per mix. Godot doesn't tell an instance which pair it is for, but it does create
// them in pair order, for every pair, whenever a bus is (re)built (_update_bus_effects). So the
// n-th instance is for pair n modulo the pair count, and pair 0 is the writer. (A stereo project
// has one pair, so every instance writes, which is right.) Putting one send resource on two buses
// would give two writers; use one send per channel name.
Ref<AudioEffectInstance> Vst3SidechainSend::_instantiate() {
	Ref<Vst3SidechainSendInstance> inst;
	inst.instantiate();
	const uint64_t pairs = uint64_t(AudioServer::get_singleton()->get_speaker_mode()) + 1; // stereo = 0
	inst->writer = (instances_made++ % pairs) == 0;
	inst->send = Ref<Vst3SidechainSend>(this);
	return inst;
}

void Vst3SidechainSend::set_channel(const String &p_channel) {
	channel_name = p_channel;
	std::shared_ptr<gaph::SidechainChannel> next = gaph::sidechain_channel(gaph_godot::to_std(p_channel));
	AudioServer::get_singleton()->lock();
	channel = std::move(next);
	AudioServer::get_singleton()->unlock();
}

String Vst3SidechainSend::get_channel() const {
	return channel_name;
}

void Vst3SidechainSend::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_channel", "channel"), &Vst3SidechainSend::set_channel);
	ClassDB::bind_method(D_METHOD("get_channel"), &Vst3SidechainSend::get_channel);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "channel", PROPERTY_HINT_PLACEHOLDER_TEXT, "e.g. kick"), "set_channel", "get_channel");
}

} // namespace godot
