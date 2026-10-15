#pragma once

// Vst3Processor: one VST3 plugin instance, seen from the host side.
//
// THE BIG DIFFERENCE FROM CLAP: A PLUGIN IS TWO OBJECTS
//   component  (IComponent + IAudioProcessor)  the DSP. Runs process() on the audio thread.
//   controller (IEditController)               parameters, their text, presets, the GUI.
// They may be one object, or two that never share memory and talk only through messages
// (IConnectionPoint), so that a host could even run them in different processes. The host
// therefore has to:
//   - create both, and connect them to each other;
//   - copy the component's state into the controller after creating it (setComponentState);
//   - send every parameter change to both: to the processor through process(), to the
//     controller with setParamNormalized() on the main thread;
//   - forward edits made in the controller (its GUI) to the processor. The controller reports
//     them to us through IComponentHandler, which we implement.
//
// THE LIFECYCLE ([thread] as before)
//   createInstance ─> initialize ─> setBusArrangements, activateBus ─> setupProcessing
//    [main]            [main]        [main]                            [main]
//      ─> setActive(true) ─> setProcessing(true) ─> process ... process
//         [main]             [main or audio]         [audio]
// and the reverse to shut down: setProcessing(false), setActive(false), terminate. Unlike CLAP,
// setProcessing may be called from the main thread, so no lazy start is needed.
//
// PARAMETERS ARE ALWAYS 0..1
// VST3 exchanges every parameter as a "normalised" double in 0..1. A plugin with a -60..0 dB
// knob maps it internally. So min_value = 0 and max_value = 1 for every ParamInfo, and the
// plugin's text (getParamStringByValue) is the only way to show real units.

#include "core/plugin_processor.h"
#include "core/rt_queue.h"
#include "core/run_loop.h"
#include "vst3_module.h"

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/connectionproxy.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gaph {

struct Vst3BusInfo {
	std::string name;
	uint32_t channels = 0;
	bool is_main = false; // kMain; otherwise kAux, a sidechain.
};

class Vst3Processor : public PluginProcessor {
public:
	// p_class_id: the class's 16-byte id as 32 hex characters, or empty for the first effect.
	static std::unique_ptr<Vst3Processor> create(VST3::Hosting::Module::Ptr p_module, const std::string &p_class_id,
			double p_sample_rate, uint32_t p_max_frames, bool p_activate, std::string &r_error);

	~Vst3Processor() override;

	bool process(const AudioBlock &p_block) override;
	void detach_from_audio() override {} // setProcessing(false) is allowed on the main thread.
	bool pop_output_param(ParamEvent &r_event) override;
	void on_param_set(uint32_t p_id, double p_value) override;
	bool value_to_text(uint32_t p_id, double p_value, std::string &r_text) const override;
	bool save_state(std::vector<uint8_t> &r_data) override;
	bool load_state(const std::vector<uint8_t> &p_data) override;
	bool get_param_value(uint32_t p_id, double &r_value) const override;
	void idle() override { run_loop.pump(); }

	// GUI: the controller's IPlugView ("editor"), attached to the platform's window (an X11 id,
	// an HWND or an NSView*: core/native_window.h). See plugin_processor.h.
	bool gui_available() override;
	bool gui_open(uint64_t p_parent, double p_scale, uint32_t &r_width, uint32_t &r_height, std::string &r_error) override;
	void gui_close() override;
	bool gui_is_open() const override { return view != nullptr; }
	bool gui_take_resize(uint32_t &r_width, uint32_t &r_height) override;
	bool needs_restart() override { return restart_requested.load(std::memory_order_acquire); }
	void restart() override;

	// Main thread.
	std::vector<ParamInfo> read_params() const override;
	bool take_params_changed() override { return params_changed.exchange(false, std::memory_order_acq_rel); }
	const std::vector<Vst3BusInfo> &get_input_buses() const { return input_info; }
	const std::vector<Vst3BusInfo> &get_output_buses() const { return output_info; }
	const VST3::Hosting::ClassInfo &get_class_info() const { return class_info; }

private:
	Vst3Processor() = default;

	// IComponentHandler: how the controller talks back to us. A GUI knob turn arrives as
	// beginEdit (mouse down), performEdit (each move), endEdit (mouse up). The macros from
	// funknownimpl.h write queryInterface/addRef/release; "NonDestroyable" makes release() a
	// no-op, because this object lives and dies with the Vst3Processor around it.
	class ComponentHandler : public Steinberg::U::ImplementsNonDestroyable<Steinberg::U::Directly<Steinberg::Vst::IComponentHandler>> {
	public:
		Vst3Processor *owner = nullptr;
		Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override { return Steinberg::kResultOk; }
		Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID p_id, Steinberg::Vst::ParamValue p_value) override;
		Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override { return Steinberg::kResultOk; }
		Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 p_flags) override;
	};

	// IPlugFrame: how a view asks its host to resize the window. On Linux the same object is
	// also the IRunLoop (timers and fds, run_loop.h): a VST3 view finds it by asking its frame
	// with queryInterface, since Linux has no system-wide event loop to use instead.
	class PlugFrame : public Steinberg::U::ImplementsNonDestroyable<Steinberg::U::Directly<Steinberg::IPlugFrame
