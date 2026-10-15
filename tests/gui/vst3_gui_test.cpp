// The VST3 editor path, with the host's real code (vst3_open_module, Vst3Processor, NativeWindow)
// and our VST3 test plugin's editor (tests/plugins/gaph_test_vst3.cpp): Windows (natively in CI,
// or under Wine on Linux) and macOS (CI's Mac runner). The VST3 counterpart of clap_gui_test.cpp.
// VST is a registered trademark of Steinberg Media Technologies GmbH.
#include "core/native_window.h"
#include "vst3/vst3_module.h"
#include "vst3/vst3_processor.h"

#include "gui_probe.h"

#include <cstdio>
#include <cstdlib>

using namespace gaph;

static int failures = 0;
#define CHECK(cond, ...)                                 \
	do {                                                 \
		std::printf("%s  ", (cond) ? "PASS" : "FAIL"); \
		std::printf(__VA_ARGS__);                        \
		std::printf("\n");                               \
		failures += (cond) ? 0 : 1;                      \
	} while (0)

int main(int argc, char **argv) {
#if defined(__APPLE__)
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
#endif
	const bool scaled = plugin_window_api().host_sets_scale;
	const uint32_t want_w = scaled ? 450 : 300, want_h = scaled ? 300 : 200;
	std::string err;
	auto mod = vst3_open_module(argc > 1 ? argv[1] : "gaph_test_vst3.vst3", err);
	auto p = mod ? Vst3Processor::create(mod, "", 48000.0, 512, true, err) : nullptr;
	CHECK(p != nullptr, "the VST3 test plugin loads%s%s", err.empty() ? "" : ": ", err.c_str());
	if (!p) {
		return 1;
	}
	CHECK(p->gui_available(), "its editor is offered for this platform (\"%s\")", plugin_window_api().vst3);

	NativeWindow window;
	CHECK(window.create("GAPH Test VST3", 400, 300, 0, err), "the host window opens");
	uint32_t w = 0, h = 0;
	bool ok = p->gui_open(window.get_id(), 1.5, w, h, err);
	CHECK(ok && w == want_w && h == want_h, "gui_open with editor scale 1.5 reports %ux%u, want %ux%u (%s)%s%s", w, h, want_w, want_h,
			scaled ? "setContentScaleFactor reached the view" : "no content scale on macOS", err.empty() ? "" : ": ", err.c_str());
	window.resize(w, h);
	window.show();
	pump();
	Child c = child_of(window.get_id());
	CHECK(c.exists, "the editor is inside the host window (attached() got the handle)");
	CHECK(c.w == int(want_w) && c.h == int(want_h) && c.shown, "it is %dx%d and shown", c.w, c.h);
	int cw, ch;
	content_size(window.get_id(), cw, ch);
	CHECK(cw == int(want_w) && ch == int(want_h), "the host window's content area matches it exactly (%dx%d)", cw, ch);

	p->gui_close();
	pump();
	CHECK(!child_of(window.get_id()).exists, "gui_close() removes the editor (removed(), then the view released)");
	ok = p->gui_open(window.get_id(), 1.0, w, h, err);
	c = child_of(window.get_id());
	CHECK(ok && w == 300 && h == 200 && c.w == 300 && c.h == 200, "reopened at scale 1.0: %ux%u, the editor %dx%d", w, h, c.w, c.h);
	p->gui_close();

	// The view asks for a new size (IPlugFrame::resizeView, from attached()); the host confirms
	// with onSize at once and fits its window on the next frame.
#if defined(_WIN32)
	_putenv_s("GAPH_TEST_GUI_REQUEST_RESIZE", "520x340");
#else
	setenv("GAPH_TEST_GUI_REQUEST_RESIZE", "520x340", 1);
#endif
	ok = p->gui_open(window.get_id(), 1.0, w, h, err);
	uint32_t rw = 0, rh = 0;
	const bool requested = p->gui_take_resize(rw, rh);
	CHECK(ok && requested && rw == 520 && rh == 340, "the editor's resize request reaches the host (%ux%u)", rw, rh);
	if (requested) {
		window.resize(rw, rh);
	}
	pump();
	c = child_of(window.get_id());
	content_size(window.get_id(), cw, ch);
	CHECK(c.w == 520 && c.h == 340 && cw == 520 && ch == 340, "... and the window and editor follow it: window %dx%d, editor %dx%d", cw, ch, c.w, c.h);
	p->gui_close();

	window.destroy();
	p.reset();
	mod.reset();
	std::printf("VST3 GUI (%s) %s\n", plugin_window_api().vst3, failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
}
