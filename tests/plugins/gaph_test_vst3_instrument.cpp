// A deterministic VST3 instrument for the tests, the twin of gaph_test_instrument.c. Built twice,
// as two bundles: gaph_test_vst3_instrument.vst3, and with GAPH_TEST_TRANSPORT defined
// gaph_test_vst3_transport.vst3. One class each, so a test loads it as "the bundle's first class":
// a class id's text differs between Windows (COM byte order) and elsewhere. The two classes:
//
//   GAPH Test VST3 Instrument  each sounding note adds velocity x 0.25; a "Volume" parameter,
//                              which MIDI controller 7 drives through IMidiMapping (VST3's way:
//                              no controller events), scales the sum
//   GAPH Test VST3 Transport   plays back the ProcessContext: left = song position x 0.001 at every
//                              sample (moved on at the tempo while playing), right = tempo x 0.001
//                              while playing, else 0
//
// No oscillator, no audio input: each note start and end is an exact step in the audio.
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

const FUID INSTRUMENT_ID(0x67617068u, 0x74657374u, 0x76737433u, 0x696e7301u);
const FUID TRANSPORT_ID(0x67617068u, 0x74657374u, 0x76737433u, 0x74727001u);
constexpr ParamID VOLUME_ID = 1;
constexpr int MAX_NOTES = 64;

struct Note {
	int16 channel, pitch;
	int32 id;
	float level;
};

class TestInstrument : public SingleComponentEffect, public IMidiMapping {
public:
	explicit TestInstrument(bool p_transport) :
			transport_only(p_transport) {}
	static FUnknown *create_notes(void *) { return static_cast<IAudioProcessor *>(new TestInstrument(false)); }
	static FUnknown *create_transport(void *) { return static_cast<IAudioProcessor *>(new TestInstrument(true)); }

	tresult PLUGIN_API initialize(FUnknown *context) override {
		if (SingleComponentEffect::initialize(context) != kResultOk) {
			return kResultFalse;
		}
		addAudioOutput(STR16("Main"), SpeakerArr::kStereo);
		addEventInput(STR16("Notes"), 16);
		parameters.addParameter(STR16("Volume"), STR16(""), 0, 1.0, ParameterInfo::kCanAutomate, VOLUME_ID);
		return kResultOk;
	}

	tresult PLUGIN_API setBusArrangements(SpeakerArrangement *in, int32 n_in, SpeakerArrangement *out, int32 n_out) override {
		const bool ok = n_in == 0 && n_out == 1 && out[0] == SpeakerArr::kStereo;
		return ok ? SingleComponentEffect::setBusArrangements(in, n_in, out, n_out) : kResultFalse;
	}

	tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultTrue : kResultFalse; }

	tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override {
		rate = setup.sampleRate;
		return SingleComponentEffect::setupProcessing(setup);
	}

	// IMidiMapping: controller 7 (channel volume), on any channel, drives Volume.
	tresult PLUGIN_API getMidiControllerAssignment(int32 bus, int16 channel, CtrlNumber number, ParamID &id) override {
		(void)channel;
		if (bus == 0 && number == ControllerNumbers::kCtrlVolume && !transport_only) {
			id = VOLUME_ID;
			return kResultTrue;
		}
		return kResultFalse;
	}

	tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override {
		if (id == VOLUME_ID) {
			volume = value;
		}
		return SingleComponentEffect::setParamNormalized(id, value);
	}

	OBJ_METHODS(TestInstrument, SingleComponentEffect)
	DEFINE_INTERFACES
	DEF_INTERFACE(IMidiMapping)
	END_DEFINE_INTERFACES(SingleComponentEffect)
	REFCOUNT_METHODS(SingleComponentEffect)

	tresult PLUGIN_API process(ProcessData &data) override {
		if (data.numOutputs < 1 || data.numSamples == 0) {
			return kResultOk;
		}
		IParamValueQueue *volume_queue = nullptr;
		if (IParameterChanges *changes = data.inputParameterChanges) {
			for (int32 i = 0; i < changes->getParameterCount(); i++) {
				IParamValueQueue *q = changes->getParameterData(i);
				if (q && q->getParameterId() == VOLUME_ID) {
					volume_queue = q;
				}
			}
		}
		IEventList *events = data.inputEvents;
		const int32 event_count = events ? events->getEventCount() : 0;
		float **out = data.outputs[0].channelBuffers32;
		const ProcessContext *ctx = data.processContext;
		const bool known = ctx && (ctx->state & ProcessContext::kProjectTimeMusicValid);
		const bool playing = known && (ctx->state & ProcessContext::kPlaying);
		const double tempo = ctx && (ctx->state & ProcessContext::kTempoValid) ? ctx->tempo : 120.0;
		int32 next_event = 0, next_point = 0;
		for (int32 f = 0; f < data.numSamples; f++) {
			Event e;
			while (next_event < event_count && events->getEvent(next_event, e) == kResultOk && e.sampleOffset <= f) {
				handle(e);
				next_event++;
			}
			int32 offset;
			ParamValue v;
			while (volume_queue && next_point < volume_queue->getPointCount() && volume_queue->getPoint(next_point, offset, v) == kResultOk &&
					offset <= f) {
				volume = v;
				next_point++;
			}
			if (transport_only) {
				const double beat = known ? ctx->projectTimeMusic + (playing ? double(f) / rate * tempo / 60.0 : 0.0) : 0.0;
				out[0][f] = known ? float(beat * 0.001) : 0.0f;
				out[1][f] = playing ? float(tempo * 0.001) : 0.0f;
			} else {
				float sum = 0.0f;
				for (int i = 0; i < note_count; i++) {
					sum += notes[i].level;
				}
				out[0][f] = out[1][f] = float(sum * volume);
			}
		}
		data.outputs[0].silenceFlags = 0;
		return kResultOk;
	}

private:
	void handle(const Event &e) {
		if (e.type == Event::kNoteOnEvent && note_count < MAX_NOTES) {
			notes[note_count++] = Note{ e.noteOn.channel, e.noteOn.pitch, e.noteOn.noteId, e.noteOn.velocity * 0.25f };
		} else if (e.type == Event::kNoteOffEvent) {
			int keep = 0;
			for (int i = 0; i < note_count; i++) {
				const Note &n = notes[i];
				const bool match = (e.noteOff.noteId >= 0 && n.id >= 0) ? n.id == e.noteOff.noteId : (n.channel == e.noteOff.channel && n.pitch == e.noteOff.pitch);
				if (!match) {
					notes[keep++] = n;
				}
			}
			note_count = keep;
		}
	}

	bool transport_only;
	double rate = 44100.0;
	double volume = 1.0;
	Note notes[MAX_NOTES];
	int note_count = 0;
};

} // namespace

BEGIN_FACTORY_DEF("godot-audio-plugin-host", "", "")
#ifndef GAPH_TEST_TRANSPORT
DEF_CLASS2(INLINE_UID_FROM_FUID(INSTRUMENT_ID), PClassInfo::kManyInstances, kVstAudioEffectClass, "GAPH Test VST3 Instrument",
		Vst::kDistributable, Vst::PlugType::kInstrumentSynth, "1.0.0", kVstVersionString, TestInstrument::create_notes)
#else
DEF_CLASS2(INLINE_UID_FROM_FUID(TRANSPORT_ID), PClassInfo::kManyInstances, kVstAudioEffectClass, "GAPH Test VST3 Transport",
		Vst::kDistributable, Vst::PlugType::kInstrumentSynth, "1.0.0", kVstVersionString, TestInstrument::create_transport)
#endif
END_FACTORY
