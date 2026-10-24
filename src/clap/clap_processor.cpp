#include "clap_processor.h"
#include "core/native_window.h"
#include "core/channel_map.h"

#include <cstdio>
#include <cstring>


// The version from VERSION, which SConstruct passes in as a bare number (1.1.0), made a string
// here. Two macro steps, because # stringifies its argument as written: the inner one would
// otherwise give "GAPH_VERSION_NUMBER". Tests that compile this file on their own get "dev".
#define GAPH_STRINGIFY_(x) #x
#define GAPH_STRINGIFY(x) GAPH_STRINGIFY_(x)
#ifdef GAPH_VERSION_NUMBER
#define GAPH_VERSION GAPH_STRINGIFY(GAPH_VERSION_NUMBER)
#else
#define GAPH_VERSION "dev"
#endif

namespace gaph {

static constexpr uint32_t MAX_IN_EVENTS = 256;

// WHICH THREAD AM I ON?
// CLAP's thread-check extension lets a plugin ask the host "is this the audio thread?"; good
// plugins assert on it to catch host bugs. `thread_local` gives every thread its own copy of
// this variable, so setting it in process() marks the audio thread without affecting any other.
// It is also set on the main thread while it stands in for the audio thread: stopping
// processing during a plugin swap, when the audio lock guarantees the real audio thread is idle.
static thread_local bool t_audio_thread = false;

// Sets t_audio_thread for one scope and restores the previous value on exit (RAII again).
// The anonymous namespace makes the struct private to this file.
namespace {
struct AudioThreadScope {
	bool prev;
	AudioThreadScope() :
			prev(t_audio_thread) { t_audio_thread = true; }
	~AudioThreadScope() { t_audio_thread = prev; }
};
} // namespace

std::unique_ptr<ClapProcessor> ClapProcessor::create(std::shared_ptr<ClapLibrary> p_library, const std::string &p_plugin_id,
		double p_sample_rate, uint32_t p_max_frames, bool p_activate, std::string &r_error) {
	if (!p_library) {
		r_error = "no library";
		return nullptr;
	}
	const clap_plugin_descriptor_t *desc = p_library->find(p_plugin_id);
	if (!desc) {
		r_error = p_plugin_id.empty() ? "binary contains no plugins" : "no plugin with id '" + p_plugin_id + "'";
		return nullptr;
	}

	std::unique_ptr<ClapProcessor> p(new ClapProcessor());
	p->library = std::move(p_library);
	p->main_thread = std::this_thread::get_id();
	p->sample_rate = p_sample_rate;
	p->max_frames = p_max_frames;

	// Fill in our side of the conversation before creating the plugin: a plugin may call host
	// functions from inside create_plugin() or init(). The strings must outlive the plugin;
	// string literals live for the whole program, so they do.
	p->host.clap_version = CLAP_VERSION;
	p->host.host_data = p.get(); // How the static host_* functions find this object again.
	p->host.name = "Godot Audio Plugin Host";
	p->host.vendor = "godot-audio-plugin-host";
	p->host.url = "";
	p->host.version = GAPH_VERSION;
	p->host.get_extension = host_get_extension;
	p->host.request_restart = host_request_restart;
	p->host.request_process = host_request_process;
	p->host.request_callback = host_request_callback;

	const clap_plugin_factory_t *factory = p->library->get_factory();
	p->plugin = factory->create_plugin(factory, &p->host, desc->id);
	if (!p->plugin) {
		r_error = std::string("create_plugin failed for ") + desc->id;
		return nullptr;
	}
	if (!p->plugin->init(p->plugin)) {
		r_error = std::string("init failed for ") + desc->id;
		return nullptr; // The destructor still calls destroy(), as the spec requires.
	}
	p->plugin_initialized = true;
	// Extensions can only be queried after init(). Either may be missing: a plugin without
	// audio-ports has no audio I/O at all (a MIDI tool, say), and process() then refuses to run.
	p->ext_params = static_cast<const clap_plugin_params_t *>(p->plugin->get_extension(p->plugin, CLAP_EXT_PARAMS));
	p->ext_audio_ports = static_cast<const clap_plugin_audio_ports_t *>(p->plugin->get_extension(p->plugin, CLAP_EXT_AUDIO_PORTS));
	p->ext_state = static_cast<const clap_plugin_state_t *>(p->plugin->get_extension(p->plugin, CLAP_EXT_STATE));
	p->ext_gui = static_cast<const clap_plugin_gui_t *>(p->plugin->get_extension(p->plugin, CLAP_EXT_GUI));
	p->ext_timer = static_cast<const clap_plugin_timer_support_t *>(p->plugin->get_extension(p->plugin, CLAP_EXT_TIMER_SUPPORT));
	p->ext_fd = static_cast<const clap_plugin_posix_fd_support_t *>(p->plugin->get_extension(p->plugin, CLAP_EXT_POSIX_FD_SUPPORT));

	// Allocate everything process() will need now, while allocation is still allowed.
	p->in_event_storage.resize(MAX_IN_EVENTS);
	// `ctx` is the event lists' equivalent of host_data: it leads the static callbacks back here.
	p->in_events.ctx = p.get();
	p->in_events.size = in_events_size;
	p->in_events.get = in_events_get;
	p->out_events.ctx = p.get();
	p->out_events.try_push = out_events_try_push;

	p->setup_ports(true, p->inputs, p->input_info, p->main_input);
	p->setup_ports(false, p->outputs, p->output_info, p->main_output);

	if (p_activate && !p->activate(r_error)) {
		return nullptr;
	}
	return p;
}

ClapProcessor::~ClapProcessor() {
	if (!plugin) {
		return;
	}
	// Walk the lifecycle backwards. Each step checks its own flag, so this is right wherever the
	// plugin got to, even if creation failed half-way. The GUI goes first: CLAP requires
	// gui.destroy() before the plugin itself is destroyed.
	gui_close();
	stop_processing_as_audio();
	deactivate();
	plugin->destroy(plugin);
	plugin = nullptr;
}

// Reads the plugin's port list and allocates a buffer for every channel of every port.
// A vocoder's inputs, for example: [0] "main" 2 channels (IS_MAIN), [1] "Source" 2 channels (the
// sidechain). Ports may only change while the plugin is deactivated, so this runs at creation
// and on restart, never mid-stream.
void ClapProcessor::setup_ports(bool p_input, std::vector<Port> &r_ports, std::vector<ClapPortInfo> &r_info, int &r_main) {
	r_ports.clear();
	r_info.clear();
	r_main = -1;
	const uint32_t count = ext_audio_ports ? ext_audio_ports->count(plugin, p_input) : 0;
	for (uint32_t i = 0; i < count; i++) {
		clap_audio_port_info_t info = {};
		if (!ext_audio_ports->get(plugin, i, p_input, &info)) {
			continue;
		}
		ClapPortInfo pi;
		pi.name = info.name;
		pi.channels = info.channel_count;
		pi.is_main = (info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0;
		if (pi.is_main && r_main < 0) {
			r_main = int(r_info.size());
		}
		r_info.push_back(pi);
		r_ports.emplace_back();
	}
	// No port flagged as main: treat the first as main, which is what DAWs do too.
	if (r_main < 0 && !r_info.empty()) {
		r_main = 0;
	}
	// Two passes on purpose: the pointers below point into each Port's own vectors, and growing
	// r_ports (emplace_back above) can move every Port in memory. So the list is completed
	// first, and only then do we take addresses.
	for (size_t i = 0; i < r_ports.size(); i++) {
		Port &port = r_ports[i];
		port.channels = r_info[i].channels;
		port.storage.assign(size_t(port.channels) * max_frames, 0.0f);
		port.ptrs.resize(port.channels);
		for (uint32_t c = 0; c < port.channels; c++) {
			port.ptrs[c] = port.storage.data() + size_t(c) * max_frames;
		}
	}

	std::vector<clap_audio_buffer_t> &buffers = p_input ? input_buffers : output_buffers;
	buffers.assign(r_ports.size(), clap_audio_buffer_t{});
	for (size_t i = 0; i < r_ports.size(); i++) {
		// 32-bit float only. A plugin can also offer 64-bit (data64), but Godot mixes in 32-bit,
		// and every plugin is required to accept 32-bit.
		buffers[i].data32 = r_ports[i].ptrs.data();
		buffers[i].channel_count = r_ports[i].channels;
	}
}

bool ClapProcessor::activate(std::string &r_error) {
	if (active) {
		return true;
	}
	// min_frames = 1: we promise nothing about a minimum block size. max_frames is binding: the
	// plugin sizes its internal buffers for it, so sending more would overrun them, which is why
	// EffectRuntime splits large blocks.
	if (!plugin->activate(plugin, sample_rate, 1, max_frames)) {
		r_error = std::string("activate failed for ") + plugin->desc->id;
		return false;
	}
	active = true;
	steady_time = 0;
	return true;
}

void ClapProcessor::deactivate() {
	if (active) {
		plugin->deactivate(plugin);
		active = false;
	}
}

void ClapProcessor::stop_processing_as_audio() {
	if (processing) {
		// stop_processing() is an audio-thread call, but here we are on the main thread. That is
		// safe only because the audio lock is held (or the processor is unreachable), so the real
		// audio thread cannot be in process() at the same moment. The scope makes thread-check
		// answer "yes, audio thread" for the duration, so a strict plugin does not object.
		AudioThreadScope scope;
		plugin->stop_processing(plugin);
		processing = false;
	}
}

void ClapProcessor::detach_from_audio() {
	stop_processing_as_audio();
}

void ClapProcessor::restart() {
	restart_requested.store(false, std::memory_order_release);
	stop_processing_as_audio();
	deactivate();
	// Ports may only change while the plugin is inactive, so this is where a rescan lands.
	setup_ports(true, inputs, input_info, main_input);
	setup_ports(false, outputs, output_info, main_output);
	std::string err;
	if (!activate(err)) {
		std::fprintf(stderr, "[clap_host] %s\n", err.c_str());
	}
}

void ClapProcessor::idle() {
	// request_callback() may be called from any thread; on_main_thread() must run on the main
	// thread. The atomic flag is the hand-off between them. exchange(false) reads the flag and
	// clears it in one indivisible step, so a request arriving at the same moment is not lost.
	if (callback_requested.exchange(false, std::memory_order_acq_rel)) {
		plugin->on_main_thread(plugin);
	}
	// The GUI's timers and fds (run_loop.h).
	run_loop.pump();
	// A plugin that is not processing (the probe, with its GUI open) reports parameter changes
	// made in its GUI by asking for a flush: params.flush() then hands them over as output events
	// (out_events_try_push queues them, pop_output_param delivers them). A processing instance
	// reports them through process() instead, so flush may not run alongside it.
	if (flush_requested.exchange(false, std::memory_order_acq_rel) && !active && ext_params) {
		in_event_count = 0;
		ext_params->flush(plugin, &in_events, &out_events);
	}
}

bool ClapProcessor::pop_output_param(ParamEvent &r_event) {
	return output_params.pop(r_event);
}

// AUDIO THREAD. The real-time rules apply to everything from here to the end of the function:
// every buffer used below was allocated in create() or setup_ports().
bool ClapProcessor::process(const AudioBlock &p_block) {
	t_audio_thread = true;
	if (!active || main_output < 0 || p_block.frames > max_frames) {
		return false;
	}
	// start_processing() is lazy, on the first block, because it must run on the audio thread
	// and this is the first moment we are on it.
	if (!processing) {
		if (!plugin->start_processing(plugin)) {
			return false;
		}
		processing = true;
	}
	const uint32_t n = p_block.frames;
	const size_t bytes = sizeof(float) * n;

	// Inputs are copied rather than aliased: a plugin may process in place (write its output
	// into its input buffer), which would corrupt EffectRuntime's buffers if we lent them out.
	//
	// The bus is always stereo; the plugin's ports may not be (see core/channel_map.h).
	bool aux_fed = false;
	for (size_t i = 0; i < inputs.size(); i++) {
		Port &port = inputs[i];
		if (int(i) == main_input && p_block.channels >= 2) {
			copy_stereo_into_port(port.channels, port.ptrs.data(), p_block.inputs, n);
		} else if (!aux_fed && p_block.aux_inputs && p_block.aux_channels >= 2) {
			// The first non-main input is the sidechain (a vocoder's "Source", say), with the same
			// mono/stereo/wider mapping as the main input.
			aux_fed = true;
			copy_stereo_into_port(port.channels, port.ptrs.data(), p_block.aux_inputs, n);
		} else {
			// Any further aux inputs, or the sidechain when none is assigned: silence.
			for (uint32_t c = 0; c < port.channels; c++) {
				std::memset(port.ptrs[c], 0, bytes);
			}
		}
		// constant_mask is an optimisation flag: bit c set means "channel c holds the same value
		// throughout". We never set it on inputs, and clear it on outputs so we can see whether
		// the plugin set it.
		input_buffers[i].constant_mask = 0;
	}
	for (clap_audio_buffer_t &b : output_buffers) {
		b.constant_mask = 0;
	}

	// Translate our ParamEvents into CLAP's event struct. Every CLAP event starts with the same
	// header (size, time, space_id, type, flags). That lets a plugin walk a mixed list of notes,
	// parameters and MIDI by reading only the header and switching on `type`. -1 in note_id,
	// port, channel and key means "all": a plain knob move, not a per-note modulation.
	in_event_count = p_block.param_event_count < MAX_IN_EVENTS ? p_block.param_event_count : MAX_IN_EVENTS;
	for (uint32_t i = 0; i < in_event_count; i++) {
		const ParamEvent &src = p_block.param_events[i];
		clap_event_param_value_t &ev = in_event_storage[i];
		ev.header.size = sizeof(clap_event_param_value_t);
		ev.header.time = src.frame_offset < n ? src.frame_offset : 0;
		ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		ev.header.type = CLAP_EVENT_PARAM_VALUE;
		ev.header.flags = 0;
		ev.param_id = src.param_id;
		// The cookie is an optional shortcut the plugin hands out in its param info. Hosts may
		// always send nullptr, and plugins must accept it (clap/ext/params.h).
		ev.cookie = nullptr;
		ev.note_id = -1;
		ev.port_index = -1;
		ev.channel = -1;
		ev.key = -1;
		ev.value = src.value;
	}

	clap_process_t proc = {};
	proc.steady_time = steady_time;
	proc.frames_count = n;
	// No transport (tempo, bar position, playing or stopped): a game has no timeline. Plugins
	// must accept nullptr here; tempo-synced delays fall back to their own default tempo.
	proc.transport = nullptr;
	proc.audio_inputs = input_buffers.empty() ? nullptr : input_buffers.data();
	proc.audio_inputs_count = uint32_t(input_buffers.size());
	proc.audio_outputs = output_buffers.empty() ? nullptr : output_buffers.data();
	proc.audio_outputs_count = uint32_t(output_buffers.size());
	proc.in_events = &in_events;
	proc.out_events = &out_events;

	const clap_process_status status = plugin->process(plugin, &proc);
	steady_time += n;
	// The other statuses (CONTINUE, CONTINUE_IF_NOT_QUIET, TAIL, SLEEP) tell a DAW when it may
	// stop calling a silent plugin to save CPU. We always keep calling, which the spec allows.
	if (status == CLAP_PROCESS_ERROR) {
		return false;
	}

	// Main output port -> the bus. A mono output goes to both sides (src_c sticks at the last
	// channel). Channels flagged constant are filled from their first sample, because a plugin
	// that sets the flag may leave the rest of the buffer unwritten.
	const Port &out = outputs[main_output];
	const uint64_t constant = output_buffers[main_output].constant_mask;
	for (uint32_t c = 0; c < p_block.channels; c++) {
		const uint32_t src_c = c < out.channels ? c : out.channels - 1;
		const float *src = out.ptrs[src_c];
		if (src_c < 64 && (constant & (uint64_t(1) << src_c))) {
			const float v = src[0];
			for (uint32_t f = 0; f < n; f++) {
				p_block.outputs[c][f] = v;
			}
		} else {
			std::memcpy(p_block.outputs[c], src, bytes);
		}
	}
	return true;
}

uint32_t ClapProcessor::in_events_size(const clap_input_events_t *p_list) {
	return static_cast<const ClapProcessor *>(p_list->ctx)->in_event_count;
}

const clap_event_header_t *ClapProcessor::in_events_get(const clap_input_events_t *p_list, uint32_t p_index) {
	const ClapProcessor *p = static_cast<const ClapProcessor *>(p_list->ctx);
	return p_index < p->in_event_count ? &p->in_event_storage[p_index].header : nullptr;
}

bool ClapProcessor::out_events_try_push(const clap_output_events_t *p_list, const clap_event_header_t *p_event) {
	ClapProcessor *p = static_cast<ClapProcessor *>(p_list->ctx);
	// Check the header before casting: a plugin can send events from other "spaces" (its own
	// custom event types), and those have a different layout behind the same header.
	if (p_event->space_id == CLAP_CORE_EVENT_SPACE_ID && p_event->type == CLAP_EVENT_PARAM_VALUE) {
		const clap_event_param_value_t *ev = reinterpret_cast<const clap_event_param_value_t *>(p_event);
		ParamEvent out;
		out.param_id = ev->param_id;
		out.frame_offset = p_event->time;
		out.value = ev->value;
		return p->output_params.push(out);
	}
	// Gestures, notes and the rest are accepted and ignored for now.
	return true;
}

std::vector<ParamInfo> ClapProcessor::read_params() const {
	std::vector<ParamInfo> out;
	if (!ext_params) {
		return out;
	}
	const uint32_t n = ext_params->count(plugin);
	for (uint32_t i = 0; i < n; i++) {
		clap_param_info_t info = {};
		if (!ext_params->get_info(plugin, i, &info)) {
			continue;
		}
		ParamInfo p;
		p.id = info.id;
		p.name = info.name;
		p.module = info.module;
		p.min_value = info.min_value;
		p.max_value = info.max_value;
		p.default_value = info.default_value;
		if (info.flags & CLAP_PARAM_IS_STEPPED) {
			// CLAP stepped parameters move in whole numbers across their plain range.
			p.flags |= PARAM_STEPPED;
			p.step_count = uint32_t(info.max_value - info.min_value);
		}
		if (info.flags & CLAP_PARAM_IS_AUTOMATABLE) {
			p.flags |= PARAM_AUTOMATABLE;
		}
		if (info.flags & CLAP_PARAM_IS_READONLY) {
			p.flags |= PARAM_READ_ONLY;
		}
		if (info.flags & CLAP_PARAM_IS_HIDDEN) {
			p.flags |= PARAM_HIDDEN;
		}
		if (info.flags & CLAP_PARAM_IS_BYPASS) {
			p.flags |= PARAM_BYPASS;
		}
		out.push_back(p);
	}
	return out;
}

bool ClapProcessor::value_to_text(uint32_t p_param_id, double p_value, std::string &r_text) const {
	if (!ext_params || !ext_params->value_to_text) {
		return false;
	}
	char buf[256] = {};
	if (!ext_params->value_to_text(plugin, p_param_id, p_value, buf, sizeof(buf))) {
		return false;
	}
	r_text = buf;
	return true;
}

// --- State and main-thread parameter access -------------------------------------------------
//
// CLAP state goes through two tiny stream objects: the plugin calls write() (saving) or read()
// (loading) as many times as it likes, with chunks of any size, and the host keeps the bytes.
// Like clap_host, each is a C struct with a `ctx` pointer back to our data.

namespace {
struct ReadCursor {
	const std::vector<uint8_t> *data;
	size_t pos;
};
} // namespace

bool ClapProcessor::save_state(std::vector<uint8_t> &r_data) {
	if (!ext_state) {
		return false;
	}
	r_data.clear();
	clap_ostream_t out = {};
	out.ctx = &r_data;
	out.write = [](const clap_ostream_t *p_stream, const void *p_buffer, uint64_t p_size) -> int64_t {
		auto *bytes = static_cast<std::vector<uint8_t> *>(p_stream->ctx);
		const uint8_t *b = static_cast<const uint8_t *>(p_buffer);
		bytes->insert(bytes->end(), b, b + p_size);
		return int64_t(p_size);
	};
	// A lambda with an empty capture list [] converts to a plain function pointer, which is what
	// a C struct needs. One that captured anything could not.
	return ext_state->save(plugin, &out);
}

bool ClapProcessor::load_state(const std::vector<uint8_t> &p_data) {
	if (!ext_state) {
		return false;
	}
	ReadCursor cursor{ &p_data, 0 };
	clap_istream_t in = {};
	in.ctx = &cursor;
	in.read = [](const clap_istream_t *p_stream, void *p_buffer, uint64_t p_size) -> int64_t {
		auto *c = static_cast<ReadCursor *>(p_stream->ctx);
		const size_t left = c->data->size() - c->pos;
		const size_t n = p_size < left ? size_t(p_size) : left;
		std::memcpy(p_buffer, c->data->data() + c->pos, n);
		c->pos += n;
		return int64_t(n); // 0 once everything is read: end of file.
	};
	return ext_state->load(plugin, &in);
}

bool ClapProcessor::get_param_value(uint32_t p_id, double &r_value) const {
	return ext_params && ext_params->get_value(plugin, p_id, &r_value);
}

// MAIN THREAD. Only for an inactive instance (the probe): a running one gets its values through
// process(), and params.flush() may not run alongside process(). flush() is CLAP's way to hand
// a plugin parameter events when no audio is being processed.
void ClapProcessor::on_param_set(uint32_t p_id, double p_value) {
	if (active || !ext_params) {
		return;
	}
	clap_event_param_value_t &ev = in_event_storage[0];
	ev = {};
	ev.header.size = sizeof(ev);
	ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	ev.header.type = CLAP_EVENT_PARAM_VALUE;
	ev.param_id = p_id;
	ev.note_id = -1;
	ev.port_index = -1;
	ev.channel = -1;
	ev.key = -1;
	ev.value = p_value;
	in_event_count = 1;
	ext_params->flush(plugin, &in_events, &out_events);
	in_event_count = 0;
}

// --- clap_host ------------------------------------------------------------------------------
// Everything below is called BY the plugin. CLAP says which thread each one may arrive on;
// request_restart, request_process and request_callback may come from any thread, which is why
// they only set atomic flags and do the real work later, on the main thread, in idle().

const void *ClapProcessor::host_get_extension(const clap_host_t *, const char *p_id) {
	// `static const` locals are built once and live for the whole program, so the pointers we
	// hand out stay valid however long the plugin keeps them. The braces fill the struct's
	// function pointers in declaration order.
	static const clap_host_thread_check_t thread_check = { host_is_main_thread, host_is_audio_thread };
	static const clap_host_log_t log = { host_log };
	static const clap_host_params_t params = { host_params_rescan, host_params_clear, host_params_request_flush };
	static const clap_host_state_t state = { host_state_mark_dirty };
	static const clap_host_latency_t latency = { host_latency_changed };
	static const clap_host_audio_ports_t audio_ports = { host_audio_ports_is_rescan_flag_supported, host_audio_ports_rescan };
	static const clap_host_gui_t gui = { host_gui_resize_hints_changed, host_gui_request_resize, host_gui_request_show,
		host_gui_request_hide, host_gui_closed };
	static const clap_host_timer_support_t timer = { host_timer_register, host_timer_unregister };
	static const clap_host_posix_fd_support_t posix_fd = { host_fd_register, host_fd_modify, host_fd_unregister };

	if (!p_id) {
		return nullptr;
	}
	if (!std::strcmp(p_id, CLAP_EXT_THREAD_CHECK)) {
		return &thread_check;
	}
	if (!std::strcmp(p_id, CLAP_EXT_LOG)) {
		return &log;
	}
	if (!std::strcmp(p_id, CLAP_EXT_PARAMS)) {
		return &params;
	}
	if (!std::strcmp(p_id, CLAP_EXT_STATE)) {
		return &state;
	}
	if (!std::strcmp(p_id, CLAP_EXT_LATENCY)) {
		return &latency;
	}
	if (!std::strcmp(p_id, CLAP_EXT_AUDIO_PORTS)) {
		return &audio_ports;
	}
	if (!std::strcmp(p_id, CLAP_EXT_GUI)) {
		return &gui;
	}
	if (!std::strcmp(p_id, CLAP_EXT_TIMER_SUPPORT)) {
		return &timer;
	}
	if (!std::strcmp(p_id, CLAP_EXT_POSIX_FD_SUPPORT)) {
		return &posix_fd;
	}
	return nullptr;
}

void ClapProcessor::host_request_restart(const clap_host_t *p_host) {
	from_mut(p_host)->restart_requested.store(true, std::memory_order_release);
}

void ClapProcessor::host_request_process(const clap_host_t *) {
	// Every bus with a loaded plugin is processed continuously; nothing to wake.
}

void ClapProcessor::host_request_callback(const clap_host_t *p_host) {
	from_mut(p_host)->callback_requested.store(true, std::memory_order_release);
}

bool ClapProcessor::host_is_main_thread(const clap_host_t *p_host) {
	return std::this_thread::get_id() == from(p_host)->main_thread;
}

bool ClapProcessor::host_is_audio_thread(const clap_host_t *) {
	return t_audio_thread;
}

void ClapProcessor::host_log(const clap_host_t *, clap_log_severity p_severity, const char *p_msg) {
	if (t_audio_thread || !p_msg) {
		return; // No I/O on the audio thread: messages from there are dropped.
	}
	// stderr rather than Godot's print(), to keep this file free of Godot. Godot shows stderr in
	// its terminal output, not in the editor's Output panel.
	static const char *names[] = { "debug", "info", "warning", "error", "fatal", "host misbehaving", "plugin misbehaving" };
	const char *sev = (p_severity >= 0 && p_severity <= 6) ? names[p_severity] : "?";
	std::fprintf(stderr, "[clap %s] %s\n", sev, p_msg);
}

// [main-thread]. VALUES and TEXT need nothing from us: values are read fresh whenever they are
// shown. INFO or ALL means names, ranges or the list itself changed.
void ClapProcessor::host_params_rescan(const clap_host_t *p_host, clap_param_rescan_flags p_flags) {
	if (p_flags & (CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_ALL)) {
		from_mut(p_host)->params_changed.store(true, std::memory_order_release);
	}
}

void ClapProcessor::host_params_clear(const clap_host_t *, clap_id, clap_param_clear_flags) {
}

// [thread-safe]. Handled in idle(); see there.
void ClapProcessor::host_params_request_flush(const clap_host_t *p_host) {
	from_mut(p_host)->flush_requested.store(true, std::memory_order_release);
}

void ClapProcessor::host_state_mark_dirty(const clap_host_t *) {
}

void ClapProcessor::host_latency_changed(const clap_host_t *) {
	// Godot's buses have no latency compensation to update.
}

bool ClapProcessor::host_audio_ports_is_rescan_flag_supported(const clap_host_t *, uint32_t) {
	return true;
}

void ClapProcessor::host_audio_ports_rescan(const clap_host_t *p_host, uint32_t) {
	// Ports are re-read whenever the plugin is restarted.
	from_mut(p_host)->restart_requested.store(true, std::memory_order_release);
}

} // namespace gaph

// --- GUI -------------------------------------------------------------------------------------
//
// The order clap/ext/gui.h prescribes for an embedded GUI:
//   is_api_supported -> create -> set_scale -> get_size -> set_parent -> show
// (Plugins built on nice-plug honour set_scale, and abort inside
// create() if the host offers no clap.gui; ours offers it, see host_get_extension.)
// and hide -> destroy to close. "Embedded" (is_floating = false) is the only kind nih-plug
// offers, and the one this host does: the plugin draws inside a window we own.

namespace gaph {

bool ClapProcessor::gui_available() {
	return ext_gui && ext_gui->is_api_supported(plugin, plugin_window_api().clap, false);
}

bool ClapProcessor::gui_open(uint64_t p_parent, double p_scale, uint32_t &r_width, uint32_t &r_height, std::string &r_error) {
	if (gui_created) {
		return true;
	}
	if (!gui_available()) {
		r_error = std::string("this plugin has no embeddable GUI for this platform (CLAP window API \"") + plugin_window_api().clap + "\")";
		return false;
	}
	if (!ext_gui->create(plugin, plugin_window_api().clap, false)) {
		r_error = "gui.create failed";
		return false;
	}
	gui_created = true;
	// The scale before the size: get_size() answers in physical pixels at the scale set, and
	// some plugins (nice-plug's) report 0x0 until set_scale() has been called. A plugin that
	// works out its scale from the OS itself may refuse the call; that's fine. Not on macOS:
	// "cocoa" sizes are logical points and gui.h says not to call it (native_window.h).
	if (plugin_window_api().host_sets_scale) {
		ext_gui->set_scale(plugin, p_scale > 0 ? p_scale : 1.0);
	}
	r_width = 0;
	r_height = 0;
	if (!ext_gui->get_size(plugin, &r_width, &r_height) || r_width == 0 || r_height == 0) {
		r_width = 400;
		r_height = 300;
	}
	clap_window_t window = {};
	window.api = plugin_window_api().clap;
	// The handle sits in a union: x11 is an integer (an XID), win32 and cocoa are pointers (an
	// HWND, an NSView*). Filling the field of the right type matters on 32-bit builds.
	if (std::strcmp(window.api, CLAP_WINDOW_API_X11) == 0) {
		window.x11 = clap_xwnd(p_parent);
	} else {
		window.ptr = reinterpret_cast<void *>(uintptr_t(p_parent));
	}
	if (!ext_gui->set_parent(plugin, &window)) {
		r_error = "gui.set_parent failed";
		gui_close();
		return false;
	}
	ext_gui->show(plugin);
	return true;
}

void ClapProcessor::gui_close() {
	if (!gui_created) {
		return;
	}
	ext_gui->hide(plugin);
	ext_gui->destroy(plugin);
	gui_created = false;
	resize_pending = false;
}

bool ClapProcessor::gui_take_resize(uint32_t &r_width, uint32_t &r_height) {
	if (!resize_pending) {
		return false;
	}
	resize_pending = false;
	r_width = resize_width;
	r_height = resize_height;
	return true;
}

// [thread-safe & !floating] per the spec; nih-plug calls it from the main thread. The host
// resizes its window on the next frame (via gui_take_resize), and the plugin's own view already
// has the new size.
bool ClapProcessor::host_gui_request_resize(const clap_host_t *p_host, uint32_t p_width, uint32_t p_height) {
	ClapProcessor *p = from_mut(p_host);
	p->resize_width = p_width;
	p->resize_height = p_height;
	p->resize_pending = true;
	return true;
}

void ClapProcessor::host_gui_resize_hints_changed(const clap_host_t *) {
	// Our window is sized to the GUI, not resizable by the user, so hints change nothing.
}

bool ClapProcessor::host_gui_request_show(const clap_host_t *) {
	return false; // The host decides when the window shows (opening it shows the GUI).
}

bool ClapProcessor::host_gui_request_hide(const clap_host_t *) {
	return false;
}

void ClapProcessor::host_gui_closed(const clap_host_t *p_host, bool) {
	from_mut(p_host)->gui_closed.store(true, std::memory_order_release);
}

// --- Timers and fds, for the GUI (run_loop.h) ----------------------------------------------
// The plugin registers them; the callbacks call back into the plugin's own extension.

bool ClapProcessor::host_timer_register(const clap_host_t *p_host, uint32_t p_period_ms, clap_id *r_timer_id) {
	ClapProcessor *p = from_mut(p_host);
	if (!p->ext_timer) {
		return false;
	}
	// The id is only known once registered, so the callback reads it back through a shared cell.
	auto id = std::make_shared<clap_id>(0);
	*id = p->run_loop.register_timer(p_period_ms, [p, id] { p->ext_timer->on_timer(p->plugin, *id); });
	*r_timer_id = *id;
	return true;
}

bool ClapProcessor::host_timer_unregister(const clap_host_t *p_host, clap_id p_timer_id) {
	return from_mut(p_host)->run_loop.unregister_timer(p_timer_id);
}

bool ClapProcessor::host_fd_register(const clap_host_t *p_host, int p_fd, clap_posix_fd_flags_t) {
	ClapProcessor *p = from_mut(p_host);
	if (!p->ext_fd) {
		return false;
	}
	// Only readability is watched (run_loop.h), which is what GUI toolkits register fds for.
	p->run_loop.register_fd(p_fd, [p, p_fd] { p->ext_fd->on_fd(p->plugin, p_fd, CLAP_POSIX_FD_READ); });
	return true;
}

bool ClapProcessor::host_fd_modify(const clap_host_t *p_host, int p_fd, clap_posix_fd_flags_t p_flags) {
	return host_fd_register(p_host, p_fd, p_flags);
}

bool ClapProcessor::host_fd_unregister(const clap_host_t *p_host, int p_fd) {
	return from_mut(p_host)->run_loop.unregister_fd(p_fd);
}

} // namespace gaph
