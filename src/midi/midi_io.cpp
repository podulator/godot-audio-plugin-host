#include "midi_io.h"

#include "core/block_clock.h"
#include "midi_driver.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>

using namespace godot;
using gaph::midi::Kind;

namespace {

TypedArray<Dictionary> port_list(bool p_inputs) {
	std::string err;
	TypedArray<Dictionary> out;
	for (const gaph::midi::PortInfo &p : gaph::midi::list_ports(p_inputs, err)) {
		Dictionary d;
		d["id"] = String::utf8(p.id.c_str());
		d["name"] = String::utf8(p.name.c_str());
		out.push_back(d);
	}
	return out;
}

} // namespace

// --- MidiOutput: opening ------------------------------------------------------------------------

MidiOutput::MidiOutput() {}

MidiOutput::~MidiOutput() {
	close();
}

TypedArray<Dictionary> MidiOutput::get_ports() {
	return port_list(false);
}

Ref<MidiOutput> MidiOutput::open(const String &p_port_id) {
	std::string err;
	std::unique_ptr<gaph::midi::OutputPort> p = gaph::midi::open_output(p_port_id.utf8().get_data(), err);
	ERR_FAIL_COND_V_MSG(!p, Ref<MidiOutput>(), vformat("MidiOutput: %s (%s).", String::utf8(err.c_str()), gaph::midi::ports_backend_name()));
	Ref<MidiOutput> out;
	out.instantiate();
	out->port = std::move(p);
	out->port_id = p_port_id;
	out->sender = std::thread([o = out.ptr()] { o->run(); });
	return out;
}

void MidiOutput::close() {
	{
		std::lock_guard<std::mutex> lock(mutex);
		quit = true;
	}
	wake.notify_all();
	if (sender.joinable()) {
		sender.join();
	}
	port.reset();
	pending.clear();
}

void MidiOutput::set_delay_ms(double p_ms) {
	delay_ms = p_ms < 0 ? -1.0 : p_ms;
}

int64_t MidiOutput::delay_us() const {
	if (delay_ms >= 0) {
		return int64_t(delay_ms * 1000.0);
	}
	return int64_t(AudioServer::get_singleton()->get_output_latency() * 1e6);
}

// --- MidiOutput: events -------------------------------------------------------------------------

void MidiOutput::queue(int64_t p_due_us, uint64_t p_source, std::vector<uint8_t> p_bytes) {
	if (!port) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		Pending p{ p_due_us, next_seq++, p_source, std::move(p_bytes) };
		pending.insert(std::upper_bound(pending.begin(), pending.end(), p), std::move(p));
	}
	wake.notify_all();
}

void MidiOutput::send_bytes(const PackedByteArray &p_bytes) {
	ERR_FAIL_COND_MSG(p_bytes.is_empty() || !(p_bytes[0] & 0x80), "MidiOutput: send_bytes() takes a whole MIDI message, starting with its status byte.");
	queue(gaph::host_time_us(), 0, std::vector<uint8_t>(p_bytes.ptr(), p_bytes.ptr() + p_bytes.size()));
}

// MAIN THREAD (the driver). Values at MIDI 2.0 resolution, scaled down to MIDI 1.0 bytes.
void MidiOutput::midi_event(int64_t p_time_us, int64_t p_source, int p_kind, int p_channel, int p_index, int64_t p_value, int64_t p_note_id) {
	const Kind kind = Kind(p_kind);
	if (p_kind < 0 || p_kind > int(Kind::CHOKE) || p_channel < 1 || p_channel > 16) {
		return;
	}
	uint8_t b[3];
	gaph::midi::encode_midi1(kind, uint8_t(p_channel), uint16_t(p_index), uint32_t(p_value), b);
	const size_t n = (kind == Kind::PROGRAM || kind == Kind::CHANNEL_PRESSURE) ? 2 : 3;
	if (kind == Kind::NOTE_ON) {
		book.note_on(uint64_t(p_source), p_note_id, uint8_t(p_channel), uint16_t(p_index), p_time_us);
	} else if (kind == Kind::NOTE_OFF) {
		book.note_off(uint64_t(p_source), p_note_id, uint8_t(p_channel), uint16_t(p_index), p_time_us);
	}
	queue(p_time_us + delay_us(), uint64_t(p_source), std::vector<uint8_t>(b, b + n));
}

