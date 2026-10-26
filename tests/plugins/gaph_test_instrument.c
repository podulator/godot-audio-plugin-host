/* A deterministic CLAP instrument for the tests. Plain C, one file, two plugins:
 *
 *   org.godot-audio-plugin-host.test-instrument       takes CLAP note events (and MIDI for controllers)
 *   org.godot-audio-plugin-host.test-instrument-midi  takes MIDI 1.0 only
 *   org.godot-audio-plugin-host.test-transport        plays back the host's transport
 *
 * No oscillator: every sounding note adds a constant, velocity (0..1) x 0.25, to both outputs, and
 * channel volume (CC 7, 0..127) scales the sum. So a note's start and end show as exact steps in
 * the audio, on the very sample its event names, and the level says how many notes sound and how
 * loud. A choke, or All Sound Off (CC 120), silences notes like a note-off. No audio input.
 * One parameter, Master (id 1, 0..1, default 1), scales everything; it is the plugin's state too
 * (a little-endian double), so a saved scene brings it back.
 *
 * The transport plugin ignores notes: its left output is the song position in beats x 0.001 at
 * every sample (from proc->transport and transport events, moved on at the tempo while playing),
 * its right output the tempo x 0.001 while playing, else 0. So the audio shows what the plugin was
 * told, sample by sample: a loop jump is a step down on the very sample it happens.
 */
#include <clap/clap.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NOTES 64

typedef struct {
	int16_t channel, key;
	int32_t note_id;
	float level;
} Note;

typedef struct {
	clap_plugin_t plugin;
	const clap_host_t *host;
	bool midi_only;
	bool transport_only;
	double rate;
	/* The transport as last told: position `beat` at sample `at` of this block (may be negative
	 * when carried over from an earlier block), tempo, playing. */
	bool known, playing;
	double beat, tempo;
	int64_t at;
	Note notes[MAX_NOTES];
	int note_count;
	float volume[16]; /* CC 7 per channel, 0..1 */
	double master;    /* parameter 1, and the state */
} Instrument;

static const char *const FEATURES[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_STEREO, NULL };

static const clap_plugin_descriptor_t DESC_CLAP = {
	CLAP_VERSION_INIT, "org.godot-audio-plugin-host.test-instrument", "GAPH Test Instrument", "Godot Audio Plugin Host", "", "", "", "1.0.0",
	"Each note adds velocity x 0.25: notes as CLAP events", FEATURES,
};
static const clap_plugin_descriptor_t DESC_TRANSPORT = {
	CLAP_VERSION_INIT, "org.godot-audio-plugin-host.test-transport", "GAPH Test Transport", "Godot Audio Plugin Host", "", "", "", "1.0.0",
	"Plays back the host's transport: song position and tempo as levels", FEATURES,
};
static const clap_plugin_descriptor_t DESC_MIDI = {
	CLAP_VERSION_INIT, "org.godot-audio-plugin-host.test-instrument-midi", "GAPH Test Instrument (MIDI)", "Godot Audio Plugin Host", "", "", "", "1.0.0",
	"Each note adds velocity x 0.25: notes as MIDI 1.0", FEATURES,
};

static Instrument *self_of(const clap_plugin_t *p) {
	return (Instrument *)p->plugin_data;
}

/* --- notes ----------------------------------------------------------------------------------- */

static void note_start(Instrument *s, int16_t channel, int16_t key, int32_t note_id, double velocity) {
	if (s->note_count < MAX_NOTES) {
		Note *n = &s->notes[s->note_count++];
		n->channel = channel;
		n->key = key;
		n->note_id = note_id;
		n->level = (float)(velocity * 0.25);
	}
}

/* Ends the matching notes: by id when both sides have one, else by channel and key; key -1 or
 * channel -1 match all (CLAP's wildcards, and All Sound Off). */
static void note_end(Instrument *s, int16_t channel, int16_t key, int32_t note_id) {
	int keep = 0;
	for (int i = 0; i < s->note_count; i++) {
		const Note *n = &s->notes[i];
		bool match;
		if (note_id >= 0 && n->note_id >= 0) {
			match = n->note_id == note_id;
		} else {
			match = (channel < 0 || n->channel == channel) && (key < 0 || n->key == key);
		}
		if (!match) {
			s->notes[keep++] = *n;
		}
	}
	s->note_count = keep;
}

static float level_now(const Instrument *s) {
	float sum = 0.0f;
	for (int i = 0; i < s->note_count; i++) {
		sum += s->notes[i].level * s->volume[s->notes[i].channel & 15];
	}
	return (float)(sum * s->master);
}

static void handle_midi1(Instrument *s, const uint8_t data[3]) {
	const uint8_t status = data[0] & 0xF0;
	const int16_t channel = (int16_t)(data[0] & 0x0F);
	if (status == 0x90 && data[2] > 0) {
		note_start(s, channel, data[1], -1, data[2] / 127.0);
	} else if (status == 0x80 || (status == 0x90 && data[2] == 0)) {
		note_end(s, channel, data[1], -1);
	} else if (status == 0xB0 && data[1] == 7) {
		s->volume[channel] = data[2] / 127.0f;
	} else if (status == 0xB0 && data[1] == 120) {
		note_end(s, channel, -1, -1); /* All Sound Off */
	}
}

