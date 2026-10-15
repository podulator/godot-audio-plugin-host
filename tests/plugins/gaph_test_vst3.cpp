// gaph test plugin, VST3: the VST3 twin of gaph_test_plugin.c, a tiny, fully deterministic effect
// for testing the host on every platform without third-party plugins. Built by examples/SConstruct
// with test_plugin=yes.
//
//   buses:  stereo main in, stereo main out, stereo sidechain in ("Sidechain", an aux bus)
//   params: 1 "Gain"           normalised 0..1 = gain 0..2, default 0.5   out = main * gain
//           2 "Sidechain Mix"  0..1, default 0                                  + sidechain * mix
//           so the defaults are an exact null test
//   changes: applied at their own sample offset (sample-accurate)
//   state:  the two values, as two little-endian doubles (Gain as 0..2)
//
//   gui: a blank editor, embedded: a child HWND ("HWND") or a subview NSView ("NSView"),
//        300 x 200 times the scale from IPlugViewContentScaleSupport. On macOS a host must not
//        set one (sizes are points there); if it does, the view answers 999 x 999.
//        With GAPH_TEST_GUI_REQUEST_RESIZE=WxH set, attaching it asks the host for that size
//        (IPlugFrame::resizeView), to test the host following a plugin's resize.
//   No GUI on Linux, where the host's X11 path is tested with real plugins.
//
// VST is a registered trademark of Steinberg Media Technologies GmbH.

#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "base/source/fstreamer.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define GAPH_TEST_GUI 1
#elif defined(__APPLE__)
#include <CoreGraphics/CGGeometry.h>
#include <objc/message.h>
#include <objc/runtime.h>
#define GAPH_TEST_GUI 1
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

const FUID TEST_CLASS_ID(0x67617068u, 0x74657374u, 0x76737433u, 0x67756901u);

#if defined(GAPH_TEST_GUI)
#if defined(_WIN32)
const char *const PLATFORM = kPlatformTypeHWND;
void *child_create(void *parent, int w, int h) {
	return IsWindow((HWND)parent) ? CreateWindowExW(0, L"STATIC", L"GAPH Test VST3", WS_CHILD | WS_VISIBLE, 0, 0, w, h, (HWND)parent, nullptr, nullptr, nullptr) : nullptr;
}
void child_resize(void *c, int w, int h) {
	SetWindowPos((HWND)c, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
void child_destroy(void *c) {
	DestroyWindow((HWND)c);
}
#else
const char *const PLATFORM = kPlatformTypeNSView;
void *child_create(void *parent, int w, int h) {
	if (!parent) {
		return nullptr;
	}
	id v = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSView"), sel_registerName("alloc"));
	v = ((id(*)(id, SEL, CGRect))objc_msgSend)(v, sel_registerName("initWithFrame:"), CGRectMake(0, 0, w, h));
	((void (*)(id, SEL, id))objc_msgSend)((id)parent, sel_registerName("addSubview:"), v);
	return v;
}
void child_resize(void *c, int w, int h) {
	((void (*)(id, SEL, CGSize))objc_msgSend)((id)c, sel_registerName("setFrameSize:"), CGSizeMake(w, h));
}
void child_destroy(void *c) {
	((void (*)(id, SEL))objc_msgSend)((id)c, sel_registerName("removeFromSuperview"));
	((void (*)(id, SEL))objc_msgSend)((id)c, sel_registerName("release"));
}
#endif

const int GUI_W = 300, GUI_H = 200;

// CPluginView (the SDK's) keeps the rect, the parent handle and the IPlugFrame; this adds the
// child, the scale and the resize request. A view is used from the main thread only.
class TestView : public CPluginView, public IPlugViewContentScaleSupport {
public:
	TestView() {
		ViewRect r(0, 0, GUI_W, GUI_H);
		setRect(r);
	}

	tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
		return type && std::strcmp(type, PLATFORM) == 0 ? kResultTrue : kResultFalse;
	}

	tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor) override {
#if defined(__APPLE__)
		(void)factor;
		ViewRect r(0, 0, 999, 999); // a host called it on macOS: answer visibly wrong
		setRect(r);
		return kResultFalse;
#else
		const float s = factor > 0 ? factor : 1.0f;
		ViewRect r(0, 0, int32(GUI_W * s + 0.5f), int32(GUI_H * s + 0.5f));
		setRect(r);
		return kResultTrue;
#endif
	}

	void attachedToParent() override {
		child = child_create(systemWindow, rect.getWidth(), rect.getHeight());
		unsigned w = 0, h = 0;
		const char *want = std::getenv("GAPH_TEST_GUI_REQUEST_RESIZE");
		if (child && plugFrame && want && std::sscanf(want, "%ux%u", &w, &h) == 2 && w && h) {
			ViewRect r(0, 0, int32(w), int32(h));
			plugFrame->resizeView(this, &r); // the host answers by calling onSize
		}
	}

	void removedFromParent() override {
		if (child) {
			child_destroy(child);
			child = nullptr;
		}
	}

	tresult PLUGIN_API onSize(ViewRect *size) override {
		CPluginView::onSize(size);
		if (child && size) {
			child_resize(child, size->getWidth(), size->getHeight());
		}
		return kResultOk;
	}

	OBJ_METHODS(TestView, CPluginView)
	DEFINE_INTERFACES
		DEF_INTERFACE(IPlugViewContentScaleSupport)
	END_DEFINE_INTERFACES(CPluginView)
	REFCOUNT_METHODS(CPluginView)

private:
	void *child = nullptr;
};
#endif

// out = main * gain + sidechain * mix, applied on each change's own sample. VST3 exchanges every
// parameter normalised (0..1): Gain's 0..1 means 0..2, so 0.5 is unity and the defaults (0.5, 0)
// are an exact null test, as with the CLAP test plugin.
constexpr ParamID GAIN_ID = 1, MIX_ID = 2;