// MAIN THREAD. From p_time_us on, this source's messages not sent yet are dropped; RELEASE and CUT
// end its notes sounding then (CUT also sends All Sound Off on their channels).
void MidiOutput::midi_stop(int64_t p_time_us, int64_t p_source, int p_mode) {
	const int64_t due = p_time_us + delay_us();
	{
		std::lock_guard<std::mutex> lock(mutex);
		pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const Pending &p) { return p.source == uint64_t(p_source) && p.due_us >= due; }), pending.end());
	}
	book.prune(gaph::host_time_us());
	if (p_mode == int(gaph::midi::StopMode::WITHDRAW)) {
		book.withdraw(uint64_t(p_source), p_time_us);
		return;
	}
	bool channels[16] = {};
	for (const gaph::midi::NoteBook::Sounding &s : book.stop(uint64_t(p_source), p_time_us)) {
		uint8_t b[3];
		gaph::midi::encode_midi1(Kind::NOTE_OFF, s.channel, s.key, 0x8000, b);
		queue(due, uint64_t(p_source), std::vector<uint8_t>(b, b + 3));
		channels[(s.channel - 1) & 15] = true;
	}
	if (p_mode == int(gaph::midi::StopMode::CUT)) {
		for (int c = 0; c < 16; c++) {
			if (channels[c]) {
				queue(due, uint64_t(p_source), { uint8_t(0xB0 | c), 120, 0 });
			}
		}
	}
}

// The sender: sleeps until the next message is due, then sends it.
void MidiOutput::run() {
	std::unique_lock<std::mutex> lock(mutex);
	while (!quit) {
		if (pending.empty()) {
			wake.wait(lock);
			continue;
		}
		const int64_t due = pending.front().due_us;
		const int64_t now = gaph::host_time_us();
		if (due > now) {
			wake.wait_until(lock, std::chrono::steady_clock::time_point(std::chrono::microseconds(due)));
			continue;
		}
		Pending p = std::move(pending.front());
		pending.erase(pending.begin());
		lock.unlock();
		port->send(p.bytes.data(), p.bytes.size());
		lock.lock();
	}
}

// --- MidiInput ------------------------------------------------------------------------------------

MidiInput::MidiInput() {
	set_process_mode(PROCESS_MODE_ALWAYS); // live input keeps working in a paused game
}

MidiInput::~MidiInput() {
	close();
}

TypedArray<Dictionary> MidiInput::get_ports() {
	return port_list(true);
}

bool MidiInput::open(const String &p_port_id) {
	close();
	std::string err;
	port = gaph::midi::open_input(p_port_id.utf8().get_data(), [this](const uint8_t *p_b, size_t p_n, int64_t p_t) { received_on_backend(p_b, p_n, p_t); }, err);
	open_error = port ? String() : String::utf8(err.c_str());
	ERR_FAIL_COND_V_MSG(!port, false, vformat("MidiInput: %s (%s).", open_error, gaph::midi::ports_backend_name()));
	port_id = p_port_id;
	return true;
}

void MidiInput::close() {
	port.reset(); // the backend stops calling back before this returns
	port_id = String();
	std::lock_guard<std::mutex> lock(mutex);
	inbox.clear();
}

// The backend's thread: queue it, nothing more.
void MidiInput::received_on_backend(const uint8_t *p_bytes, size_t p_size, int64_t p_time_us) {
	std::lock_guard<std::mutex> lock(mutex);
	inbox.push_back(Message{ std::vector<uint8_t>(p_bytes, p_bytes + p_size), p_time_us });
}

void MidiInput::inject_bytes(const PackedByteArray &p_bytes) {
	ERR_FAIL_COND_MSG(p_bytes.is_empty(), "MidiInput: inject_bytes() takes a MIDI message.");
	received_on_backend(p_bytes.ptr(), size_t(p_bytes.size()), gaph::host_time_us());
}

void MidiInput::set_driver(MidiDriver *p_driver) {
	driver_id = p_driver ? p_driver->get_instance_id() : ObjectID();
	live.clear();
}

MidiDriver *MidiInput::get_driver() const {
	return Object::cast_to<MidiDriver>(ObjectDB::get_instance(driver_id));
}

void MidiInput::set_latency_ms(double p_ms) {
	latency_ms = p_ms > 0 ? p_ms : 0.0;
}