static void take_transport(Instrument *s, const clap_event_transport_t *t, int64_t at) {
	s->known = true;
	s->playing = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
	s->beat = (double)t->song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
	s->tempo = (t->flags & CLAP_TRANSPORT_HAS_TEMPO) ? t->tempo : 120.0;
	s->at = at;
}

static void handle_event(Instrument *s, const clap_event_header_t *h) {
	if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) {
		return;
	}
	if (h->type == CLAP_EVENT_PARAM_VALUE) {
		const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
		if (e->param_id == 1) {
			s->master = e->value;
		}
		return;
	}
	if (h->type == CLAP_EVENT_TRANSPORT) {
		take_transport(s, (const clap_event_transport_t *)h, (int64_t)h->time);
		return;
	}
	if (s->transport_only) {
		return;
	}
	if (!s->midi_only && (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF || h->type == CLAP_EVENT_NOTE_CHOKE)) {
		const clap_event_note_t *e = (const clap_event_note_t *)h;
		if (h->type == CLAP_EVENT_NOTE_ON) {
			note_start(s, e->channel, e->key, e->note_id, e->velocity);
		} else {
			note_end(s, e->channel, e->key, e->note_id);
		}
	} else if (h->type == CLAP_EVENT_MIDI) {
		handle_midi1(s, ((const clap_event_midi_t *)h)->data);
	}
}

/* --- the plugin ------------------------------------------------------------------------------ */

static bool p_init(const clap_plugin_t *p) {
	Instrument *s = self_of(p);
	for (int c = 0; c < 16; c++) {
		s->volume[c] = 1.0f;
	}
	s->master = 1.0;
	return true;
}

static void p_destroy(const clap_plugin_t *p) {
	free(self_of(p));
}

static bool p_activate(const clap_plugin_t *p, double rate, uint32_t min_frames, uint32_t max_frames) {
	(void)min_frames, (void)max_frames;
	self_of(p)->rate = rate;
	return true;
}

static void p_deactivate(const clap_plugin_t *p) {
	(void)p;
}

static bool p_start_processing(const clap_plugin_t *p) {
	(void)p;
	return true;
}

static void p_stop_processing(const clap_plugin_t *p) {
	(void)p;
}

static void p_reset(const clap_plugin_t *p) {
	self_of(p)->note_count = 0;
}

static clap_process_status p_process(const clap_plugin_t *p, const clap_process_t *proc) {
	Instrument *s = self_of(p);
	const uint32_t frames = proc->frames_count;
	const uint32_t count = proc->in_events->size(proc->in_events);
	if (proc->audio_outputs_count < 1 || proc->audio_outputs[0].channel_count < 2) {
		return CLAP_PROCESS_ERROR;
	}
	float *left = proc->audio_outputs[0].data32[0];
	float *right = proc->audio_outputs[0].data32[1];
	uint32_t next = 0;
	if (s->transport_only && proc->transport) {
		take_transport(s, proc->transport, 0);
	}
	for (uint32_t f = 0; f < frames; f++) {
		/* Every event due at this sample first, so its change shows on exactly this sample. */
		while (next < count) {
			const clap_event_header_t *h = proc->in_events->get(proc->in_events, next);
			if (h->time > f) {
				break;
			}
			handle_event(s, h);
			next++;
		}
		if (s->transport_only) {
			const double beat = s->playing ? s->beat + (double)((int64_t)f - s->at) / s->rate * s->tempo / 60.0 : s->beat;
			left[f] = s->known ? (float)(beat * 0.001) : 0.0f;
			right[f] = s->known && s->playing ? (float)(s->tempo * 0.001) : 0.0f;
		} else {
			const float v = level_now(s);
			left[f] = v;
			right[f] = v;
		}
	}
	s->at -= (int64_t)frames; /* the same moment, counted from the next block's first sample */
	while (next < count) { /* any beyond the block's end (a host should not send them) */
		handle_event(s, proc->in_events->get(proc->in_events, next++));
	}
	return CLAP_PROCESS_CONTINUE;
}

/* --- extensions: one stereo output, one note input --------------------------------------------- */

static uint32_t ports_count(const clap_plugin_t *p, bool is_input) {
	(void)p;
	return is_input ? 0 : 1;
}

