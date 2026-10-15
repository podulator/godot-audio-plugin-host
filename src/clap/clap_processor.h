#pragma once

// ClapProcessor: one CLAP plugin instance, seen from the host side.
//
// THE CLAP LIFECYCLE (clap/plugin.h). Each arrow is one call, and the thread it must run on is
// part of the rule:
//
//   create_plugin ─> init ─> activate(rate, min, max) ─> start_processing ─> process ... process
//    [main]          [main]   [main]                       [audio]             [audio]
//                                                                                  │
//   destroy <─ deactivate <────────────────────────────── stop_processing <────────┘
//    [main]     [main]                                      [audio]
//
//   - init:     the plugin sets itself up. From here its extensions (params, ports...) can be read.
//   - activate: the host promises a sample rate and a maximum block size. The plugin allocates
//               its buffers now, so that process() never has to.
//   - start/stop_processing: bracket a run of process() calls on the audio thread.
//
// THE HOST SIDE: clap_host
// A plugin can talk back to the host, through the clap_host struct we give it at creation. It is
// a C struct of function pointers (C, because C is the one ABI every compiler and language agrees
// on). Each pointer goes to one of our static host_* functions below. The plugin passes the
// struct back on every call, and `host_data` inside it points at our ClapProcessor, so a static
// function can find its object again. That is the usual way to fit C++ objects behind a C API.
//
// EXTENSIONS
// Everything beyond the basic lifecycle is an "extension", looked up by string id. The plugin
// asks the host for e.g. "clap.thread-check" (host_get_extension), and we ask the plugin for e.g.
// "clap.params". A missing extension (nullptr) is normal and must be handled.

#include "clap_library.h"
#include "core/plugin_processor.h"
#include "core/rt_queue.h"
#include "core/run_loop.h"

#include <clap/clap.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace gaph {

// One audio port, for display and debugging. A vocoder may have two inputs: main, and "Source" (the
// sidechain).
struct ClapPortInfo {
	std::string name;
	uint32_t channels = 0;
	bool is_main = false;
};

// Free of Godot, so it can be tested from plain C++.
class ClapProcessor : public PluginProcessor {
public:
	// Creates and initialises the plugin, and activates it unless p_activate is false (enough to
	// read parameters and ports). Returns null with r_error set on failure.
	static std::unique_ptr<ClapProcessor> create(std::shared_ptr<ClapLibrary> p_library, const std::string &p_plugin_id,
			double p_sample_rate, uint32_t p_max_frames, bool p_activate, std::string &r_error);

	~ClapProcessor() override;

	bool process(const AudioBlock &p_block) override;
	void detach_from_audio() override;
	void idle() override;
	bool pop_output_param(ParamEvent &r_event) override;
	bool needs_restart() override { return restart_requested.load(std::memory_order_acquire); }
	void restart() override;

	// Main thread.
	std::vector<ParamInfo> read_params() const override;
	bool take_params_changed() override { return params_changed.exchange(false, std::memory_order_acq_rel); }
	const std::vector<ClapPortInfo> &get_input_ports() const { return input_info; }
	const std::vector<ClapPortInfo> &get_output_ports() const { return output_info; }
	const clap_plugin_descriptor_t *get_descriptor() const { return plugin ? plugin->desc : nullptr; }
	bool value_to_text(uint32_t p_param_id, double p_value, std::string &r_text) const override;
	bool save_state(std::vector<uint8_t> &r_data) override;
	bool load_state(const std::vector<uint8_t> &p_data) override;
	bool get_param_value(uint32_t p_id, double &r_value) const override;
	void on_param_set(uint32_t p_id, double p_value) override;

	// GUI: CLAP's gui extension, embedded (not floating, the only kind nih-plug supports) into
	// the platform's window: "x11", "win32" or "cocoa" (core/native_window.h). See
	// plugin_processor.h.
	bool gui_available() override;
	bool gui_open(uint64_t p_parent, double p_scale, uint32_t &r_width, uint32_t &r_height, std::string &r_error) override;
	void gui_close() override;
	bool gui_is_open() const override { return gui_created; }
	bool gui_take_resize(uint32_t &r_width, uint32_t &r_height) override;
	bool gui_take_closed() override { return gui_closed.exchange(false, std::memory_order_acq_rel); }

private:
	ClapProcessor() = default;

	// One port's planar buffers: `storage` holds every channel back to back, and ptrs[c] points
	// at channel c inside it. A clap_audio_buffer wants exactly that `float **` shape.
	struct Port {
		uint32_t channels = 0;
		std::vector<float> storage;
		std::vector<float *> ptrs;
	};

	bool activate(std::string &r_error);
	void deactivate();
	void stop_processing_as_audio();
	void setup_ports(bool p_input, std::vector<Port> &r_ports, std::vector<ClapPortInfo> &r_info, int &r_main);

	// Turn the clap_host pointer the plugin hands back into our object (see host_data above).
	static const ClapProcessor *from(const clap_host_t *p_host) { return static_cast<const ClapProcessor *>(p_host->host_data); }
	static ClapProcessor *from_mut(const clap_host_t *p_host) { return static_cast<ClapProcessor *>(p_host->host_data); }

