// MIDI ports on Linux: the ALSA sequencer, opened at run time (dlopen libasound.so.2), so a system
// without ALSA has no ports rather than a library that fails to load. Every port, hardware or
// software (Midi Through, PipeWire, other programs), is a sequencer port; we make one client per
// open port, connect it, and move bytes through ALSA's own MIDI encoder and decoder.
//
// The few ALSA types used are written out here (layouts unchanged since ALSA 1.0, 2004), so the
// build needs no ALSA headers; tests/midi/alsa_layout.cpp checks them against the real ones.
#if defined(__linux__) && !defined(__ANDROID__)

#include "midi_ports.h"

#include "core/block_clock.h"

#include <dlfcn.h>
#include <poll.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>

namespace gaph::midi {

namespace alsa {

struct snd_seq_t;
struct snd_midi_event_t;
struct snd_seq_client_info_t;
struct snd_seq_port_info_t;

// snd_seq_event_t (alsa/seq_event.h): 28 bytes.
struct Event {
	uint8_t type, flags, tag, queue;
	uint32_t time[2];
	uint8_t source_client, source_port, dest_client, dest_port;
	uint8_t data[12];
};
static_assert(sizeof(Event) == 28, "snd_seq_event_t is 28 bytes");

enum : int { OPEN_OUTPUT = 1, OPEN_INPUT = 2, NONBLOCK = 1 };
enum : unsigned {
	CAP_READ = 1u << 0, CAP_WRITE = 1u << 1, CAP_SUBS_READ = 1u << 5, CAP_SUBS_WRITE = 1u << 6, CAP_NO_EXPORT = 1u << 7,
	TYPE_MIDI_GENERIC = 1u << 1, TYPE_APPLICATION = 1u << 20,
};
enum : uint8_t { QUEUE_DIRECT = 253, ADDRESS_SUBSCRIBERS = 254, ADDRESS_UNKNOWN = 253 };

struct Api {
	void *lib = nullptr;
	int (*seq_open)(snd_seq_t **, const char *, int, int);
	int (*seq_close)(snd_seq_t *);
	int (*set_client_name)(snd_seq_t *, const char *);
	int (*create_simple_port)(snd_seq_t *, const char *, unsigned, unsigned);
	int (*client_id)(snd_seq_t *);
	int (*connect_to)(snd_seq_t *, int, int, int);
	int (*connect_from)(snd_seq_t *, int, int, int);
	int (*event_output_direct)(snd_seq_t *, Event *);
	int (*event_input)(snd_seq_t *, Event **);
	int (*poll_count)(snd_seq_t *, short);
	int (*poll_descriptors)(snd_seq_t *, struct pollfd *, unsigned, short);
	int (*client_info_malloc)(snd_seq_client_info_t **);
	void (*client_info_free)(snd_seq_client_info_t *);
	void (*client_info_set_client)(snd_seq_client_info_t *, int);
	int (*client_info_get_client)(const snd_seq_client_info_t *);
	const char *(*client_info_get_name)(snd_seq_client_info_t *);
	int (*query_next_client)(snd_seq_t *, snd_seq_client_info_t *);
	int (*port_info_malloc)(snd_seq_port_info_t **);
	void (*port_info_free)(snd_seq_port_info_t *);
	void (*port_info_set_client)(snd_seq_port_info_t *, int);
	void (*port_info_set_port)(snd_seq_port_info_t *, int);
	int (*port_info_get_port)(const snd_seq_port_info_t *);
	unsigned (*port_info_get_capability)(const snd_seq_port_info_t *);
	unsigned (*port_info_get_type)(const snd_seq_port_info_t *);
	const char *(*port_info_get_name)(const snd_seq_port_info_t *);
	int (*query_next_port)(snd_seq_t *, snd_seq_port_info_t *);
	int (*midi_event_new)(size_t, snd_midi_event_t **);
	void (*midi_event_free)(snd_midi_event_t *);
	void (*midi_event_no_status)(snd_midi_event_t *, int);
	long (*midi_event_encode)(snd_midi_event_t *, const unsigned char *, long, Event *);
	long (*midi_event_decode)(snd_midi_event_t *, unsigned char *, long, const Event *);
	void (*midi_event_reset_encode)(snd_midi_event_t *);
};

// Loaded once, on first use. False (and r_error) if ALSA is missing or lacks a function.
const Api *api(std::string &r_error) {
	static Api a;
	static std::string failure;
	static std::once_flag once;
	std::call_once(once, [] {
		a.lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
		if (!a.lib) {
			failure = "ALSA (libasound.so.2) is not installed";
			return;
		}
		bool ok = true;
		auto get = [&](auto &p_fn, const char *p_name) {
			p_fn = reinterpret_cast<std::remove_reference_t<decltype(p_fn)>>(dlsym(a.lib, p_name));
			ok = ok && p_fn != nullptr;
		};
		get(a.seq_open, "snd_seq_open");
		get(a.seq_close, "snd_seq_close");
		get(a.set_client_name, "snd_seq_set_client_name");
		get(a.create_simple_port, "snd_seq_create_simple_port");
		get(a.client_id, "snd_seq_client_id");
		get(a.connect_to, "snd_seq_connect_to");
		get(a.connect_from, "snd_seq_connect_from");
		get(a.event_output_direct, "snd_seq_event_output_direct");
		get(a.event_input, "snd_seq_event_input");
		get(a.poll_count, "snd_seq_poll_descriptors_count");
		get(a.poll_descriptors, "snd_seq_poll_descriptors");
		get(a.client_info_malloc, "snd_seq_client_info_malloc");
		get(a.client_info_free, "snd_seq_client_info_free");
		get(a.client_info_set_client, "snd_seq_client_info_set_client");
		get(a.client_info_get_client, "snd_seq_client_info_get_client");
		get(a.client_info_get_name, "snd_seq_client_info_get_name");
		get(a.query_next_client, "snd_seq_query_next_client");
		get(a.port_info_malloc, "snd_seq_port_info_malloc");
		get(a.port_info_free, "snd_seq_port_info_free");
		get(a.port_info_set_client, "snd_seq_port_info_set_client");
		get(a.port_info_set_port, "snd_seq_port_info_set_port");
		get(a.port_info_get_port, "snd_seq_port_info_get_port");
		get(a.port_info_get_capability, "snd_seq_port_info_get_capability");
		get(a.port_info_get_type, "snd_seq_port_info_get_type");
		get(a.port_info_get_name, "snd_seq_port_info_get_name");
		get(a.query_next_port, "snd_seq_query_next_port");
		get(a.midi_event_new, "snd_midi_event_new");
		get(a.midi_event_free, "snd_midi_event_free");
		get(a.midi_event_no_status, "snd_midi_event_no_status");
		get(a.midi_event_encode, "snd_midi_event_encode");
		get(a.midi_event_decode, "snd_midi_event_decode");
		get(a.midi_event_reset_encode, "snd_midi_event_reset_encode");
		if (!ok) {
			failure = "ALSA (libasound.so.2) lacks a sequencer function";
		}
	});
	r_error = failure;
	return failure.empty() ? &a : nullptr;
}

} // namespace alsa

std::vector<PortInfo> list_ports(bool p_inputs, std::string &r_error) {
	std::vector<PortInfo> out;
	const alsa::Api *a = alsa::api(r_error);
	if (!a) {
		return out;
	}
	alsa::snd_seq_t *seq = nullptr;
	if (a->seq_open(&seq, "default", alsa::OPEN_OUTPUT | alsa::OPEN_INPUT, 0) < 0) {
		r_error = "cannot open the ALSA sequencer (/dev/snd/seq)";
		return out;
	}
	// An input of ours reads from a port others can read from; an output writes to one others can
	// write to.
	const unsigned need = p_inputs ? (alsa::CAP_READ | alsa::CAP_SUBS_READ) : (alsa::CAP_WRITE | alsa::CAP_SUBS_WRITE);
	const int self = a->client_id(seq);
	alsa::snd_seq_client_info_t *ci = nullptr;
	alsa::snd_seq_port_info_t *pi = nullptr;
	a->client_info_malloc(&ci);
	a->port_info_malloc(&pi);
	a->client_info_set_client(ci, -1);
	while (a->query_next_client(seq, ci) >= 0) {
		const int client = a->client_info_get_client(ci);
		if (client == 0 || client == self) {
			continue; // the System client (timer, announcements), and ourselves
		}
		a->port_info_set_client(pi, client);
		a->port_info_set_port(pi, -1);
		while (a->query_next_port(seq, pi) >= 0) {
			const unsigned caps = a->port_info_get_capability(pi);
			if ((caps & need) != need || (caps & alsa::CAP_NO_EXPORT)) {
				continue;
			}
			PortInfo p;
			p.id = std::to_string(client) + ":" + std::to_string(a->port_info_get_port(pi));
			p.name = std::string(a->client_info_get_name(ci)) + ": " + a->port_info_get_name(pi);
			out.push_back(p);
		}
	}
	a->port_info_free(pi);
	a->client_info_free(ci);
	a->seq_close(seq);
	return out;
}

const char *ports_backend_name() {
	return "ALSA";
}

namespace {

bool parse_address(const std::string &p_id, int &r_client, int &r_port) {
	const size_t colon = p_id.find(':');
	if (colon == std::string::npos) {
		return false;
	}
	char *end = nullptr;
	r_client = int(std::strtol(p_id.c_str(), &end, 10));
	if (end != p_id.c_str() + colon) {
		return false;
	}
	r_port = int(std::strtol(p_id.c_str() + colon + 1, &end, 10));
	return *end == 0 && r_client >= 0 && r_port >= 0;
}

// One sequencer client with one port, connected to the port p_id names.
struct Client {
	const alsa::Api *a = nullptr;
	alsa::snd_seq_t *seq = nullptr;
	alsa::snd_midi_event_t *codec = nullptr;
	int port = -1;

