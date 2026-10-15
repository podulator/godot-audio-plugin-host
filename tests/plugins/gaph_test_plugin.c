/* gaph test plugin: a tiny, fully deterministic CLAP effect for testing the host on every
 * platform, in CI too, where no third-party plugins are installed. Plain C, CLAP headers only.
 *
 *   ports:  stereo main in, stereo main out, stereo sidechain in ("Sidechain")
 *   params: 1 "Gain"           0..2, default 1    out = main * gain + sidechain * mix
 *           2 "Sidechain Mix"  0..1, default 0    so the defaults are an exact null test
 *   events: applied at their own sample (sample-accurate), so timing is testable exactly
 *   state:  the two values, as two little-endian doubles
 *
 * Every result is exact arithmetic on the input, so tests can check to the sample.
 *
 *   gui:    on Windows and macOS, a blank "GUI" embedded in the host's window: a child window
 *           (CLAP "win32") or a subview (CLAP "cocoa"), 300 x 200 times the scale the host sets.
 *           A test checks the host's window API, scale and sizing by looking at it inside the
 *           host's window. With GAPH_TEST_GUI_REQUEST_RESIZE=WxH set, showing it asks the host
 *           for that size (clap.gui's request_resize), to test the host following a plugin's
 *           resize. No GUI on Linux (the host's X11 path is tested with real plugins). */
#include <clap/clap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PARAM_GAIN = 1, PARAM_MIX = 2 };

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib, "user32.lib") /* MSVC; MinGW links -luser32 instead */
#endif
#define GAPH_TEST_GUI 1
#elif defined(__APPLE__)
/* Objective-C from plain C: classes looked up by name, messages sent through objc_msgSend cast to
 * the method's real signature. Links libobjc; AppKit is already loaded by any GUI host. */
#include <CoreGraphics/CGGeometry.h>
#include <objc/message.h>
#include <objc/runtime.h>
#define GAPH_TEST_GUI 1
#endif

typedef struct {
	clap_plugin_t plugin;
	const clap_host_t *host;
	double gain;
	double mix;
#ifdef GAPH_TEST_GUI
	double gui_scale;
	void *gui_child; /* the "GUI": an HWND child window, or an NSView* subview */
#endif
} test_plugin_t;

static const char *const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, NULL };

static const clap_plugin_descriptor_t descriptor = {
	CLAP_VERSION_INIT,
	"org.godot-audio-plugin-host.test",
	"GAPH Test Plugin",
	"godot-audio-plugin-host",
	"",
	"",
	"",
	"1.0.0",
	"Deterministic gain + sidechain mix, for testing hosts",
	features,
};

/* --- params ------------------------------------------------------------------------------ */

static uint32_t params_count(const clap_plugin_t *p) {
	(void)p;
	return 2;
}

static bool params_get_info(const clap_plugin_t *p, uint32_t index, clap_param_info_t *info) {
	(void)p;
	if (index > 1) {
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->id = index == 0 ? PARAM_GAIN : PARAM_MIX;
	info->flags = CLAP_PARAM_IS_AUTOMATABLE;
	snprintf(info->name, sizeof(info->name), "%s", index == 0 ? "Gain" : "Sidechain Mix");
	info->min_value = 0.0;
	info->max_value = index == 0 ? 2.0 : 1.0;
	info->default_value = index == 0 ? 1.0 : 0.0;
	return true;
}

static double *param_slot(test_plugin_t *t, clap_id id) {
	return id == PARAM_GAIN ? &t->gain : (id == PARAM_MIX ? &t->mix : NULL);
}

static bool params_get_value(const clap_plugin_t *p, clap_id id, double *value) {
	double *slot = param_slot((test_plugin_t *)p->plugin_data, id);
	if (!slot) {
		return false;
	}
	*value = *slot;
	return true;
}

static bool params_value_to_text(const clap_plugin_t *p, clap_id id, double value, char *out, uint32_t size) {
	(void)p;
	if (id == PARAM_GAIN) {
		snprintf(out, size, "%.3f x", value);
	} else if (id == PARAM_MIX) {
		snprintf(out, size, "%.0f%%", value * 100.0);
	} else {
		return false;
	}
	return true;
}

static bool params_text_to_value(const clap_plugin_t *p, clap_id id, const char *text, double *value) {
	(void)p;
	if (!param_slot((test_plugin_t *)p->plugin_data, id)) {
		return false;
	}
	*value = atof(text) / (id == PARAM_MIX ? 100.0 : 1.0);
	return true;
}

/* One parameter event: the core event space's PARAM_VALUE is the only one this plugin reads. */
static void apply_event(test_plugin_t *t, const clap_event_header_t *h) {
	if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) {
		return;
	}
	const clap_event_param_value_t *ev = (const clap_event_param_value_t *)h;
	double *slot = param_slot(t, ev->param_id);
	if (slot) {
		*slot = ev->value;
	}
}

