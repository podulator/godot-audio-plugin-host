#include "vst3_processor.h"
#include "core/native_window.h"
#include "core/channel_map.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <cstdio>
#include <cstring>

namespace gaph {

using namespace Steinberg;
using namespace Steinberg::Vst;

std::unique_ptr<Vst3Processor> Vst3Processor::create(VST3::Hosting::Module::Ptr p_module, const std::string &p_class_id,
		double p_sample_rate, uint32_t p_max_frames, bool p_activate, std::string &r_error) {
	if (!p_module) {
		r_error = "no module";
		return nullptr;
	}
	std::unique_ptr<Vst3Processor> p(new Vst3Processor());
	p->module = std::move(p_module);
	p->sample_rate = p_sample_rate;
	p->max_frames = p_max_frames;
	p->handler.owner = p.get();
	p->frame.owner = p.get();

	// Choose the class: the requested one, or the first audio effect.
	bool found = false;
	for (const auto &c : vst3_effect_classes(p->module)) {
		if (p_class_id.empty() || c.ID().toString() == p_class_id) {
			p->class_info = c;
			found = true;
			break;
		}
	}
	if (!found) {
		r_error = p_class_id.empty() ? "module contains no audio effect" : "no effect class with id " + p_class_id;
		return nullptr;
	}

	// 1. The component. U::cast<T> is queryInterface wrapped in an IPtr: "give me your T
	//    interface, or nullptr if you don't have one".
	const VST3::Hosting::PluginFactory &factory = p->module->getFactory();
	p->component = factory.createInstance<IComponent>(p->class_info.ID());
	if (!p->component) {
		r_error = "could not create component of " + p->class_info.name();
		return nullptr;
	}
	if (p->component->initialize(vst3_host_context()) != kResultOk) {
		r_error = "component initialize failed";
		return nullptr;
	}
	p->component_initialized = true;
	p->processor = U::cast<IAudioProcessor>(p->component);
	if (!p->processor) {
		r_error = "component has no IAudioProcessor";
		return nullptr;
	}
	if (p->processor->canProcessSampleSize(kSample32) != kResultTrue) {
		r_error = "plugin cannot process 32-bit float";
		return nullptr;
	}

	// 2. The controller: either the component itself, or a separate class the component names.
	p->controller = U::cast<IEditController>(p->component);
	if (p->controller) {
		p->single_component = true;
	} else {
		TUID cid;
		if (p->component->getControllerClassId(cid) == kResultTrue) {
			p->controller = factory.createInstance<IEditController>(VST3::UID(cid));
			if (p->controller && p->controller->initialize(vst3_host_context()) == kResultOk) {
				p->controller_initialized = true;
			} else {
				p->controller = nullptr;
			}
		}
	}
	// A plugin without a controller still makes sound; it just has no parameters to show.
	if (p->controller) {
		p->controller->setComponentHandler(&p->handler);
	}

	// 3. Connect the halves so they can message each other. Each side gets a ConnectionProxy
	//    wrapped around the other, which the SDK uses so a disconnect can be forced from our
	//    side even if a plugin keeps holding on to the pointer.
	if (p->controller && !p->single_component) {
		auto comp_cp = U::cast<IConnectionPoint>(p->component);
		auto ctrl_cp = U::cast<IConnectionPoint>(p->controller);
		if (comp_cp && ctrl_cp) {
			p->component_cp = owned(new ConnectionProxy(comp_cp));
			p->controller_cp = owned(new ConnectionProxy(ctrl_cp));
			p->component_cp->connect(ctrl_cp);
			p->controller_cp->connect(comp_cp);
		}
	}
	p->sync_controller_state();

	if (!p->setup_buses(r_error)) {
		return nullptr;
	}
	if (p_activate && !p->start(r_error)) {
		return nullptr;
	}
	return p;
}

// The component is the source of truth for state (preset data, parameter values). A fresh
// controller learns it by reading the component's saved state as a byte stream.
void Vst3Processor::sync_controller_state() {
	if (!controller || single_component) {
		return;
	}
	MemoryStream stream;
	if (component->getState(&stream) == kResultOk) {
		stream.seek(0, IBStream::kIBSeekSet, nullptr);
		controller->setComponentState(&stream);
	}
}

Vst3Processor::~Vst3Processor() {
	// The reverse of create(), the same order the SDK's PlugProvider uses, GUI first.
	gui_close();
	stop();
	if (controller) {
		controller->setComponentHandler(nullptr);
	}
	if (component_cp) {
		component_cp->disconnect();
	}
	if (controller_cp) {
		controller_cp->disconnect();
	}
	if (component && component_initialized) {
		component->terminate();
	}
	if (controller && controller_initialized) {
		controller->terminate();
	}
	// IPtr members release their objects as they are destroyed; the module last of all.
}

// Buses are VST3's ports. Everything here must happen while the plugin is inactive.
bool Vst3Processor::setup_buses(std::string &r_error) {
	auto read = [&](BusDirection p_dir, std::vector<Vst3BusInfo> &r_info, int &r_main) {
		r_info.clear();
		r_main = -1;
		const int32 n = component->getBusCount(kAudio, p_dir);
		for (int32 i = 0; i < n; i++) {
			BusInfo bi = {};
			component->getBusInfo(kAudio, p_dir, i, bi);
			Vst3BusInfo info;
			info.name = Steinberg::Vst::StringConvert::convert(bi.name); // UTF-16 (String128) to UTF-8.
			info.is_main = bi.busType == kMain;
			if (info.is_main && r_main < 0) {
				r_main = int(i);
			}
			r_info.push_back(info);
		}
		if (r_main < 0 && !r_info.empty()) {
			r_main = 0;
		}
	};
	read(kInput, input_info, main_input);
	read(kOutput, output_info, main_output);

	// Ask for stereo on the main buses and leave the rest as the plugin has them. A plugin may
	// refuse (a mono-only effect, say); then we use whatever it ends up with, which is why the
	// channel counts are read back afterwards rather than assumed.
	auto wanted = [&](BusDirection p_dir, const std::vector<Vst3BusInfo> &p_info, int p_main) {
		std::vector<SpeakerArrangement> arr(p_info.size(), SpeakerArr::kStereo);
		for (size_t i = 0; i < p_info.size(); i++) {
			if (int(i) != p_main) {
				processor->getBusArrangement(p_dir, int32(i), arr[i]);
			}
		}
		return arr;
	};
	std::vector<SpeakerArrangement> in_arr = wanted(kInput, input_info, main_input);
	std::vector<SpeakerArrangement> out_arr = wanted(kOutput, output_info, main_output);
	processor->setBusArrangements(in_arr.data(), int32(in_arr.size()), out_arr.data(), int32(out_arr.size()));

	auto allocate = [&](BusDirection p_dir, std::vector<Vst3BusInfo> &r_info, std::vector<Bus> &r_buses, std::vector<AudioBusBuffers> &r_buffers) {
		r_buses.assign(r_info.size(), Bus{});
		for (size_t i = 0; i < r_info.size(); i++) {
			SpeakerArrangement arr = 0;
			processor->getBusArrangement(p_dir, int32(i), arr);
			r_info[i].channels = uint32_t(SpeakerArr::getChannelCount(arr));
			// Every audio bus is switched on, sidechains included: a plugin may refuse to run
			// when one is off, and a plugin needs its sidechain bus active to see a sidechain.
			// When no sidechain is assigned it is fed silence, flagged as such.
			component->activateBus(kAudio, p_dir, int32(i), true);
			Bus &b = r_buses[i];
			b.channels = r_info[i].channels;
			b.storage.assign(size_t(b.channels) * max_frames, 0.0f);
			b.ptrs.resize(b.channels);
			for (uint32_t c = 0; c < b.channels; c++) {
				b.ptrs[c] = b.storage.data() + size_t(c) * max_frames;
			}
		}
		// As in clap_processor.cpp: pointers taken only once the bus list is final.
		r_buffers.assign(r_buses.size(), AudioBusBuffers{});
		for (size_t i = 0; i < r_buses.size(); i++) {
			r_buffers[i].numChannels = int32(r_buses[i].channels);
			r_buffers[i].channelBuffers32 = r_buses[i].ptrs.data();
		}
	};
	allocate(kInput, input_info, inputs, input_buffers);
	allocate(kOutput, output_info, outputs, output_buffers);

	// Event buses carry MIDI-like notes. An effect on a Godot bus never gets any, so they are
	// switched off, and process() can pass no event lists at all.
	for (BusDirection dir : { kInput, kOutput }) {
		for (int32 i = 0; i < component->getBusCount(kEvent, dir); i++) {
			component->activateBus(kEvent, dir, i, false);
		}
	}
	if (main_output < 0) {
		r_error = "plugin has no audio output";
		return false;
	}
	return true;
}

bool Vst3Processor::start(std::string &r_error) {
	// The equivalent of CLAP's activate(): sample rate and maximum block size are promised here,
	// and the plugin allocates for them.
	ProcessSetup setup = {};
	setup.processMode = kRealtime;
	setup.symbolicSampleSize = kSample32;
	setup.maxSamplesPerBlock = int32(max_frames);
	setup.sampleRate = sample_rate;
	if (processor->setupProcessing(setup) != kResultOk) {
		r_error = "setupProcessing failed";
		return false;
	}
	if (component->setActive(true) != kResultOk) {
		r_error = "setActive(true) failed";
		return false;
	}
	active = true;
	// Many plugins return kNotImplemented here, which is fine: they simply need no notice.
	processor->setProcessing(true);
	processing = true;

	// One queue per parameter, allocated now. A block can then mention every parameter once
	// without addParameterData() having to grow the list on the audio thread.
	const int32 count = controller ? controller->getParameterCount() : 0;
	input_changes.setMaxParameters(count + 8);
	output_changes.setMaxParameters(count + 8);

	// ProcessContext describes the timeline: tempo, bar position, whether the transport plays.
	// A game has none of that, so we report a stopped transport at 120 BPM in 4/4. It is still
	// worth sending: some plugins read the sample rate or tempo from it, and crash on nullptr.
	context = {};
	context.sampleRate = sample_rate;
	context.tempo = 120.0;
	context.timeSigNumerator = 4;
	context.timeSigDenominator = 4;
	context.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid;
	sample_time = 0;
	return true;
}

void Vst3Processor::stop() {
	if (processing) {
		processor->setProcessing(false);
		processing = false;
	}
	if (active) {
		component->setActive(false);
		active = false;
	}
}

void Vst3Processor::restart() {
	restart_requested.store(false, std::memory_order_release);
	stop();
	std::string err;
	if (!setup_buses(err) || !start(err)) {
		std::fprintf(stderr, "[vst3_host] restart failed: %s\n", err.c_str());
	}
}

// restartComponent is the controller asking the host to redo part of the setup. Only some
// flags need a full stop and start; parameter value changes are picked up as edits anyway.
tresult PLUGIN_API Vst3Processor::ComponentHandler::restartComponent(int32 p_flags) {
	if (p_flags & (kReloadComponent | kIoChanged | kLatencyChanged)) {
		owner->restart_requested.store(true, std::memory_order_release);
	}
	// kParamTitlesChanged: names or ranges; kParamIDMappingChanged: the list itself (VST 3.7).
	if (p_flags & (kParamTitlesChanged | kParamIDMappingChanged | kReloadComponent)) {
		owner->params_changed.store(true, std::memory_order_release);
	}
	return kResultOk;
}

tresult PLUGIN_API Vst3Processor::ComponentHandler::performEdit(ParamID p_id, ParamValue p_value) {
	std::lock_guard<std::mutex> guard(owner->edit_mutex);
	owner->edit_out_params.push_back(ParamEvent{ p_id, 0, p_value });
	return kResultOk;
}

// AUDIO THREAD. Same rules as ClapProcessor::process(): nothing here allocates.
bool Vst3Processor::process(const AudioBlock &p_block) {
	if (!active || main_output < 0 || p_block.frames > max_frames) {
		return false;
	}
	const uint32_t n = p_block.frames;
	const size_t bytes = sizeof(float) * n;

	// Inputs: main bus from the Godot bus, the first non-main (kAux) bus from the sidechain,
	// silence for anything else. Same mapping as the CLAP side, see core/channel_map.h.
	bool aux_fed = false;
	for (size_t i = 0; i < inputs.size(); i++) {
		Bus &bus = inputs[i];
		bool silent = false;
		if (int(i) == main_input && p_block.channels >= 2) {
			copy_stereo_into_port(bus.channels, bus.ptrs.data(), p_block.inputs, n);
		} else if (!aux_fed && p_block.aux_inputs && p_block.aux_channels >= 2) {
			aux_fed = true;
			copy_stereo_into_port(bus.channels, bus.ptrs.data(), p_block.aux_inputs, n);
		} else {
			for (uint32_t c = 0; c < bus.channels; c++) {
				std::memset(bus.ptrs[c], 0, bytes);
			}
			silent = true;
		}
		// silenceFlags: bit c set means "channel c is all zeros", so a plugin can skip work.
		// Only set where we know it, the buses we filled with zeros ourselves.
		input_buffers[i].silenceFlags = !silent ? 0 : (bus.channels >= 64 ? ~uint64(0) : (uint64(1) << bus.channels) - 1);
	}
	for (AudioBusBuffers &b : output_buffers) {
		b.silenceFlags = 0;
	}

	// Parameter changes. VST3 groups them per parameter: one IParamValueQueue each, holding
	// (sampleOffset, value) points. addParameterData finds or claims a queue for the id from
	// the ones setMaxParameters() made, and addPoint at an offset already present overwrites it.
	input_changes.clearQueue();
	for (uint32_t i = 0; i < p_block.param_event_count; i++) {
		const ParamEvent &ev = p_block.param_events[i];
		int32 queue_index = 0, point_index = 0;
		if (IParamValueQueue *q = input_changes.addParameterData(ev.param_id, queue_index)) {
			q->addPoint(int32(ev.frame_offset < n ? ev.frame_offset : 0), ev.value, point_index);
		}
	}
	output_changes.clearQueue();

	context.projectTimeSamples = sample_time;
	context.continousTimeSamples = sample_time; // [sic]: the SDK's own spelling.

	ProcessData data;
	data.processMode = kRealtime;
	data.symbolicSampleSize = kSample32;
	data.numSamples = int32(n);
	data.numInputs = int32(input_buffers.size());
	data.numOutputs = int32(output_buffers.size());
	data.inputs = input_buffers.empty() ? nullptr : input_buffers.data();
	data.outputs = output_buffers.data();
	data.inputParameterChanges = &input_changes;
	data.outputParameterChanges = &output_changes;
	data.inputEvents = nullptr;
	data.outputEvents = nullptr;
	data.processContext = &context;

	const tresult result = processor->process(data);
	sample_time += n;
	if (result != kResultOk) {
		return false;
	}

	// Changes the processor made itself (a meter, an auto-gain): keep the last point of each.
	for (int32 i = 0; i < output_changes.getParameterCount(); i++) {
		IParamValueQueue *q = output_changes.getParameterData(i);
		const int32 points = q ? q->getPointCount() : 0;
		int32 offset = 0;
		ParamValue value = 0;
		if (points > 0 && q->getPoint(points - 1, offset, value) == kResultOk) {
			audio_out_params.push(ParamEvent{ q->getParameterId(), uint32_t(offset), value });
		}
	}

	const Bus &out = outputs[main_output];
	const uint64 silent = output_buffers[main_output].silenceFlags;
	for (uint32_t c = 0; c < p_block.channels; c++) {
		const uint32_t src_c = c < out.channels ? c : out.channels - 1;
		if (src_c < 64 && (silent & (uint64(1) << src_c))) {
			std::memset(p_block.outputs[c], 0, bytes); // Flagged silent: may be unwritten.
		} else {
			std::memcpy(p_block.outputs[c], out.ptrs[src_c], bytes);
		}
	}
	return true;
}

bool Vst3Processor::pop_output_param(ParamEvent &r_event) {
	if (audio_out_params.pop(r_event)) {
		return true;
	}
	std::lock_guard<std::mutex> guard(edit_mutex);
	if (edit_out_params.empty()) {
		return false;
	}
	r_event = edit_out_params.front();
	edit_out_params.erase(edit_out_params.begin());
	return true;
}

// Keep the controller's copy in step with what we send the processor, so its text, and later
// its GUI, show the value that is actually playing.
void Vst3Processor::on_param_set(uint32_t p_id, double p_value) {
	if (controller) {
		controller->setParamNormalized(p_id, p_value);
	}
}

bool Vst3Processor::value_to_text(uint32_t p_id, double p_value, std::string &r_text) const {
	String128 text = {};
	if (!controller || controller->getParamStringByValue(p_id, p_value, text) != kResultOk) {
		return false;
	}
	r_text = Steinberg::Vst::StringConvert::convert(text);
	// VST3 keeps units apart from the value text ("0.0" and "dB"); CLAP puts them in the text.
	// Joining them here makes both formats read the same in Godot. Looking the parameter up by
	// index on every call is fine for the main thread and an inspector refresh.
	for (int32 i = 0; i < controller->getParameterCount(); i++) {
		ParameterInfo info = {};
		if (controller->getParameterInfo(i, info) == kResultOk && info.id == p_id) {
			std::string units = Steinberg::Vst::StringConvert::convert(info.units);
			// Trim, because plugins differ: some give "dB", some " dB".
			units.erase(0, units.find_first_not_of(' '));
			units.erase(units.find_last_not_of(' ') + 1);
			if (!units.empty() && r_text.find(units) == std::string::npos) {
				r_text += " " + units;
			}
			break;
		}
	}
	return true;
}

// --- State ---------------------------------------------------------------------------------
//
// A VST3 plugin's state comes in two halves, matching its two objects: the component's (the
// sound) and the controller's (GUI-side settings such as a selected tab). They are packed into
// one byte array:
//
//   "GAPHVST3"  8 bytes, so a blob from anything else is refused rather than misread
//   u64         component state size, then the bytes
//   u64         controller state size, then the bytes
//
// Sizes are little-endian, written byte by byte, so the file reads the same on any machine.

static const char STATE_MAGIC[8] = { 'G', 'A', 'P', 'H', 'V', 'S', 'T', '3' };

static void put_u64(std::vector<uint8_t> &r_out, uint64_t p_v) {
	for (int i = 0; i < 8; i++) {
		r_out.push_back(uint8_t(p_v >> (8 * i)));
	}
}

static bool get_u64(const std::vector<uint8_t> &p_in, size_t &r_pos, uint64_t &r_v) {
	if (r_pos + 8 > p_in.size()) {
		return false;
	}
	r_v = 0;
	for (int i = 0; i < 8; i++) {
		r_v |= uint64_t(p_in[r_pos + i]) << (8 * i);
	}
	r_pos += 8;
	return true;
}

bool Vst3Processor::save_state(std::vector<uint8_t> &r_data) {
	// Known gap: on an instance that never runs (the probe), parameter changes reach only the
	// controller (on_param_set), not the component, so the component half of a state saved from
	// it misses them. Saved scenes still come back right, because the values set by hand are
	// stored too and sent after the state. A live instance has no such gap.
	MemoryStream comp, ctrl;
	if (component->getState(&comp) != kResultOk) {
		return false;
	}
	// A controller with nothing of its own to save is normal; that half is just empty.
	if (controller && !single_component) {
		controller->getState(&ctrl);
	}
	r_data.assign(STATE_MAGIC, STATE_MAGIC + 8);
	put_u64(r_data, uint64_t(comp.getSize()));
	r_data.insert(r_data.end(), comp.getData(), comp.getData() + comp.getSize());
	put_u64(r_data, uint64_t(ctrl.getSize()));
	r_data.insert(r_data.end(), ctrl.getData(), ctrl.getData() + ctrl.getSize());
	return true;
}

bool Vst3Processor::load_state(const std::vector<uint8_t> &p_data) {
	if (p_data.size() < 8 || std::memcmp(p_data.data(), STATE_MAGIC, 8) != 0) {
		return false;
	}
	size_t pos = 8;
	uint64_t comp_size = 0, ctrl_size = 0;
	if (!get_u64(p_data, pos, comp_size) || pos + comp_size > p_data.size()) {
		return false;
	}
	const size_t comp_at = pos;
	pos += comp_size;
	if (!get_u64(p_data, pos, ctrl_size) || pos + ctrl_size > p_data.size()) {
		return false;
	}
	const size_t ctrl_at = pos;

	// MemoryStream(pointer, size) reads straight from our buffer without copying or owning it.
	// const_cast because the constructor takes void*, though reading never writes.
	auto stream_at = [&](size_t p_at, uint64_t p_size) {
		return MemoryStream(const_cast<uint8_t *>(p_data.data() + p_at), TSize(p_size));
	};
	MemoryStream comp = stream_at(comp_at, comp_size);
	if (component->setState(&comp) != kResultOk) {
		return false;
	}
	// As at creation: the controller learns the component's state, then gets its own.
	if (controller && !single_component) {
		MemoryStream comp_again = stream_at(comp_at, comp_size);
		controller->setComponentState(&comp_again);
		if (ctrl_size > 0) {
			MemoryStream ctrl = stream_at(ctrl_at, ctrl_size);
			controller->setState(&ctrl);
		}
	}
	return true;
}

bool Vst3Processor::get_param_value(uint32_t p_id, double &r_value) const {
	if (!controller) {
		return false;
	}
	r_value = controller->getParamNormalized(p_id);
	return true;
}

std::vector<ParamInfo> Vst3Processor::read_params() const {
	std::vector<ParamInfo> out;
	if (!controller) {
		return out;
	}
	const int32 n = controller->getParameterCount();
	for (int32 i = 0; i < n; i++) {
		ParameterInfo info = {};
		if (controller->getParameterInfo(i, info) != kResultOk) {
			continue;
		}
		ParamInfo p;
		p.id = info.id;
		p.name = Steinberg::Vst::StringConvert::convert(info.title);
		p.min_value = 0.0; // Always normalised; see the header.
		p.max_value = 1.0;
		p.default_value = info.defaultNormalizedValue;
		if (info.stepCount > 0) {
			p.flags |= PARAM_STEPPED;
			p.step_count = uint32_t(info.stepCount);
		}
		if (info.flags & ParameterInfo::kCanAutomate) {
			p.flags |= PARAM_AUTOMATABLE;
		}
		if (info.flags & ParameterInfo::kIsReadOnly) {
			p.flags |= PARAM_READ_ONLY;
		}
		// Program-change parameters select presets, a job for a preset API rather than a slider.
		if (info.flags & (ParameterInfo::kIsHidden | ParameterInfo::kIsProgramChange)) {
			p.flags |= PARAM_HIDDEN;
		}
		if (info.flags & ParameterInfo::kIsBypass) {
			p.flags |= PARAM_BYPASS;
		}
		out.push_back(p);
	}
	return out;
}

} // namespace gaph