	bool open(const std::string &p_id, bool p_input, std::string &r_error) {
		a = alsa::api(r_error);
		int client = 0, target = 0;
		if (!a) {
			return false;
		}
		if (!parse_address(p_id, client, target)) {
			r_error = "not an ALSA port id (client:port): '" + p_id + "'";
			return false;
		}
		if (a->seq_open(&seq, "default", p_input ? alsa::OPEN_INPUT : alsa::OPEN_OUTPUT, p_input ? alsa::NONBLOCK : 0) < 0) {
			r_error = "cannot open the ALSA sequencer (/dev/snd/seq)";
			return false;
		}
		a->set_client_name(seq, "Godot");
		const unsigned caps = p_input ? (alsa::CAP_WRITE | alsa::CAP_SUBS_WRITE) : (alsa::CAP_READ | alsa::CAP_SUBS_READ);
		port = a->create_simple_port(seq, p_input ? "in" : "out", caps, alsa::TYPE_MIDI_GENERIC | alsa::TYPE_APPLICATION);
		const int linked = port < 0 ? -1 : (p_input ? a->connect_from(seq, port, client, target) : a->connect_to(seq, port, client, target));
		if (linked < 0 || a->midi_event_new(4096, &codec) < 0) {
			r_error = "cannot connect to ALSA port " + p_id;
			return false;
		}
		a->midi_event_no_status(codec, 1); // every message with its status byte: no running status
		return true;
	}