static void params_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) {
	(void)out;
	test_plugin_t *t = (test_plugin_t *)p->plugin_data;
	for (uint32_t i = 0; i < in->size(in); i++) {
		apply_event(t, in->get(in, i));
	}
}

static const clap_plugin_params_t ext_params = {
	params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush,
};

/* --- process -------------------------------------------------------------------------------
 * Sample-accurate: the block is processed in runs between events, and each event takes effect
 * on its own sample, the first one in the block included. */

static clap_process_status plugin_process(const clap_plugin_t *p, const clap_process_t *proc) {
	test_plugin_t *t = (test_plugin_t *)p->plugin_data;
	const uint32_t n = proc->frames_count;
	const uint32_t n_events = proc->in_events->size(proc->in_events);
	const clap_audio_buffer_t *main_in = proc->audio_inputs_count > 0 ? &proc->audio_inputs[0] : NULL;
	const clap_audio_buffer_t *side_in = proc->audio_inputs_count > 1 ? &proc->audio_inputs[1] : NULL;
	const clap_audio_buffer_t *main_out = proc->audio_outputs_count > 0 ? &proc->audio_outputs[0] : NULL;
	if (!main_out) {
		return CLAP_PROCESS_ERROR;
	}
	uint32_t ev = 0;
	for (uint32_t f = 0; f < n; f++) {
		while (ev < n_events) {
			const clap_event_header_t *h = proc->in_events->get(proc->in_events, ev);
			if (h->time > f) {
				break;
			}
			apply_event(t, h);
			ev++;
		}
		for (uint32_t c = 0; c < main_out->channel_count; c++) {
			const float m = (main_in && c < main_in->channel_count) ? main_in->data32[c][f] : 0.0f;
			const float s = (side_in && c < side_in->channel_count) ? side_in->data32[c][f] : 0.0f;
			main_out->data32[c][f] = (float)(m * t->gain + s * t->mix);
		}
	}
	while (ev < n_events) { /* Events stamped past the block's end still count. */
		apply_event(t, proc->in_events->get(proc->in_events, ev++));
	}
	return CLAP_PROCESS_CONTINUE;
}

/* --- audio ports ------------------------------------------------------------------------- */

static uint32_t ports_count(const clap_plugin_t *p, bool is_input) {
	(void)p;
	return is_input ? 2 : 1;
}