class TestEffect : public SingleComponentEffect {
public:
	static FUnknown *create(void *) { return static_cast<IAudioProcessor *>(new TestEffect); }

	tresult PLUGIN_API initialize(FUnknown *context) override {
		if (SingleComponentEffect::initialize(context) != kResultOk) {
			return kResultFalse;
		}
		addAudioInput(STR16("Main"), SpeakerArr::kStereo);
		addAudioInput(STR16("Sidechain"), SpeakerArr::kStereo, kAux, 0);
		addAudioOutput(STR16("Main"), SpeakerArr::kStereo);
		parameters.addParameter(STR16("Gain"), STR16("x"), 0, 0.5, ParameterInfo::kCanAutomate, GAIN_ID);
		parameters.addParameter(STR16("Sidechain Mix"), STR16(""), 0, 0.0, ParameterInfo::kCanAutomate, MIX_ID);
		return kResultOk;
	}

	tresult PLUGIN_API setBusArrangements(SpeakerArrangement *in, int32 n_in, SpeakerArrangement *out, int32 n_out) override {
		const bool stereo = n_in == 2 && n_out == 1 && in[0] == SpeakerArr::kStereo && in[1] == SpeakerArr::kStereo && out[0] == SpeakerArr::kStereo;
		return stereo ? SingleComponentEffect::setBusArrangements(in, n_in, out, n_out) : kResultFalse;
	}

	tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultTrue : kResultFalse; }

	tresult PLUGIN_API process(ProcessData &data) override {
		// Each parameter's queue of (sample offset, value) points, applied as the loop reaches them.
		IParamValueQueue *queues[2] = { nullptr, nullptr };
		if (IParameterChanges *changes = data.inputParameterChanges) {
			for (int32 i = 0; i < changes->getParameterCount(); i++) {
				IParamValueQueue *q = changes->getParameterData(i);
				if (q && (q->getParameterId() == GAIN_ID || q->getParameterId() == MIX_ID)) {
					queues[q->getParameterId() == GAIN_ID ? 0 : 1] = q;
				}
			}
		}
		if (data.numOutputs < 1 || data.numSamples == 0 || data.numInputs < 1) {
			return kResultOk;
		}
		float **in = data.inputs[0].channelBuffers32;
		float **side = data.numInputs > 1 ? data.inputs[1].channelBuffers32 : nullptr;
		float **out = data.outputs[0].channelBuffers32;
		int32 next[2] = { 0, 0 };
		for (int32 f = 0; f < data.numSamples; f++) {
			for (int k = 0; k < 2; k++) {
				IParamValueQueue *q = queues[k];
				int32 offset;
				ParamValue v;
				while (q && next[k] < q->getPointCount() && q->getPoint(next[k], offset, v) == kResultOk && offset <= f) {
					(k == 0 ? gain : mix) = k == 0 ? v * 2.0 : v;
					next[k]++;
				}
			}
			for (int32 c = 0; c < 2; c++) {
				out[c][f] = float(in[c][f] * gain + (side ? side[c][f] * mix : 0.0));
			}
		}
		data.outputs[0].silenceFlags = 0;
		return kResultOk;
	}

	// State: the two values as two little-endian doubles (Gain as the real 0..2, as in CLAP).
	tresult PLUGIN_API getState(IBStream *state) override {
		IBStreamer w(state, kLittleEndian);
		return w.writeDouble(gain) && w.writeDouble(mix) ? kResultOk : kResultFalse;
	}

	tresult PLUGIN_API setState(IBStream *state) override {
		IBStreamer r(state, kLittleEndian);
		double g = 0, m = 0;
		if (!r.readDouble(g) || !r.readDouble(m)) {
			return kResultFalse;
		}
		gain = g;
		mix = m;
		// The controller side shows the restored values (one object here: SingleComponentEffect).
		setParamNormalized(GAIN_ID, gain / 2.0);
		setParamNormalized(MIX_ID, mix);
		return kResultOk;
	}

	// Kept in step when the host sets a value on the controller side (the inspector).
	tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override {
		if (id == GAIN_ID) {
			gain = value * 2.0;
		} else if (id == MIX_ID) {
			mix = value;
		}
		return SingleComponentEffect::setParamNormalized(id, value);
	}

	tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue value, String128 string) override {
		char text[32];
		if (id == GAIN_ID) {
			std::snprintf(text, sizeof(text), "%.3f", value * 2.0); // the host joins the units, "x"
		} else if (id == MIX_ID) {
			std::snprintf(text, sizeof(text), "%.0f%%", value * 100.0);
		} else {
			return SingleComponentEffect::getParamStringByValue(id, value, string);
		}
		int i = 0;
		for (; i < 31 && text[i]; i++) {
			string[i] = char16(text[i]);
		}
		string[i] = 0;
		return kResultOk;
	}

	IPlugView *PLUGIN_API createView(FIDString name) override {
#if defined(GAPH_TEST_GUI)
		if (name && std::strcmp(name, ViewType::kEditor) == 0) {
			return new TestView;
		}
#else
		(void)name;
#endif
		return nullptr;
	}

private:
	double gain = 1.0; // the real values: written on the audio thread, or by setState/setParamNormalized
	double mix = 0.0;
};

} // namespace

BEGIN_FACTORY_DEF("godot-audio-plugin-host", "", "")
DEF_CLASS2(INLINE_UID_FROM_FUID(TEST_CLASS_ID), PClassInfo::kManyInstances, kVstAudioEffectClass, "GAPH Test VST3",
		Vst::kDistributable, Vst::PlugType::kFxTools, "1.0.0", kVstVersionString, TestEffect::create)
END_FACTORY