	~Client() {
		if (codec) {
			a->midi_event_free(codec);
		}
		if (seq) {
			a->seq_close(seq);
		}
	}
};

class AlsaOutput : public OutputPort {
public:
	Client c;
	bool send(const uint8_t *p_bytes, size_t p_size) override {
		alsa::Event ev;
		std::memset(&ev, 0, sizeof(ev));
		c.a->midi_event_reset_encode(c.codec);
		if (c.a->midi_event_encode(c.codec, p_bytes, long(p_size), &ev) <= 0 || ev.type == 255) { // 255: no event
			return false;
		}
		ev.queue = alsa::QUEUE_DIRECT;
		ev.source_port = uint8_t(c.port);
		ev.dest_client = alsa::ADDRESS_SUBSCRIBERS;
		ev.dest_port = alsa::ADDRESS_UNKNOWN;
		return c.a->event_output_direct(c.seq, &ev) >= 0;
	}
};

class AlsaInput : public InputPort {
public:
	Client c;
	Callback callback;
	std::atomic<bool> running{ true };
	std::thread reader;

	void start() {
		reader = std::thread([this] { run(); });
	}
	~AlsaInput() override {
		running = false;
		if (reader.joinable()) {
			reader.join();
		}
	}

private:
	void run() {
		std::vector<struct pollfd> fds(size_t(std::max(1, c.a->poll_count(c.seq, POLLIN))));
		c.a->poll_descriptors(c.seq, fds.data(), unsigned(fds.size()), POLLIN);
		uint8_t bytes[4096];
		while (running) {
			if (poll(fds.data(), nfds_t(fds.size()), 50) <= 0) { // 50 ms: how soon a close is noticed
				continue;
			}
			alsa::Event *ev = nullptr;
			while (running && c.a->event_input(c.seq, &ev) >= 0 && ev) {
				const int64_t now = gaph::host_time_us();
				const long n = c.a->midi_event_decode(c.codec, bytes, long(sizeof(bytes)), ev);
				if (n > 0) {
					callback(bytes, size_t(n), now);
				}
			}
		}
	}
};

} // namespace

std::unique_ptr<OutputPort> open_output(const std::string &p_id, std::string &r_error) {
	auto out = std::make_unique<AlsaOutput>();
	if (!out->c.open(p_id, false, r_error)) {
		return nullptr;
	}
	return out;
}

std::unique_ptr<InputPort> open_input(const std::string &p_id, InputPort::Callback p_callback, std::string &r_error) {
	auto in = std::make_unique<AlsaInput>();
	if (!in->c.open(p_id, true, r_error)) {
		return nullptr;
	}
	in->callback = std::move(p_callback);
	in->start();
	return in;
}

} // namespace gaph::midi

#endif
