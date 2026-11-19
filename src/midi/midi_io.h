#pragma once

// MidiOutput and MidiInput: MIDI to and from the platform's ports (hardware, other programs), on
// top of midi_ports.h's backends.
//
// MidiOutput is a MidiDriver target like an instrument: midi.set_channel_target(1, out). Each event
// leaves at its due time, from a sender thread of its own, as MIDI 1.0 bytes (values scaled down).
// Due times are when an instrument would be heard, so by default the output waits as long as the
// audio does to reach the speakers (AudioServer.get_output_latency()), and hardware and game sound
// together; `delay_ms` sets it by hand. midi_stop withdraws what has not left yet and, for RELEASE
// and CUT, sends note-offs (CUT adds All Sound Off on those channels).
//
// MidiInput is a node that reads one port. Each message arrives on the backend's thread with its
// arrival time, waits in a queue, and is handed out on the main thread every frame: as the
// `received` signal (MIDI 1.0 values), and to a MidiDriver if one is set (`driver`), which plays it
// on its channel's target and ends its notes on stop and panic. `latency_ms` keeps the spacing of
// what was played exact: each message is due its arrival time plus that much, instead of "the
// next frame", so a frame's jitter disappears at the cost of that fixed delay.

#include "core/midi_target.h"
#include "core/midi_values.h"
#include "midi_ports.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace godot {

class MidiOutput : public RefCounted {
	GDCLASS(MidiOutput, RefCounted);

public:
	MidiOutput();
	~MidiOutput() override;

	static TypedArray<Dictionary> get_ports();
	static Ref<MidiOutput> open(const String &p_port_id);
	bool is_open() const { return port != nullptr; }
	String get_port_id() const { return port_id; }
	void close();

	void set_delay_ms(double p_ms); // -1: the audio's output latency (the default)
	double get_delay_ms() const { return delay_ms; }
	void send_bytes(const PackedByteArray &p_bytes); // now, as they are

	// The MidiDriver target methods.
	void midi_event(int64_t p_time_us, int64_t p_source, int p_kind, int p_channel, int p_index, int64_t p_value, int64_t p_note_id);
	void midi_stop(int64_t p_time_us, int64_t p_source, int p_mode);

protected:
	static void _bind_methods();

private:
	struct Pending {
		int64_t due_us;
		uint64_t seq;
		uint64_t source;
		std::vector<uint8_t> bytes;
		bool operator<(const Pending &o) const { return due_us != o.due_us ? due_us < o.due_us : seq < o.seq; }
	};
	int64_t delay_us() const;
	void queue(int64_t p_due_us, uint64_t p_source, std::vector<uint8_t> p_bytes);
	void run();

	std::unique_ptr<gaph::midi::OutputPort> port;
	String port_id;
	double delay_ms = -1.0;
	gaph::midi::NoteBook book; // MAIN thread: what each source has sounding, for midi_stop
	std::mutex mutex;
	std::condition_variable wake;
	std::vector<Pending> pending; // sorted by due time
	uint64_t next_seq = 0;
	bool quit = false;
	std::thread sender;
};

class MidiDriver;

class MidiInput : public Node {
	GDCLASS(MidiInput, Node);

public:
	MidiInput();
	~MidiInput() override;

	static TypedArray<Dictionary> get_ports();
	bool open(const String &p_port_id);
	void close();
	bool is_open() const { return port != nullptr; }
	String get_port_id() const { return port_id; }
	String get_open_error() const { return open_error; }

	void set_driver(MidiDriver *p_driver);
	MidiDriver *get_driver() const;
	void set_latency_ms(double p_ms);
	double get_latency_ms() const { return latency_ms; }

	// For tests and scripts: as if this message had arrived now.
	void inject_bytes(const PackedByteArray &p_bytes);

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct Message {
		std::vector<uint8_t> bytes;
		int64_t time_us;
	};
	void received_on_backend(const uint8_t *p_bytes, size_t p_size, int64_t p_time_us);
	void deliver();

	std::unique_ptr<gaph::midi::InputPort> port;
	String port_id, open_error;
	ObjectID driver_id;
	double latency_ms = 0.0;
	std::mutex mutex;
	std::vector<Message> inbox; // filled on the backend's thread, emptied on the main thread
	std::map<int, std::vector<int64_t>> live; // MAIN thread: channel * 128 + key -> the driver's note ids, oldest first
};

} // namespace godot
