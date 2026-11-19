// MIDI ports on macOS: CoreMIDI. A port's id is its unique id (kMIDIPropertyUniqueID), which
// survives replugging and renaming; its name is the display name. Input arrives as packets on
// CoreMIDI's own thread, split into messages (MessageSplitter); output goes with MIDISend.
#if defined(__APPLE__)

#include "midi_ports.h"

#include "core/block_clock.h"

#include <CoreMIDI/CoreMIDI.h>

#include <cstdlib>
#include <mutex>

namespace gaph::midi {

namespace {

std::string cf_to_std(CFStringRef p_s) {
	if (!p_s) {
		return "";
	}
	char buf[512];
	return CFStringGetCString(p_s, buf, sizeof(buf), kCFStringEncodingUTF8) ? std::string(buf) : std::string();
}

PortInfo describe(MIDIEndpointRef p_e) {
	SInt32 uid = 0;
	MIDIObjectGetIntegerProperty(p_e, kMIDIPropertyUniqueID, &uid);
	CFStringRef name = nullptr;
	MIDIObjectGetStringProperty(p_e, kMIDIPropertyDisplayName, &name);
	PortInfo p{ std::to_string(uid), cf_to_std(name) };
	if (name) {
		CFRelease(name);
	}
	return p;
}

// One CoreMIDI client for the whole process, made on first use.
MIDIClientRef client(std::string &r_error) {
	static MIDIClientRef c = 0;
	static OSStatus status = noErr;
	static std::once_flag once;
	std::call_once(once, [] { status = MIDIClientCreate(CFSTR("Godot"), nullptr, nullptr, &c); });
	if (status != noErr) {
		r_error = "cannot start CoreMIDI (" + std::to_string(int(status)) + ")";
	}
	return status == noErr ? c : 0;
}

MIDIEndpointRef find(const std::string &p_id, bool p_inputs) {
	char *end = nullptr;
	const long uid = std::strtol(p_id.c_str(), &end, 10);
	if (p_id.empty() || *end) {
		return 0;
	}
	const ItemCount n = p_inputs ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
	for (ItemCount i = 0; i < n; i++) {
		const MIDIEndpointRef e = p_inputs ? MIDIGetSource(i) : MIDIGetDestination(i);
		SInt32 u = 0;
		if (MIDIObjectGetIntegerProperty(e, kMIDIPropertyUniqueID, &u) == noErr && long(u) == uid) {
			return e;
		}
	}
	return 0;
}

class MacOutput : public OutputPort {
public:
	MIDIPortRef port = 0;
	MIDIEndpointRef dest = 0;
	~MacOutput() override {
		if (port) {
			MIDIPortDispose(port);
		}
	}
	bool send(const uint8_t *p_bytes, size_t p_size) override {
		std::vector<Byte> buf(p_size + 256);
		MIDIPacketList *list = reinterpret_cast<MIDIPacketList *>(buf.data());
		MIDIPacket *packet = MIDIPacketListInit(list);
		packet = MIDIPacketListAdd(list, buf.size(), packet, 0, p_size, p_bytes); // time 0: now
		return packet && MIDISend(port, dest, list) == noErr;
	}
};

class MacInput : public InputPort {
public:
	MIDIPortRef port = 0;
	Callback callback;
	MessageSplitter splitter; // CoreMIDI's thread only
	~MacInput() override {
		if (port) {
			MIDIPortDispose(port); // no read callbacks after this returns
		}
	}
	static void read(const MIDIPacketList *p_list, void *p_self, void *) {
		MacInput *self = static_cast<MacInput *>(p_self);
		const int64_t now = gaph::host_time_us();
		const MIDIPacket *packet = &p_list->packet[0];
		for (UInt32 i = 0; i < p_list->numPackets; i++) {
			self->splitter.feed(packet->data, packet->length, [&](const uint8_t *p_b, size_t p_n) { self->callback(p_b, p_n, now); });
			packet = MIDIPacketNext(packet);
		}
	}
};

} // namespace

std::vector<PortInfo> list_ports(bool p_inputs, std::string &r_error) {
	std::vector<PortInfo> out;
	if (!client(r_error)) {
		return out;
	}
	const ItemCount n = p_inputs ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
	for (ItemCount i = 0; i < n; i++) {
		out.push_back(describe(p_inputs ? MIDIGetSource(i) : MIDIGetDestination(i)));
	}
	return out;
}

const char *ports_backend_name() {
	return "CoreMIDI";
}

std::unique_ptr<OutputPort> open_output(const std::string &p_id, std::string &r_error) {
	const MIDIClientRef c = client(r_error);
	const MIDIEndpointRef dest = c ? find(p_id, false) : 0;
	if (!dest) {
		r_error = r_error.empty() ? "no MIDI output with id " + p_id : r_error;
		return nullptr;
	}
	auto out = std::make_unique<MacOutput>();
	out->dest = dest;
	if (MIDIOutputPortCreate(c, CFSTR("out"), &out->port) != noErr) {
		out->port = 0;
		r_error = "cannot open MIDI output " + p_id;
		return nullptr;
	}
	return out;
}

// MIDIInputPortCreate is the classic API (MIDI 1.0 packets); deprecated in favour of the UMP one,
// still supported, and what MIDI 1.0 bytes need.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
std::unique_ptr<InputPort> open_input(const std::string &p_id, InputPort::Callback p_callback, std::string &r_error) {
	const MIDIClientRef c = client(r_error);
	const MIDIEndpointRef src = c ? find(p_id, true) : 0;
	if (!src) {
		r_error = r_error.empty() ? "no MIDI input with id " + p_id : r_error;
		return nullptr;
	}
	auto in = std::make_unique<MacInput>();
	in->callback = std::move(p_callback);
	if (MIDIInputPortCreate(c, CFSTR("in"), &MacInput::read, in.get(), &in->port) != noErr || MIDIPortConnectSource(in->port, src, nullptr) != noErr) {
		r_error = "cannot open MIDI input " + p_id;
		return nullptr;
	}
	return in;
}
#pragma clang diagnostic pop

} // namespace gaph::midi

#endif