static bool ports_get(const clap_plugin_t *p, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
	(void)p;
	if (is_input || index != 0) {
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->id = 0;
	strcpy(info->name, "Out");
	info->flags = CLAP_AUDIO_PORT_IS_MAIN;
	info->channel_count = 2;
	info->port_type = CLAP_PORT_STEREO;
	info->in_place_pair = CLAP_INVALID_ID;
	return true;
}

static const clap_plugin_audio_ports_t AUDIO_PORTS = { ports_count, ports_get };

static uint32_t notes_count(const clap_plugin_t *p, bool is_input) {
	(void)p;
	return is_input ? 1 : 0;
}

static bool notes_get(const clap_plugin_t *p, uint32_t index, bool is_input, clap_note_port_info_t *info) {
	if (!is_input || index != 0) {
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->id = 0;
	strcpy(info->name, "Notes");
	if (self_of(p)->midi_only) {
		info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
		info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
	} else {
		info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
		info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
	}
	return true;
}

static const clap_plugin_note_ports_t NOTE_PORTS = { notes_count, notes_get };

/* params: Master only. Also flushed while not processing (the host's probe, setting a value). */
static uint32_t params_count(const clap_plugin_t *p) {
	return self_of(p)->transport_only ? 0 : 1;
}

static bool params_info(const clap_plugin_t *p, uint32_t index, clap_param_info_t *info) {
	if (self_of(p)->transport_only || index != 0) {
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->id = 1;
	info->flags = CLAP_PARAM_IS_AUTOMATABLE;
	strcpy(info->name, "Master");
	info->min_value = 0.0;
	info->max_value = 1.0;
	info->default_value = 1.0;
	return true;
}

static bool params_value(const clap_plugin_t *p, clap_id id, double *value) {
	if (id != 1) {
		return false;
	}
	*value = self_of(p)->master;
	return true;
}

static bool params_to_text(const clap_plugin_t *p, clap_id id, double value, char *out, uint32_t size) {
	(void)p, (void)id;
	snprintf(out, size, "%.2f", value);
	return true;
}

static bool params_from_text(const clap_plugin_t *p, clap_id id, const char *text, double *value) {
	(void)p, (void)id;
	*value = atof(text);
	return true;
}

static void params_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) {
	(void)out;
	for (uint32_t i = 0; i < in->size(in); i++) {
		handle_event(self_of(p), in->get(in, i));
	}
}

static const clap_plugin_params_t PARAMS = { params_count, params_info, params_value, params_to_text, params_from_text, params_flush };

/* state: Master as a little-endian double (8 bytes; the test machines are all little-endian). */
static bool state_save(const clap_plugin_t *p, const clap_ostream_t *stream) {
	const double v = self_of(p)->master;
	return stream->write(stream, &v, sizeof(v)) == (int64_t)sizeof(v);
}

static bool state_load(const clap_plugin_t *p, const clap_istream_t *stream) {
	double v = 0.0;
	if (stream->read(stream, &v, sizeof(v)) != (int64_t)sizeof(v)) {
		return false;
	}
	self_of(p)->master = v;
	return true;
}

static const clap_plugin_state_t STATE = { state_save, state_load };

static const void *p_get_extension(const clap_plugin_t *p, const char *id) {
	(void)p;
	if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) {
		return &AUDIO_PORTS;
	}
	if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) {
		return &NOTE_PORTS;
	}
	if (!strcmp(id, CLAP_EXT_PARAMS)) {
		return &PARAMS;
	}
	if (!strcmp(id, CLAP_EXT_STATE)) {
		return &STATE;
	}
	return NULL;
}

static void p_on_main_thread(const clap_plugin_t *p) {
	(void)p;
}

/* --- factory and entry ---------------------------------------------------------------------- */

static uint32_t factory_count(const clap_plugin_factory_t *f) {
	(void)f;
	return 3;
}

static const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *f, uint32_t index) {
	(void)f;
	return index == 0 ? &DESC_CLAP : (index == 1 ? &DESC_MIDI : (index == 2 ? &DESC_TRANSPORT : NULL));
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
	(void)f;
	const bool midi_only = !strcmp(id, DESC_MIDI.id);
	const bool transport_only = !strcmp(id, DESC_TRANSPORT.id);
	if (!midi_only && !transport_only && strcmp(id, DESC_CLAP.id)) {
		return NULL;
	}
	Instrument *s = (Instrument *)calloc(1, sizeof(Instrument));
	if (!s) {
		return NULL;
	}
	s->host = host;
	s->midi_only = midi_only;
	s->transport_only = transport_only;
	s->rate = 44100.0;
	s->plugin.desc = midi_only ? &DESC_MIDI : (transport_only ? &DESC_TRANSPORT : &DESC_CLAP);
	s->plugin.plugin_data = s;
	s->plugin.init = p_init;
	s->plugin.destroy = p_destroy;
	s->plugin.activate = p_activate;
	s->plugin.deactivate = p_deactivate;
	s->plugin.start_processing = p_start_processing;
	s->plugin.stop_processing = p_stop_processing;
	s->plugin.reset = p_reset;
	s->plugin.process = p_process;
	s->plugin.get_extension = p_get_extension;
	s->plugin.on_main_thread = p_on_main_thread;
	return &s->plugin;
}

static const clap_plugin_factory_t FACTORY = { factory_count, factory_descriptor, factory_create };

static bool entry_init(const char *path) {
	(void)path;
	return true;
}

static void entry_deinit(void) {}

static const void *entry_get_factory(const char *id) {
	return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &FACTORY : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