// MAIN THREAD, every frame: what arrived goes out. Channel messages also reach the driver.
void MidiInput::deliver() {
	std::vector<Message> batch;
	{
		std::lock_guard<std::mutex> lock(mutex);
		batch.swap(inbox);
	}
	MidiDriver *driver = get_driver();
	const int64_t latency = int64_t(latency_ms * 1000.0);
	for (const Message &m : batch) {
		PackedByteArray raw;
		raw.resize(int64_t(m.bytes.size()));
		std::copy(m.bytes.begin(), m.bytes.end(), raw.ptrw());
		emit_signal("raw_received", raw, m.time_us);
		const uint8_t status = m.bytes[0];
		if (status < 0x80 || status >= 0xF0) {
			continue; // system messages: raw_received only
		}
		const int channel = (status & 0x0F) + 1;
		const uint8_t d1 = m.bytes.size() > 1 ? (m.bytes[1] & 0x7F) : 0, d2 = m.bytes.size() > 2 ? (m.bytes[2] & 0x7F) : 0;
		Kind kind = Kind::CONTROL;
		int index = d1;
		int64_t value = d2; // MIDI 1.0 values, for the signal
		switch (status & 0xF0) {
			case 0x80: kind = Kind::NOTE_OFF; break;
			case 0x90: kind = d2 ? Kind::NOTE_ON : Kind::NOTE_OFF; break;
			case 0xA0: kind = Kind::POLY_PRESSURE; break;
			case 0xB0: kind = Kind::CONTROL; break;
			case 0xC0: kind = Kind::PROGRAM, index = 0, value = d1; break;
			case 0xD0: kind = Kind::CHANNEL_PRESSURE, index = 0, value = d1; break;
			case 0xE0: kind = Kind::PITCH_BEND, index = 0, value = int64_t(d1 | (d2 << 7)) - 8192; break;
		}
		emit_signal("received", int(kind), channel, index, value, m.time_us);
		if (!driver) {
			continue;
		}
		const int64_t due = latency > 0 ? m.time_us + latency : 0; // 0: as soon as possible
		const int key = channel * 128 + index;
		if (kind == Kind::NOTE_ON) {
			live[key].push_back(driver->live_note_on(channel, index, gaph::midi::scale_up(d2, 7, 16), due));
		} else if (kind == Kind::NOTE_OFF) {
			auto it = live.find(key);
			if (it != live.end() && !it->second.empty()) {
				driver->live_note_off(it->second.front(), due);
				it->second.erase(it->second.begin());
			}
		} else {
			bool ok = false;
			const uint32_t inside = kind == Kind::PROGRAM ? uint32_t(value) : gaph::midi::to_inside(kind, value, 1, ok);
			driver->live_post(kind, channel, index, inside, due);
		}
	}
}

void MidiInput::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		set_process_internal(true);
	} else if (p_what == NOTIFICATION_INTERNAL_PROCESS) {
		deliver();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		if (MidiDriver *d = get_driver()) { // no frames to hear the note-offs on: end them now
			for (auto &entry : live) {
				for (int64_t id : entry.second) {
					d->live_note_off(id, 0);
				}
			}
		}
		live.clear();
	}
}

void MidiInput::_bind_methods() {
	ClassDB::bind_static_method("MidiInput", D_METHOD("get_ports"), &MidiInput::get_ports);
	ClassDB::bind_method(D_METHOD("open", "port_id"), &MidiInput::open);
	ClassDB::bind_method(D_METHOD("close"), &MidiInput::close);
	ClassDB::bind_method(D_METHOD("is_open"), &MidiInput::is_open);
	ClassDB::bind_method(D_METHOD("get_port_id"), &MidiInput::get_port_id);
	ClassDB::bind_method(D_METHOD("get_open_error"), &MidiInput::get_open_error);
	ClassDB::bind_method(D_METHOD("set_driver", "driver"), &MidiInput::set_driver);
	ClassDB::bind_method(D_METHOD("get_driver"), &MidiInput::get_driver);
	ClassDB::bind_method(D_METHOD("set_latency_ms", "ms"), &MidiInput::set_latency_ms);
	ClassDB::bind_method(D_METHOD("get_latency_ms"), &MidiInput::get_latency_ms);
	ClassDB::bind_method(D_METHOD("inject_bytes", "bytes"), &MidiInput::inject_bytes);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "latency_ms", PROPERTY_HINT_RANGE, "0,200,0.5,suffix:ms"), "set_latency_ms", "get_latency_ms");
	ADD_SIGNAL(MethodInfo("received", PropertyInfo(Variant::INT, "kind"), PropertyInfo(Variant::INT, "channel"), PropertyInfo(Variant::INT, "index"), PropertyInfo(Variant::INT, "value"), PropertyInfo(Variant::INT, "time_us")));
	ADD_SIGNAL(MethodInfo("raw_received", PropertyInfo(Variant::PACKED_BYTE_ARRAY, "bytes"), PropertyInfo(Variant::INT, "time_us")));
}

void MidiOutput::_bind_methods() {
	ClassDB::bind_static_method("MidiOutput", D_METHOD("get_ports"), &MidiOutput::get_ports);
	ClassDB::bind_static_method("MidiOutput", D_METHOD("open", "port_id"), &MidiOutput::open);
	ClassDB::bind_method(D_METHOD("is_open"), &MidiOutput::is_open);
	ClassDB::bind_method(D_METHOD("get_port_id"), &MidiOutput::get_port_id);
	ClassDB::bind_method(D_METHOD("close"), &MidiOutput::close);
	ClassDB::bind_method(D_METHOD("set_delay_ms", "ms"), &MidiOutput::set_delay_ms);
	ClassDB::bind_method(D_METHOD("get_delay_ms"), &MidiOutput::get_delay_ms);
	ClassDB::bind_method(D_METHOD("send_bytes", "bytes"), &MidiOutput::send_bytes);
	ClassDB::bind_method(D_METHOD("midi_event", "time_us", "source", "kind", "channel", "index", "value", "note_id"), &MidiOutput::midi_event);
	ClassDB::bind_method(D_METHOD("midi_stop", "time_us", "source", "mode"), &MidiOutput::midi_stop);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "delay_ms", PROPERTY_HINT_RANGE, "-1,500,0.5,suffix:ms"), "set_delay_ms", "get_delay_ms");
}
