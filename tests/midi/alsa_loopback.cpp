// The ALSA backend outside Godot: our event layout against ALSA's real headers, then a loopback
// through the kernel's Midi Through port (output -> Through -> input). Build and run:
// tests/midi/run.sh. Skips the loopback where there is no Midi Through.
#include "midi/midi_ports.h"

#include <alsa/asoundlib.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
			failures++; \
		} \
	} while (0)

int main() {
	CHECK(sizeof(snd_seq_event_t) == 28);
	CHECK(offsetof(snd_seq_event_t, queue) == 3 && offsetof(snd_seq_event_t, source) == 12 && offsetof(snd_seq_event_t, dest) == 14 && offsetof(snd_seq_event_t, data) == 16);
	CHECK(SND_SEQ_PORT_CAP_READ == 1 && SND_SEQ_PORT_CAP_SUBS_READ == 32 && SND_SEQ_PORT_CAP_WRITE == 2 && SND_SEQ_PORT_CAP_SUBS_WRITE == 64 && SND_SEQ_PORT_CAP_NO_EXPORT == 128);
	CHECK(SND_SEQ_QUEUE_DIRECT == 253 && SND_SEQ_ADDRESS_SUBSCRIBERS == 254 && SND_SEQ_ADDRESS_UNKNOWN == 253 && SND_SEQ_EVENT_NONE == 255);
	std::string err;
	std::string through;
	for (const auto &p : gaph::midi::list_ports(true, err)) {
		std::printf("      input  %s  %s\n", p.id.c_str(), p.name.c_str());
		if (p.name.find("Midi Through") != std::string::npos && through.empty()) {
			through = p.id;
		}
	}
	for (const auto &p : gaph::midi::list_ports(false, err)) {
		std::printf("      output %s  %s\n", p.id.c_str(), p.name.c_str());
	}
	if (through.empty()) {
		std::printf("SKIP  no Midi Through port (%s)\n", err.c_str());
		return failures ? 1 : 0;
	}
	std::mutex m;
	std::vector<std::vector<uint8_t>> got;
	auto in = gaph::midi::open_input(through, [&](const uint8_t *b, size_t n, int64_t) {
		std::lock_guard<std::mutex> l(m);
		got.emplace_back(b, b + n);
	}, err);
	auto out = gaph::midi::open_output(through, err);
	CHECK(in && out);
	if (in && out) {
		const std::vector<std::vector<uint8_t>> sent = { { 0x90, 60, 100 }, { 0xB3, 7, 64 }, { 0xE0, 0x00, 0x40 } };
		for (const auto &s : sent) {
			CHECK(out->send(s.data(), s.size()));
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		std::lock_guard<std::mutex> l(m);
		CHECK(got == sent);
		std::printf("      %zu of 3 messages came back through %s\n", got.size(), through.c_str());
	}
	std::printf(failures ? "%d ALSA FAILURES\n" : "ALSA OK\n", failures);
	return failures ? 1 : 0;
}
