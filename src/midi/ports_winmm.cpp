// MIDI ports on Windows: WinMM (midiIn/midiOut), on every Windows version. A port's id is its
// device name ("Name", or "Name #2" for a second device of that name), stable across replugging,
// unlike WinMM's device numbers. Input: short messages with their arrival time; SysEx input is
// not read (output sends it).
#if defined(_WIN32)

#include "midi_ports.h"

#include "core/block_clock.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <map>

namespace gaph::midi {

namespace {

std::string narrow(const wchar_t *p_w) {
	const int n = WideCharToMultiByte(CP_UTF8, 0, p_w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
	if (n > 1) {
		WideCharToMultiByte(CP_UTF8, 0, p_w, -1, &s[0], n, nullptr, nullptr);
	}
	return s;
}

// Every device's id, in WinMM's order: names made unique with " #2", " #3".
std::vector<std::string> device_ids(bool p_inputs) {
	std::vector<std::string> out;
	std::map<std::string, int> seen;
	const UINT n = p_inputs ? midiInGetNumDevs() : midiOutGetNumDevs();
	for (UINT i = 0; i < n; i++) {
		std::string name;
		if (p_inputs) {
			MIDIINCAPSW caps;
			name = midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR ? narrow(caps.szPname) : "";
		} else {
			MIDIOUTCAPSW caps;
			name = midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR ? narrow(caps.szPname) : "";
		}
		const int k = ++seen[name];
		out.push_back(k == 1 ? name : name + " #" + std::to_string(k));
	}
	return out;
}

bool find_device(const std::string &p_id, bool p_inputs, UINT &r_index) {
	const std::vector<std::string> ids = device_ids(p_inputs);
	for (size_t i = 0; i < ids.size(); i++) {
		if (ids[i] == p_id) {
			r_index = UINT(i);
			return true;
		}
	}
	return false;
}

class WinOutput : public OutputPort {
public:
	HMIDIOUT h = nullptr;
	~WinOutput() override {
		if (h) {
			midiOutReset(h);
			midiOutClose(h);
		}
	}
	bool send(const uint8_t *p_bytes, size_t p_size) override {
		if (p_size == 0) {
			return false;
		}
		if (p_bytes[0] != 0xF0 && p_size <= 3) {
			DWORD msg = 0;
			for (size_t i = 0; i < p_size; i++) {
				msg |= DWORD(p_bytes[i]) << (8 * i);
			}
			return midiOutShortMsg(h, msg) == MMSYSERR_NOERROR;
		}
		// SysEx: a long message, waited for (this is the sender's own thread).
		std::vector<char> buf(p_bytes, p_bytes + p_size);
		MIDIHDR hdr = {};
		hdr.lpData = buf.data();
		hdr.dwBufferLength = DWORD(buf.size());
		if (midiOutPrepareHeader(h, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) {
			return false;
		}
		const bool ok = midiOutLongMsg(h, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR;
		for (int i = 0; ok && !(hdr.dwFlags & MHDR_DONE) && i < 1000; i++) {
			Sleep(1);
		}
		midiOutUnprepareHeader(h, &hdr, sizeof(hdr));
		return ok;
	}
};

class WinInput : public InputPort {
public:
	HMIDIIN h = nullptr;
	Callback callback;
	~WinInput() override {
		if (h) {
			midiInStop(h);
			midiInReset(h);
			midiInClose(h); // no callbacks after this returns
		}
	}
	// WinMM's own thread. Short messages only: the status byte says how many data bytes follow.
	static void CALLBACK proc(HMIDIIN, UINT p_msg, DWORD_PTR p_self, DWORD_PTR p_data, DWORD_PTR) {
		if (p_msg != MIM_DATA) {
			return;
		}
		const int64_t now = gaph::host_time_us();
		const uint8_t b[3] = { uint8_t(p_data), uint8_t(p_data >> 8), uint8_t(p_data >> 16) };
		const size_t n = b[0] >= 0xF8 ? 1 : size_t(1 + MessageSplitter::data_bytes(b[0]));
		reinterpret_cast<WinInput *>(p_self)->callback(b, n, now);
	}
};

} // namespace

std::vector<PortInfo> list_ports(bool p_inputs, std::string &r_error) {
	r_error.clear();
	std::vector<PortInfo> out;
	for (const std::string &id : device_ids(p_inputs)) {
		out.push_back(PortInfo{ id, id });
	}
	return out;
}

const char *ports_backend_name() {
	return "WinMM";
}

std::unique_ptr<OutputPort> open_output(const std::string &p_id, std::string &r_error) {
	UINT index = 0;
	if (!find_device(p_id, false, index)) {
		r_error = "no MIDI output named '" + p_id + "'";
		return nullptr;
	}
	auto out = std::make_unique<WinOutput>();
	if (midiOutOpen(&out->h, index, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
		out->h = nullptr;
		r_error = "cannot open MIDI output '" + p_id + "' (in use by another program?)";
		return nullptr;
	}
	return out;
}

std::unique_ptr<InputPort> open_input(const std::string &p_id, InputPort::Callback p_callback, std::string &r_error) {
	UINT index = 0;
	if (!find_device(p_id, true, index)) {
		r_error = "no MIDI input named '" + p_id + "'";
		return nullptr;
	}
	auto in = std::make_unique<WinInput>();
	in->callback = std::move(p_callback);
	if (midiInOpen(&in->h, index, DWORD_PTR(&WinInput::proc), DWORD_PTR(in.get()), CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
		in->h = nullptr;
		r_error = "cannot open MIDI input '" + p_id + "' (in use by another program?)";
		return nullptr;
	}
	midiInStart(in->h);
	return in;
}

} // namespace gaph::midi

#endif