// --- GUI -------------------------------------------------------------------------------------
//
// VST3's editor is an IPlugView made by the controller: createView("editor"), then
// setFrame (so it can ask for resizes, and on Linux find the run loop), then attached() into
// our window (an X11 id, an HWND or an NSView*: core/native_window.h), then getSize. To close:
// removed(), and release the view.

namespace gaph {

using namespace Steinberg;

bool Vst3Processor::gui_available() {
	if (gui_supported < 0) {
		gui_supported = 0;
		if (controller) {
			IPtr<IPlugView> probe_view = owned(controller->createView(Vst::ViewType::kEditor));
			gui_supported = probe_view && probe_view->isPlatformTypeSupported(plugin_window_api().vst3) == kResultTrue;
		}
	}
	return gui_supported == 1;
}

bool Vst3Processor::gui_open(uint64_t p_parent, double p_scale, uint32_t &r_width, uint32_t &r_height, std::string &r_error) {
	if (view) {
		return true;
	}
	if (!gui_available()) {
		r_error = std::string("this plugin has no editor for this platform (VST3 platform type \"") + plugin_window_api().vst3 + "\")";
		return false;
	}
	// owned(): createView hands over a reference we now own, so IPtr must not add another.
	view = owned(controller->createView(Vst::ViewType::kEditor));
	if (!view) {
		r_error = "createView failed";
		return false;
	}
	view->setFrame(&frame);
	// VST3's GUI scale is an optional extra interface on the view. The SDK allows the call
	// before or after attached(); before means the view comes up at the right size at once.
	// A view without it, or one that refuses, scales itself (or not at all). Not on macOS, where
	// sizes are logical points and the OS scales (the interface is for "systems where plug-ins
	// cannot get this information directly like Microsoft Windows", says its header).
	IPtr<IPlugViewContentScaleSupport> scaling = U::cast<IPlugViewContentScaleSupport>(view);
	if (scaling && plugin_window_api().host_sets_scale) {
		scaling->setContentScaleFactor(IPlugViewContentScaleSupport::ScaleFactor(p_scale > 0 ? p_scale : 1.0));
	}
	if (view->attached(reinterpret_cast<void *>(uintptr_t(p_parent)), plugin_window_api().vst3) != kResultOk) {
		r_error = "IPlugView::attached failed";
		view->setFrame(nullptr);
		view = nullptr;
		return false;
	}
	ViewRect rect;
	r_width = 400;
	r_height = 300;
	if (view->getSize(&rect) == kResultOk && rect.getWidth() > 0 && rect.getHeight() > 0) {
		r_width = uint32_t(rect.getWidth());
		r_height = uint32_t(rect.getHeight());
	}
	return true;
}

void Vst3Processor::gui_close() {
	if (!view) {
		return;
	}
	view->removed();
	view->setFrame(nullptr);
	view = nullptr; // Releases our reference: the view is destroyed.
	resize_pending = false;
}

bool Vst3Processor::gui_take_resize(uint32_t &r_width, uint32_t &r_height) {
	if (!resize_pending) {
		return false;
	}
	resize_pending = false;
	r_width = resize_width;
	r_height = resize_height;
	return true;
}

// The view asks for a new size. The protocol: the host resizes its window, then confirms with
// onSize(). The window is resized on the next frame (gui_take_resize); confirming at once keeps
// the view and our record of it in step.
tresult PLUGIN_API Vst3Processor::PlugFrame::resizeView(IPlugView *p_view, ViewRect *p_size) {
	if (!p_view || !p_size) {
		return kInvalidArgument;
	}
	owner->resize_width = uint32_t(p_size->getWidth());
	owner->resize_height = uint32_t(p_size->getHeight());
	owner->resize_pending = true;
	p_view->onSize(p_size);
	return kResultTrue;
}

#if SMTG_OS_LINUX
tresult PLUGIN_API Vst3Processor::PlugFrame::registerEventHandler(Linux::IEventHandler *p_handler, Linux::FileDescriptor p_fd) {
	if (!p_handler) {
		return kInvalidArgument;
	}
	owner->run_loop.register_fd(p_fd, [p_handler, p_fd] { p_handler->onFDIsSet(p_fd); });
	fd_handlers.emplace_back(p_handler, p_fd);
	return kResultTrue;
}

tresult PLUGIN_API Vst3Processor::PlugFrame::unregisterEventHandler(Linux::IEventHandler *p_handler) {
	for (size_t i = fd_handlers.size(); i-- > 0;) {
		if (fd_handlers[i].first == p_handler) {
			owner->run_loop.unregister_fd(fd_handlers[i].second);
			fd_handlers.erase(fd_handlers.begin() + long(i));
		}
	}
	return kResultTrue;
}

tresult PLUGIN_API Vst3Processor::PlugFrame::registerTimer(Linux::ITimerHandler *p_handler, Linux::TimerInterval p_ms) {
	if (!p_handler) {
		return kInvalidArgument;
	}
	const uint32_t id = owner->run_loop.register_timer(uint32_t(p_ms), [p_handler] { p_handler->onTimer(); });
	timer_handlers.emplace_back(p_handler, id);
	return kResultTrue;
}

tresult PLUGIN_API Vst3Processor::PlugFrame::unregisterTimer(Linux::ITimerHandler *p_handler) {
	for (size_t i = timer_handlers.size(); i-- > 0;) {
		if (timer_handlers[i].first == p_handler) {
			owner->run_loop.unregister_timer(timer_handlers[i].second);
			timer_handlers.erase(timer_handlers.begin() + long(i));
		}
	}
	return kResultTrue;
}
#endif

} // namespace gaph
