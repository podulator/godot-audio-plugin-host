// vst3-gain: about the smallest complete VST 3 audio effect. One parameter, stereo in and out.
//
// A VST 3 plugin is normally two objects, a processor (the sound) and an edit controller (the
// parameters and GUI), which may not even share memory. The SDK's SingleComponentEffect folds
// both into one class, which is all a simple effect needs and keeps this example short.
//
// VST 3 exchanges every parameter as a normalized value, 0..1. Here 0..1 maps to a gain of 0..2,
// so the default 0.5 means "unchanged".
//
// Build: see examples/README.md. VST is a registered trademark of Steinberg Media Technologies GmbH.

#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <cstdio>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

constexpr ParamID GAIN_ID = 0;

// FUID: the class id hosts know the plugin by. Any 128-bit value, but it must never change.
const FUID GAIN_CLASS_ID(0x6761706Bu, 0x6761696Eu, 0x6578616Du, 0x706C6501u);

class Gain : public SingleComponentEffect {
public:
	static FUnknown *create(void *) { return static_cast<IAudioProcessor *>(new Gain); }

	tresult PLUGIN_API initialize(FUnknown *context) override {
		if (SingleComponentEffect::initialize(context) != kResultOk) {
			return kResultFalse;
		}
		addAudioInput(STR16("Main"), SpeakerArr::kStereo);
		addAudioOutput(STR16("Main"), SpeakerArr::kStereo);
		// The controller side: the parameter's name, its units, how many steps (0 = continuous),
		// its default (normalized), and its flags.
		parameters.addParameter(STR16("Gain"), STR16("x"), 0, 0.5, ParameterInfo::kCanAutomate, GAIN_ID);
		return kResultOk;
	}

	tresult PLUGIN_API setBusArrangements(SpeakerArrangement *in, int32 n_in, SpeakerArrangement *out, int32 n_out) override {
		// Stereo only: refuse anything else, and the host keeps our default.
		if (n_in == 1 && n_out == 1 && in[0] == SpeakerArr::kStereo && out[0] == SpeakerArr::kStereo) {
			return SingleComponentEffect::setBusArrangements(in, n_in, out, n_out);
		}
		return kResultFalse;
	}

	tresult PLUGIN_API canProcessSampleSize(int32 size) override {
		return size == kSample32 ? kResultTrue : kResultFalse;
	}

	// Audio thread: no allocation, no locks, no printing. Each parameter arrives as a queue of
	// (sample offset, value) points; the gain changes on exactly the sample each point names.
	tresult PLUGIN_API process(ProcessData &data) override {
		IParamValueQueue *queue = nullptr;
		if (IParameterChanges *changes = data.inputParameterChanges) {
			for (int32 i = 0; i < changes->getParameterCount(); i++) {
				IParamValueQueue *q = changes->getParameterData(i);
				if (q && q->getParameterId() == GAIN_ID) {
					queue = q;
				}
			}
		}
		if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples == 0) {
			return kResultOk;
		}
		float **in = data.inputs[0].channelBuffers32;
		float **out = data.outputs[0].channelBuffers32;
		int32 point = 0;
		int32 next_offset = -1;
		ParamValue next_value = 0;
		if (queue && queue->getPointCount() > 0) {
			queue->getPoint(0, next_offset, next_value);
		}
		for (int32 f = 0; f < data.numSamples; f++) {
			while (next_offset >= 0 && next_offset <= f) {
				gain = next_value * 2.0;
				setParamNormalized(GAIN_ID, next_value); // keep the controller side in step
				next_offset = -1;
				if (++point < queue->getPointCount()) {
					queue->getPoint(point, next_offset, next_value);
				}
			}
			for (int32 c = 0; c < 2; c++) {
				out[c][f] = float(in[c][f] * gain);
			}
		}
		data.outputs[0].silenceFlags = 0;
		return kResultOk;
	}

	// The controller's text for a value: "1.00 x" (the units "x" are added by the host).
	tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue value, String128 string) override {
		if (id != GAIN_ID) {
			return SingleComponentEffect::getParamStringByValue(id, value, string);
		}
		char text[32];
		std::snprintf(text, sizeof(text), "%.2f", value * 2.0);
		for (int i = 0; i < 31 && text[i]; i++) {
			string[i] = char16(text[i]);
			string[i + 1] = 0;
		}
		return kResultOk;
	}

private:
	double gain = 1.0;
};

} // namespace

// The factory hosts call GetPluginFactory() for. The macros come from pluginfactory.h.
BEGIN_FACTORY_DEF("godot-audio-plugin-host", "", "")
DEF_CLASS2(INLINE_UID_FROM_FUID(GAIN_CLASS_ID), PClassInfo::kManyInstances, kVstAudioEffectClass, "Example Gain",
		Vst::kDistributable, Vst::PlugType::kFxTools, "1.0.0", kVstVersionString, Gain::create)
END_FACTORY
