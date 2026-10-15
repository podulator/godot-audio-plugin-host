#include "x11_window.h"

#ifdef GAPH_HAS_X11

// The only file that sees X11's headers (see x11_window.h for why).
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <dlfcn.h>
#include <cstdlib>
#include <type_traits>

namespace gaph {

// Everything X11 lives here: the connection, the atom for the close button, and libX11's
// functions as typed pointers (decltype(&XOpenDisplay) is XOpenDisplay's exact type, so a
// wrong signature is a compile error, though the functions are only found at runtime).
struct X11Window::Impl {
	Display *display = nullptr;
	Atom wm_delete = 0;

	decltype(&XOpenDisplay) open_display = nullptr;
	decltype(&XCloseDisplay) close_display = nullptr;
	decltype(&XCreateSimpleWindow) create_simple_window = nullptr;
	decltype(&XDestroyWindow) destroy_window = nullptr;
	decltype(&XMapRaised) map_raised = nullptr;
	decltype(&XResizeWindow) resize_window = nullptr;
	decltype(&XStoreName) store_name = nullptr;
	decltype(&XInternAtom) intern_atom = nullptr;
	decltype(&XSetWMProtocols) set_wm_protocols = nullptr;
	decltype(&XSelectInput) select_input = nullptr;
	decltype(&XPending) pending = nullptr;
	decltype(&XNextEvent) next_event = nullptr;
	decltype(&XSync) sync = nullptr;
	decltype(&XAllocSizeHints) alloc_size_hints = nullptr;
	decltype(&XSetWMNormalHints) set_wm_normal_hints = nullptr;
	decltype(&XFree) free = nullptr;

	// libX11 is opened once per process and never closed: unloading it while any X connection
	// in the process (a plugin's, say) might still use it would be asking for trouble.
	bool load(std::string &r_error) {
		if (open_display) {
			return true;
		}
		void *lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
		if (!lib) {
			r_error = "libX11.so.6 not found: plugin windows need an X server (XWayland on Wayland)";
			return false;
		}
		bool ok = true;
		auto get = [&](auto &r_fn, const char *p_name) {
			r_fn = reinterpret_cast<std::remove_reference_t<decltype(r_fn)>>(dlsym(lib, p_name));
			ok = ok && r_fn;
		};
		get(open_display, "XOpenDisplay");
		get(close_display, "XCloseDisplay");
		get(create_simple_window, "XCreateSimpleWindow");
		get(destroy_window, "XDestroyWindow");
		get(map_raised, "XMapRaised");
		get(resize_window, "XResizeWindow");
		get(store_name, "XStoreName");
		get(intern_atom, "XInternAtom");
		get(set_wm_protocols, "XSetWMProtocols");
		get(select_input, "XSelectInput");
		get(pending, "XPending");
		get(next_event, "XNextEvent");
		get(sync, "XSync");
		get(alloc_size_hints, "XAllocSizeHints");
		get(set_wm_normal_hints, "XSetWMNormalHints");
		get(free, "XFree");
		if (!ok) {
			r_error = "libX11.so.6 lacks a function it should have";
			open_display = nullptr;
		}
		return ok;
	}
};

X11Window::X11Window() :
		impl(new Impl()) {}

// Defined here, not in the header, because unique_ptr<Impl> needs the full Impl to delete it.
X11Window::~X11Window() {
	destroy();
}

bool X11Window::create(const std::string &p_title, uint32_t p_width, uint32_t p_height, uint64_t, std::string &r_error) {
	destroy();
	if (!std::getenv("DISPLAY")) {
		r_error = "no DISPLAY: plugin windows need an X server (XWayland on Wayland)";
		return false;
	}
	if (!impl->load(r_error)) {
		return false;
	}
	// Our own connection, separate from any Godot may have, so nothing here can upset it.
	impl->display = impl->open_display(nullptr);
	if (!impl->display) {
		r_error = "cannot connect to the X server";
		return false;
	}
	Display *d = impl->display;
	const int screen = DefaultScreen(d);
	window = impl->create_simple_window(d, RootWindow(d, screen), 0, 0, p_width ? p_width : 400, p_height ? p_height : 300, 0,
			BlackPixel(d, screen), BlackPixel(d, screen));
	impl->store_name(d, window, p_title.c_str());
	// Ask the window manager to send a message, rather than kill our connection, when the user
	// clicks the close button: the WM_DELETE_WINDOW protocol.
	impl->wm_delete = impl->intern_atom(d, "WM_DELETE_WINDOW", False);
	impl->set_wm_protocols(d, window, &impl->wm_delete, 1);
	impl->select_input(d, window, StructureNotifyMask);
	resize(p_width, p_height);
	flush();
	return true;
}

void X11Window::destroy() {
	if (impl->display) {
		if (window) {
			impl->destroy_window(impl->display, window);
		}
		impl->close_display(impl->display);
	}
	impl->display = nullptr;
	window = 0;
}

void X11Window::show() {
	if (window) {
		impl->map_raised(impl->display, window);
		flush();
	}
}

void X11Window::resize(uint32_t p_width, uint32_t p_height) {
	if (!window || !p_width || !p_height) {
		return;
	}
	// Plugin GUIs have a size of their own, so the window is fixed at it (minimum = maximum)
	// rather than letting the user drag it to sizes the plugin would not fill.
	XSizeHints *hints = impl->alloc_size_hints();
	hints->flags = PMinSize | PMaxSize;
	hints->min_width = hints->max_width = int(p_width);
	hints->min_height = hints->max_height = int(p_height);
	impl->set_wm_normal_hints(impl->display, window, hints);
	impl->free(hints);
	impl->resize_window(impl->display, window, p_width, p_height);
	flush();
}

void X11Window::flush() {
	if (impl->display) {
		// XSync, not just XFlush: wait until the server has processed everything, so the window
		// really exists before its id is handed to a plugin on another connection.
		impl->sync(impl->display, False);
	}
}

bool X11Window::poll_close_requested() {
	bool close = false;
	while (impl->display && impl->pending(impl->display) > 0) {
		XEvent ev;
		impl->next_event(impl->display, &ev);
		if (ev.type == ClientMessage && Atom(ev.xclient.data.l[0]) == impl->wm_delete) {
			close = true;
		}
	}
	return close;
}

} // namespace gaph

#endif
