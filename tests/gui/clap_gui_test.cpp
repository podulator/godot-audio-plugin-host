// The CLAP GUI path, with the host's real code (ClapLibrary, ClapProcessor, NativeWindow) and our
// test plugin's GUI: Windows (natively in CI, or under Wine on Linux: tests/windows/run.sh) and
// macOS (CI's Mac runner: tests/macos/run.sh, built as Objective-C++). What Godot adds on top
// (EditorWindow) does the same on every platform: pump the events, resize on request.
#include "clap/clap_library.h"
#include "clap/clap_processor.h"
#include "core/native_window.h"

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
	[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory]; // no Dock icon
#endif
	// Windows and Linux: the host passes the scale on, so the GUI is 300x200 x 1.5. macOS: it must
	// not (cocoa is in points); the test plugin answers 999x999 if it does.
	const bool scaled = plugin_window_api().host_sets_scale;
	const uint32_t want_w = scaled ? 450 : 300, want_h = scaled ? 300 : 200;
	std::string err;
	auto lib = ClapLibrary::open(argc > 1 ? argv[1] : "gaph_test.clap", err);
	auto p = lib ? ClapProcessor::create(lib, "", 48000.0, 512, true, err) : nullptr;
	CHECK(p != nullptr, "the test plugin loads%s%s", err.empty() ? "" : ": ", err.c_str());
	if (!p) {
		return 1;
	}
	CHECK(p->gui_available(), "its GUI is offered for this platform's window API (\"%s\")", plugin_window_api().clap);

	NativeWindow window;
	CHECK(window.create("GAPH Test Plugin", 400, 300, 0, err), "the host window opens");
	uint32_t w = 0, h = 0;
	bool ok = p->gui_open(window.get_id(), 1.5, w, h, err);
	CHECK(ok && w == want_w && h == want_h, "gui_open with editor scale 1.5 reports %ux%u, want %ux%u (%s)%s%s", w, h, want_w, want_h,
			scaled ? "the scale reached the plugin" : "no set_scale on macOS", err.empty() ? "" : ": ", err.c_str());
	window.resize(w, h); // what EditorWindow::attach does with the size
	window.show();
	pump();
	Child c = child_of(window.get_id());
	CHECK(c.exists, "the plugin's GUI is inside the host window (the handle reached it)");
	CHECK(c.w == int(want_w) && c.h == int(want_h) && c.shown, "it is %dx%d and shown", c.w, c.h);
	int cw, ch;
	content_size(window.get_id(), cw, ch);
	CHECK(cw == int(want_w) && ch == int(want_h), "the host window's content area matches it exactly (%dx%d)", cw, ch);

	p->gui_close();
	pump();
	CHECK(!child_of(window.get_id()).exists, "gui_close() removes the plugin's GUI");
	ok = p->gui_open(window.get_id(), 1.0, w, h, err);
	c = child_of(window.get_id());
	CHECK(ok && w == 300 && h == 200 && c.w == 300 && c.h == 200, "reopened at scale 1.0: %ux%u, the GUI %dx%d", w, h, c.w, c.h);
	p->gui_close();

	// The plugin asks for a new size (clap.gui request_resize, from its show()); the host takes the
	// request, and on its next frame (EditorWindow::tick) fits its window around the GUI.
#if defined(_WIN32)
	_putenv_s("GAPH_TEST_GUI_REQUEST_RESIZE", "520x340");
#else
	setenv("GAPH_TEST_GUI_REQUEST_RESIZE", "520x340", 1);
#endif
	ok = p->gui_open(window.get_id(), 1.0, w, h, err);
	uint32_t rw = 0, rh = 0;
	const bool requested = p->gui_take_resize(rw, rh);
	CHECK(ok && requested && rw == 520 && rh == 340, "a plugin's resize request reaches the host (%ux%u)", rw, rh);
	if (requested) {
		window.resize(rw, rh); // EditorWindow::tick
	}
	pump();
	c = child_of(window.get_id());
	content_size(window.get_id(), cw, ch);
	CHECK(c.w == 520 && c.h == 340 && cw == 520 && ch == 340, "... and the window follows it: window %dx%d, GUI %dx%d", cw, ch, c.w, c.h);
	CHECK(!p->gui_take_resize(rw, rh), "a request is taken once");
	p->gui_close();

	window.destroy();
	p.reset();
	std::printf("CLAP GUI (%s) %s\n", plugin_window_api().clap, failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
}