static bool ports_get(const clap_plugin_t *p, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
	(void)p;
	if (index >= ports_count(p, is_input)) {
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->id = index;
	snprintf(info->name, sizeof(info->name), "%s", index == 0 ? "Main" : "Sidechain");
	info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
	info->channel_count = 2;
	info->port_type = CLAP_PORT_STEREO;
	info->in_place_pair = CLAP_INVALID_ID;
	return true;
}

static const clap_plugin_audio_ports_t ext_ports = { ports_count, ports_get };

/* --- state: two little-endian doubles ------------------------------------------------------ */

static bool state_save(const clap_plugin_t *p, const clap_ostream_t *s) {
	test_plugin_t *t = (test_plugin_t *)p->plugin_data;
	double v[2] = { t->gain, t->mix };
	return s->write(s, v, sizeof(v)) == (int64_t)sizeof(v);
}

static bool state_load(const clap_plugin_t *p, const clap_istream_t *s) {
	test_plugin_t *t = (test_plugin_t *)p->plugin_data;
	double v[2];
	uint8_t *b = (uint8_t *)v;
	int64_t got = 0;
	while (got < (int64_t)sizeof(v)) {
		const int64_t r = s->read(s, b + got, sizeof(v) - (uint64_t)got);
		if (r <= 0) {
			return false;
		}
		got += r;
	}
	t->gain = v[0];
	t->mix = v[1];
	return true;
}

static const clap_plugin_state_t ext_state = { state_save, state_load };

/* --- plugin lifecycle ---------------------------------------------------------------------- */

static bool plugin_init(const clap_plugin_t *p) {
	(void)p;
	return true;
}

static void plugin_destroy(const clap_plugin_t *p) {
	free(p->plugin_data);
}

static bool plugin_activate(const clap_plugin_t *p, double rate, uint32_t min_frames, uint32_t max_frames) {
	(void)p;
	(void)rate;
	(void)min_frames;
	(void)max_frames;
	return true;
}

static void plugin_noop(const clap_plugin_t *p) {
	(void)p;
}

static bool plugin_start_processing(const clap_plugin_t *p) {
	(void)p;
	return true;
}

#ifdef GAPH_TEST_GUI
/* --- gui (Windows and macOS) --------------------------------------------------------------- */

#define GUI_W 300
#define GUI_H 200

#if defined(_WIN32)
static const char *const GUI_API = CLAP_WINDOW_API_WIN32;
#else
static const char *const GUI_API = CLAP_WINDOW_API_COCOA;
#endif

static test_plugin_t *tp(const clap_plugin_t *p) {
	return (test_plugin_t *)p->plugin_data;
}

/* The platform part: make the child inside the host's window, size it, show it, destroy it. */
#if defined(_WIN32)
static void *child_create(void *parent, uint32_t w, uint32_t h) {
	if (!IsWindow((HWND)parent)) {
		return NULL;
	}
	return CreateWindowExW(0, L"STATIC", L"GAPH Test Plugin", WS_CHILD, 0, 0, (int)w, (int)h, (HWND)parent, NULL, NULL, NULL);
}
static void child_resize(void *c, uint32_t w, uint32_t h) {
	SetWindowPos((HWND)c, NULL, 0, 0, (int)w, (int)h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static void child_show(void *c, bool show) {
	ShowWindow((HWND)c, show ? SW_SHOW : SW_HIDE);
}
static void child_destroy(void *c) {
	DestroyWindow((HWND)c);
}
#else
typedef id (*msg_id)(id, SEL);
typedef id (*msg_id_rect)(id, SEL, CGRect);
typedef void (*msg_void_id)(id, SEL, id);
typedef void (*msg_void_size)(id, SEL, CGSize);
typedef void (*msg_void_bool)(id, SEL, signed char);

static void *child_create(void *parent, uint32_t w, uint32_t h) {
	if (!parent) {
		return NULL;
	}
	id view = ((msg_id)objc_msgSend)((id)objc_getClass("NSView"), sel_registerName("alloc"));
	view = ((msg_id_rect)objc_msgSend)(view, sel_registerName("initWithFrame:"), CGRectMake(0, 0, w, h));
	((msg_void_id)objc_msgSend)((id)parent, sel_registerName("addSubview:"), view);
	return view; /* +1 from alloc: ours, released in child_destroy */
}
static void child_resize(void *c, uint32_t w, uint32_t h) {
	((msg_void_size)objc_msgSend)((id)c, sel_registerName("setFrameSize:"), CGSizeMake(w, h));
}
static void child_show(void *c, bool show) {
	((msg_void_bool)objc_msgSend)((id)c, sel_registerName("setHidden:"), show ? 0 : 1);
}
static void child_destroy(void *c) {
	((msg_id)objc_msgSend)((id)c, sel_registerName("removeFromSuperview"));
	((msg_id)objc_msgSend)((id)c, sel_registerName("release"));
}
#endif

static bool gui_is_api_supported(const clap_plugin_t *p, const char *api, bool floating) {
	(void)p;
	return !floating && !strcmp(api, GUI_API);
}

static bool gui_get_preferred_api(const clap_plugin_t *p, const char **api, bool *floating) {
	(void)p;
	*api = GUI_API;
	*floating = false;
	return true;
}

static bool gui_create(const clap_plugin_t *p, const char *api, bool floating) {
	tp(p)->gui_scale = 1.0;
	return gui_is_api_supported(p, api, floating);
}

static void gui_destroy(const clap_plugin_t *p) {
	if (tp(p)->gui_child) {
		child_destroy(tp(p)->gui_child);
		tp(p)->gui_child = NULL;
	}
}

/* "cocoa" is in logical points, and a host must not call this there (gui.h); refusing it on
 * macOS lets a test catch a host that does. */
static bool gui_set_scale(const clap_plugin_t *p, double scale) {
#if defined(__APPLE__)
	(void)p, (void)scale;
	tp(p)->gui_scale = -1.0; /* marks the call, for the test */
	return false;
#else
	tp(p)->gui_scale = scale > 0 ? scale : 1.0;
	return true;
#endif
}

static bool gui_get_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h) {
	if (tp(p)->gui_scale < 0) { /* macOS, and the host called set_scale: answer visibly wrong */
		*w = *h = 999;
		return true;
	}
	const double s = tp(p)->gui_scale;
	*w = (uint32_t)(GUI_W * s + 0.5);
	*h = (uint32_t)(GUI_H * s + 0.5);
	return true;
}

static bool gui_no(const clap_plugin_t *p) {
	(void)p;
	return false;
}

static bool gui_get_resize_hints(const clap_plugin_t *p, clap_gui_resize_hints_t *h) {
	(void)p, (void)h;
	return false;
}

static bool gui_adjust_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h) {
	(void)p, (void)w, (void)h;
	return false;
}

static bool gui_set_size(const clap_plugin_t *p, uint32_t w, uint32_t h) {
	(void)p, (void)w, (void)h;
	return false;
}

static bool gui_set_parent(const clap_plugin_t *p, const clap_window_t *window) {
	uint32_t w, h;
	if (strcmp(window->api, GUI_API) != 0) {
		return false;
	}
	gui_get_size(p, &w, &h);
	tp(p)->gui_child = child_create(window->ptr, w, h);
	return tp(p)->gui_child != NULL;
}

static bool gui_set_transient(const clap_plugin_t *p, const clap_window_t *window) {
	(void)p, (void)window;
	return false;
}

static void gui_suggest_title(const clap_plugin_t *p, const char *title) {
	(void)p, (void)title;
}

/* With GAPH_TEST_GUI_REQUEST_RESIZE=WxH: the plugin grows its own view and asks the host to fit
 * its window around it, as a plugin does when its user resizes or rescales it. */
static void maybe_request_resize(const clap_plugin_t *p) {
	const char *want = getenv("GAPH_TEST_GUI_REQUEST_RESIZE");
	unsigned w = 0, h = 0;
	if (!want || sscanf(want, "%ux%u", &w, &h) != 2 || !w || !h) {
		return;
	}
	const clap_host_gui_t *host_gui = (const clap_host_gui_t *)tp(p)->host->get_extension(tp(p)->host, CLAP_EXT_GUI);
	if (host_gui && host_gui->request_resize(tp(p)->host, w, h)) {
		child_resize(tp(p)->gui_child, w, h);
	}
}

static bool gui_show(const clap_plugin_t *p) {
	if (!tp(p)->gui_child) {
		return false;
	}
	child_show(tp(p)->gui_child, true);
	maybe_request_resize(p);
	return true;
}

static bool gui_hide(const clap_plugin_t *p) {
	if (tp(p)->gui_child) {
		child_show(tp(p)->gui_child, false);
	}
	return true;
}

static const clap_plugin_gui_t ext_gui = {
	gui_is_api_supported, gui_get_preferred_api, gui_create, gui_destroy, gui_set_scale, gui_get_size,
	gui_no, gui_get_resize_hints, gui_adjust_size, gui_set_size, gui_set_parent, gui_set_transient,
	gui_suggest_title, gui_show, gui_hide,
};
#endif


static const void *plugin_get_extension(const clap_plugin_t *p, const char *id) {
	(void)p;
	if (!strcmp(id, CLAP_EXT_PARAMS)) {
		return &ext_params;
	}
	if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) {
		return &ext_ports;
	}
	if (!strcmp(id, CLAP_EXT_STATE)) {
		return &ext_state;
	}
#ifdef GAPH_TEST_GUI
	if (!strcmp(id, CLAP_EXT_GUI)) {
		return &ext_gui;
	}
#endif
	return NULL;
}

/* --- factory and entry --------------------------------------------------------------------- */

static uint32_t factory_count(const clap_plugin_factory_t *f) {
	(void)f;
	return 1;
}

static const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *f, uint32_t index) {
	(void)f;
	return index == 0 ? &descriptor : NULL;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
	(void)f;
	if (strcmp(id, descriptor.id) != 0) {
		return NULL;
	}
	test_plugin_t *t = (test_plugin_t *)calloc(1, sizeof(test_plugin_t));
	t->host = host;
	t->gain = 1.0;
	t->mix = 0.0;
	t->plugin.desc = &descriptor;
	t->plugin.plugin_data = t;
	t->plugin.init = plugin_init;
	t->plugin.destroy = plugin_destroy;
	t->plugin.activate = plugin_activate;
	t->plugin.deactivate = plugin_noop;
	t->plugin.start_processing = plugin_start_processing;
	t->plugin.stop_processing = plugin_noop;
	t->plugin.reset = plugin_noop;
	t->plugin.process = plugin_process;
	t->plugin.get_extension = plugin_get_extension;
	t->plugin.on_main_thread = plugin_noop;
	return &t->plugin;
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
