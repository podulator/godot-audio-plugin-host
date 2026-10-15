/* clap-gain: about the smallest complete CLAP audio effect. One parameter, stereo in and out.
 *
 * A CLAP plugin is a shared library exporting one symbol, `clap_entry`. Through it the host finds
 * a factory, the factory creates plugin instances, and each instance answers a few calls
 * (init, activate, process...) plus any "extensions" it supports. This one supports two:
 *   clap.audio-ports  so the host knows its inputs and outputs (one stereo pair each way),
 *   clap.params       so the host can see and change its one parameter, "Gain".
 *
 * Build: see examples/README.md. Plain C, CLAP headers only. */
#include <clap/clap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAIN_ID 0

typedef struct {
	clap_plugin_t plugin; /* first member, so a clap_plugin_t* is also a clap_gain_t* */
	double gain;          /* linear, 0..2; 1 = unchanged */
} clap_gain_t;

static const char *const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, NULL };

static const clap_plugin_descriptor_t descriptor = {
	.clap_version = CLAP_VERSION_INIT,
	.id = "org.godot-audio-plugin-host.example.gain",
	.name = "Example Gain",
	.vendor = "godot-audio-plugin-host",
	.version = "1.0.0",
	.description = "The smallest useful CLAP effect: one gain knob",
	.features = features,
};

/* --- clap.params --------------------------------------------------------------------------- */

static uint32_t params_count(const clap_plugin_t *p) {
	(void)p;
	return 1;
}

static bool params_get_info(const clap_plugin_t *p, uint32_t index, clap_param_info_t *info) {
	(void)p;
	if (index != 0) {
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->id = GAIN_ID;
	info->flags = CLAP_PARAM_IS_AUTOMATABLE;
	snprintf(info->name, sizeof(info->name), "Gain");
	info->min_value = 0.0;
	info->max_value = 2.0;
	info->default_value = 1.0;
	return true;
}

static bool params_get_value(const clap_plugin_t *p, clap_id id, double *value) {
	if (id != GAIN_ID) {
		return false;
	}
	*value = ((clap_gain_t *)p)->gain;
	return true;
}

static bool params_value_to_text(const clap_plugin_t *p, clap_id id, double value, char *out, uint32_t size) {
	(void)p;
	if (id != GAIN_ID) {
		return false;
	}
	snprintf(out, size, "%.2f x", value);
	return true;
}

static bool params_text_to_value(const clap_plugin_t *p, clap_id id, const char *text, double *value) {
	(void)p;
	if (id != GAIN_ID) {
		return false;
	}
	*value = atof(text);
	return true;
}

/* Parameter changes arrive as events: in process(), or here when the plugin isn't processing. */
static void apply_event(clap_gain_t *g, const clap_event_header_t *h) {
	if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
		const clap_event_param_value_t *ev = (const clap_event_param_value_t *)h;
		if (ev->param_id == GAIN_ID) {
			g->gain = ev->value;
		}
	}
}

static void params_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) {
	(void)out;
	for (uint32_t i = 0; i < in->size(in); i++) {
		apply_event((clap_gain_t *)p, in->get(in, i));
	}
}

static const clap_plugin_params_t params = {
	params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush,
};

/* --- clap.audio-ports: one stereo input, one stereo output ---------------------------------- */

static uint32_t ports_count(const clap_plugin_t *p, bool is_input) {
	(void)p;
	(void)is_input;
	return 1;
}

static bool ports_get(const clap_plugin_t *p, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
	(void)p;
	(void)is_input;
	if (index != 0) {
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->id = 0;
	snprintf(info->name, sizeof(info->name), "Main");
	info->flags = CLAP_AUDIO_PORT_IS_MAIN;
	info->channel_count = 2;
	info->port_type = CLAP_PORT_STEREO;
	info->in_place_pair = CLAP_INVALID_ID;
	return true;
}

static const clap_plugin_audio_ports_t audio_ports = { ports_count, ports_get };

/* --- process: runs on the audio thread, so no allocation, no locks, no printing -------------
 * Events carry a sample time within the block; each is applied when the loop reaches it, so a
 * gain change lands exactly on its sample. */

static clap_process_status gain_process(const clap_plugin_t *p, const clap_process_t *proc) {
	clap_gain_t *g = (clap_gain_t *)p;
	const uint32_t frames = proc->frames_count;
	const uint32_t n_events = proc->in_events->size(proc->in_events);
	uint32_t next = 0;
	const clap_audio_buffer_t *in = &proc->audio_inputs[0];
	const clap_audio_buffer_t *out = &proc->audio_outputs[0];

	for (uint32_t f = 0; f < frames; f++) {
		while (next < n_events) {
			const clap_event_header_t *h = proc->in_events->get(proc->in_events, next);
			if (h->time > f) {
				break;
			}
			apply_event(g, h);
			next++;
		}
		for (uint32_t c = 0; c < 2; c++) {
			out->data32[c][f] = (float)(in->data32[c][f] * g->gain);
		}
	}
	return CLAP_PROCESS_CONTINUE;
}

/* --- lifecycle --------------------------------------------------------------------------- */

static bool gain_init(const clap_plugin_t *p) {
	(void)p;
	return true;
}

static void gain_destroy(const clap_plugin_t *p) {
	free((clap_gain_t *)p);
}

static bool gain_activate(const clap_plugin_t *p, double rate, uint32_t min_frames, uint32_t max_frames) {
	(void)p, (void)rate, (void)min_frames, (void)max_frames;
	return true; /* Nothing to allocate: a gain has no state that depends on the rate. */
}

static void gain_noop(const clap_plugin_t *p) {
	(void)p;
}

static bool gain_start_processing(const clap_plugin_t *p) {
	(void)p;
	return true;
}

static const void *gain_get_extension(const clap_plugin_t *p, const char *id) {
	(void)p;
	if (!strcmp(id, CLAP_EXT_PARAMS)) {
		return &params;
	}
	if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) {
		return &audio_ports;
	}
	return NULL;
}

/* --- factory and entry: what the host finds through `clap_entry` ---------------------------- */

static uint32_t factory_count(const clap_plugin_factory_t *f) {
	(void)f;
	return 1;
}

static const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *f, uint32_t i) {
	(void)f;
	return i == 0 ? &descriptor : NULL;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
	(void)f, (void)host;
	if (strcmp(id, descriptor.id) != 0) {
		return NULL;
	}
	clap_gain_t *g = (clap_gain_t *)calloc(1, sizeof(clap_gain_t));
	g->gain = 1.0;
	g->plugin = (clap_plugin_t){
		.desc = &descriptor,
		.plugin_data = g,
		.init = gain_init,
		.destroy = gain_destroy,
		.activate = gain_activate,
		.deactivate = gain_noop,
		.start_processing = gain_start_processing,
		.stop_processing = gain_noop,
		.reset = gain_noop,
		.process = gain_process,
		.get_extension = gain_get_extension,
		.on_main_thread = gain_noop,
	};
	return &g->plugin;
}

static const clap_plugin_factory_t factory = { factory_count, factory_descriptor, factory_create };

static bool entry_init(const char *path) {
	(void)path;
	return true;
}

static void entry_deinit(void) {}

static const void *entry_get_factory(const char *id) {
	return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