	// The functions the plugin can call on us. `static` because a C function pointer cannot point
	// at a member function of a particular object.
	static const void *host_get_extension(const clap_host_t *p_host, const char *p_id);
	static void host_request_restart(const clap_host_t *p_host);
	static void host_request_process(const clap_host_t *p_host);
	static void host_request_callback(const clap_host_t *p_host);
	static bool host_is_main_thread(const clap_host_t *p_host);
	static bool host_is_audio_thread(const clap_host_t *p_host);
	static void host_log(const clap_host_t *p_host, clap_log_severity p_severity, const char *p_msg);
	static void host_params_rescan(const clap_host_t *p_host, clap_param_rescan_flags p_flags);
	static void host_params_clear(const clap_host_t *p_host, clap_id p_param_id, clap_param_clear_flags p_flags);
	static void host_params_request_flush(const clap_host_t *p_host);
	static void host_state_mark_dirty(const clap_host_t *p_host);
	static void host_latency_changed(const clap_host_t *p_host);
	static bool host_audio_ports_is_rescan_flag_supported(const clap_host_t *p_host, uint32_t p_flag);
	static void host_audio_ports_rescan(const clap_host_t *p_host, uint32_t p_flags);
	// clap.gui (host side): the GUI asking for things.
	static void host_gui_resize_hints_changed(const clap_host_t *p_host);
	static bool host_gui_request_resize(const clap_host_t *p_host, uint32_t p_width, uint32_t p_height);
	static bool host_gui_request_show(const clap_host_t *p_host);
	static bool host_gui_request_hide(const clap_host_t *p_host);
	static void host_gui_closed(const clap_host_t *p_host, bool p_was_destroyed);
	// clap.timer-support and clap.posix-fd-support: the run loop Linux GUIs need (run_loop.h).
	static bool host_timer_register(const clap_host_t *p_host, uint32_t p_period_ms, clap_id *r_timer_id);
	static bool host_timer_unregister(const clap_host_t *p_host, clap_id p_timer_id);
	static bool host_fd_register(const clap_host_t *p_host, int p_fd, clap_posix_fd_flags_t p_flags);
	static bool host_fd_modify(const clap_host_t *p_host, int p_fd, clap_posix_fd_flags_t p_flags);
	static bool host_fd_unregister(const clap_host_t *p_host, int p_fd);
	// Event lists, also C structs of function pointers: the plugin calls size()/get() to read the
	// events we send it, and try_push() to send events back.
	static uint32_t in_events_size(const clap_input_events_t *p_list);
	static const clap_event_header_t *in_events_get(const clap_input_events_t *p_list, uint32_t p_index);
	static bool out_events_try_push(const clap_output_events_t *p_list, const clap_event_header_t *p_event);

	// Declared first so it is destroyed last (members are destroyed in reverse order): the
	// library must stay loaded until the plugin's code has finished running.
	std::shared_ptr<ClapLibrary> library;
	clap_host_t host = {};
	const clap_plugin_t *plugin = nullptr;
	bool plugin_initialized = false;
	const clap_plugin_params_t *ext_params = nullptr;
	const clap_plugin_audio_ports_t *ext_audio_ports = nullptr;
	const clap_plugin_state_t *ext_state = nullptr;
	const clap_plugin_gui_t *ext_gui = nullptr;
	const clap_plugin_timer_support_t *ext_timer = nullptr;
	const clap_plugin_posix_fd_support_t *ext_fd = nullptr;

	RunLoop run_loop;
	bool gui_created = false;
	std::atomic<bool> gui_closed{ false };
	std::atomic<bool> flush_requested{ false };
	bool resize_pending = false;
	uint32_t resize_width = 0, resize_height = 0;

	std::thread::id main_thread; // Recorded at creation, which is always on the main thread.
	double sample_rate = 48000.0;
	uint32_t max_frames = 512;
	bool active = false;
	bool processing = false; // Only changed on the audio thread, or with it held off.
	int64_t steady_time = 0; // Running sample count; CLAP requires it to keep increasing.

	// Set from whatever thread the plugin calls from; read on the main thread. Atomic because
	// those can be different threads.
	std::atomic<bool> callback_requested{ false };
	std::atomic<bool> restart_requested{ false };
	std::atomic<bool> params_changed{ false };

	std::vector<Port> inputs;
	std::vector<Port> outputs;
	std::vector<ClapPortInfo> input_info;
	std::vector<ClapPortInfo> output_info;
	int main_input = -1; // Index into inputs, or -1 if there is no input port.
	int main_output = -1;
	// What we actually pass to the plugin: one clap_audio_buffer per port, pointing into Port.
	std::vector<clap_audio_buffer_t> input_buffers;
	std::vector<clap_audio_buffer_t> output_buffers;

	// Parameter events in CLAP's own struct, rebuilt every block from AudioBlock::param_events.
	std::vector<clap_event_param_value_t> in_event_storage;
	uint32_t in_event_count = 0;
	clap_input_events_t in_events = {};
	clap_output_events_t out_events = {};
	// Audio thread pushes (the plugin changed a parameter itself), main thread pops.
	RtQueue<ParamEvent> output_params{ 1024 };
};

} // namespace gaph
