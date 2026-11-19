#pragma once

// MIDI ports: the platform's MIDI inputs and outputs, as bytes. Plain C++, no Godot. One backend
// per platform (ports_alsa.cpp on Linux, ports_winmm.cpp on Windows, ports_coremidi.cpp on macOS,
// ports_none.cpp elsewhere), each compiled only there.
//
// Bytes are MIDI 1.0 messages, one complete message per call (a status byte and its data, or a
// whole SysEx). Every backend speaks MIDI 1.0 on the wire; MidiOutput scales values down, and
// MidiInput scales them up, at the edge.
//
// THREADS
// list_ports and open: MAIN thread. OutputPort::send: one thread at a time (MidiOutput's sender).
// InputPort's callback: the backend's own thread, with the message's arrival time on
// gaph::host_time_us()'s clock; it must not block.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gaph::midi {

struct PortInfo {
	std::string id; // stable while the port exists: what open() takes
	std::string name; // for people
};

class OutputPort {
public:
	virtual ~OutputPort() = default;
	virtual bool send(const uint8_t *p_bytes, size_t p_size) = 0;
};

class InputPort {
public:
	using Callback = std::function<void(const uint8_t *p_bytes, size_t p_size, int64_t p_time_us)>;
	virtual ~InputPort() = default; // stops the callbacks before it returns
};

// What the platform offers. r_error says why there is nothing (no MIDI system, say).
std::vector<PortInfo> list_ports(bool p_inputs, std::string &r_error);
std::unique_ptr<OutputPort> open_output(const std::string &p_id, std::string &r_error);
std::unique_ptr<InputPort> open_input(const std::string &p_id, InputPort::Callback p_callback, std::string &r_error);
// The platform's name for its MIDI system, for messages ("ALSA", "WinMM", "CoreMIDI", "none").
const char *ports_backend_name();

// Splits a stream of MIDI 1.0 bytes into whole messages, with running status: for backends that
// hand over bytes as they come (CoreMIDI packets, a raw stream). Real-time bytes (0xF8 and up)
// are messages of their own, even inside another message.
class MessageSplitter {
public:
	template <typename F>
	void feed(const uint8_t *p_bytes, size_t p_size, F &&p_out) {
		for (size_t i = 0; i < p_size; i++) {
			const uint8_t b = p_bytes[i];
			if (b >= 0xF8) {
				p_out(&b, size_t(1));
				continue;
			}
			if (b == 0xF0) {
				sysex = true;
				buf.assign(1, b);
				continue;
			}
			if (sysex) {
				buf.push_back(b);
				if (b == 0xF7) {
					p_out(buf.data(), buf.size());
					buf.clear();
					sysex = false;
				}
				continue;
			}
			if (b & 0x80) {
				status = b < 0xF0 ? b : 0; // system common messages cancel running status
				buf.assign(1, b);
				need = data_bytes(b);
			} else if (buf.empty() && status) {
				buf.assign(1, status); // running status
				buf.push_back(b);
			} else if (!buf.empty()) {
				buf.push_back(b);
			} else {
				continue; // stray data byte
			}
			if (!buf.empty() && buf.size() == size_t(need) + 1) {
				p_out(buf.data(), buf.size());
				buf.clear();
			}
		}
	}

	static int data_bytes(uint8_t p_status) {
		switch (p_status & 0xF0) {
			case 0xC0:
			case 0xD0:
				return 1;
			case 0xF0:
				return p_status == 0xF1 || p_status == 0xF3 ? 1 : (p_status == 0xF2 ? 2 : 0);
			default:
				return 2;
		}
	}

private:
	std::vector<uint8_t> buf;
	uint8_t status = 0;
	int need = 0;
	bool sysex = false;
};

} // namespace gaph::midi