#if SMTG_OS_LINUX
			,
			Steinberg::Linux::IRunLoop
#endif
			>> {
	public:
		Vst3Processor *owner = nullptr;
		Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView *p_view, Steinberg::ViewRect *p_size) override;
#if SMTG_OS_LINUX
		Steinberg::tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler *p_handler, Steinberg::Linux::FileDescriptor p_fd) override;
		Steinberg::tresult PLUGIN_API unregisterEventHandler(Steinberg::Linux::IEventHandler *p_handler) override;
		Steinberg::tresult PLUGIN_API registerTimer(Steinberg::Linux::ITimerHandler *p_handler, Steinberg::Linux::TimerInterval p_ms) override;
		Steinberg::tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler *p_handler) override;

		// Which run-loop entry belongs to which handler, to unregister by handler.
		std::vector<std::pair<Steinberg::Linux::IEventHandler *, int>> fd_handlers;
		std::vector<std::pair<Steinberg::Linux::ITimerHandler *, uint32_t>> timer_handlers;
#endif
	};

	struct Bus {
		uint32_t channels = 0;
		std::vector<float> storage;
		std::vector<float *> ptrs;
	};

	bool setup_buses(std::string &r_error);
	bool start(std::string &r_error);
	void stop();
	void sync_controller_state();

	VST3::Hosting::Module::Ptr module; // First, so it is destroyed last: see clap_processor.h.
	VST3::Hosting::ClassInfo class_info;
	Steinberg::IPtr<Steinberg::Vst::IComponent> component;
	Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor;
	Steinberg::IPtr<Steinberg::Vst::IEditController> controller;
	bool single_component = false; // Controller and component are the same object.
	bool component_initialized = false;
	bool controller_initialized = false;
	Steinberg::IPtr<Steinberg::Vst::ConnectionProxy> component_cp;
	Steinberg::IPtr<Steinberg::Vst::ConnectionProxy> controller_cp;
	ComponentHandler handler;
	PlugFrame frame;
	RunLoop run_loop;
	Steinberg::IPtr<Steinberg::IPlugView> view;
	int gui_supported = -1; // -1 not checked yet, 0 no, 1 yes
	bool resize_pending = false;
	uint32_t resize_width = 0, resize_height = 0;

	double sample_rate = 48000.0;
	uint32_t max_frames = 512;
	bool active = false;
	bool processing = false;
	int64_t sample_time = 0;
	std::atomic<bool> restart_requested{ false };
	std::atomic<bool> params_changed{ false };

	std::vector<Bus> inputs;
	std::vector<Bus> outputs;
	std::vector<Vst3BusInfo> input_info;
	std::vector<Vst3BusInfo> output_info;
	int main_input = -1;
	int main_output = -1;
	std::vector<Steinberg::Vst::AudioBusBuffers> input_buffers;
	std::vector<Steinberg::Vst::AudioBusBuffers> output_buffers;

	// SDK helpers holding the per-block parameter changes, in and out. Sized to the parameter
	// count when the plugin starts, so the audio thread only reuses what is already there.
	Steinberg::Vst::ParameterChanges input_changes;
	Steinberg::Vst::ParameterChanges output_changes;
	Steinberg::Vst::ProcessContext context = {};

	// Plugin-made parameter changes reach us two ways: from process() (audio thread, lock-free
	// queue) and from the controller via performEdit (main thread, or any thread in some
	// plugins, hence the mutex). The mutex is never taken on the audio thread.
	RtQueue<ParamEvent> audio_out_params{ 1024 };
	std::mutex edit_mutex;
	std::vector<ParamEvent> edit_out_params;
};

} // namespace gaph
