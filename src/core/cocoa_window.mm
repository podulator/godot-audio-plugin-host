// Objective-C++: C++ that can also send Objective-C messages ([object method:argument]), the
// language AppKit speaks. Built with ARC (automatic reference counting): the compiler retains and
// releases Objective-C objects itself, including the NSWindow* held in the C++ struct below.

#include "cocoa_window.h"

#if defined(__APPLE__)

#import <AppKit/AppKit.h>

#if !__has_feature(objc_arc)
#error "cocoa_window.mm must be compiled with -fobjc-arc (see SConstruct)"
#endif

namespace gaph {

struct CocoaWindow::Impl {
	NSWindow *window = nil; // strong: ARC keeps it alive while it is here
	bool shown = false;
};

CocoaWindow::CocoaWindow() :
		impl(new Impl) {}

CocoaWindow::~CocoaWindow() {
	destroy();
}

bool CocoaWindow::is_open() const {
	return impl->window != nil;
}

uint64_t CocoaWindow::get_id() const {
	// __bridge: hand out the pointer without transferring ownership; the window keeps the view.
	return impl->window ? uint64_t(reinterpret_cast<uintptr_t>((__bridge void *)impl->window.contentView)) : 0;
}

bool CocoaWindow::create(const std::string &p_title, uint32_t p_width, uint32_t p_height, uint64_t, std::string &r_error) {
	if (impl->window) {
		return true;
	}
	// Not resizable by the user: the window is sized to the plugin, as on the other platforms.
	const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
	NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, p_width, p_height)
											  styleMask:style
												backing:NSBackingStoreBuffered
												  defer:NO];
	if (!w) {
		r_error = "could not create an NSWindow";
		return false;
	}
	w.releasedWhenClosed = NO; // ARC owns it; AppKit freeing it on close would leave us dangling
	w.title = [NSString stringWithUTF8String:p_title.c_str()] ?: @"Plugin";
	w.contentView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, p_width, p_height)];
	[w center];
	impl->window = w;
	impl->shown = false;
	return true;
}

void CocoaWindow::destroy() {
	if (impl->window) {
		[impl->window orderOut:nil];
		[impl->window close];
		impl->window = nil; // ARC releases it
	}
	impl->shown = false;
}

void CocoaWindow::show() {
	if (!impl->window) {
		return;
	}
	if (impl->window.miniaturized) {
		[impl->window deminiaturize:nil];
	}
	[impl->window makeKeyAndOrderFront:nil];
	impl->shown = true;
}

void CocoaWindow::resize(uint32_t p_width, uint32_t p_height) {
	if (!impl->window || p_width == 0 || p_height == 0) {
		return;
	}
	// setContentSize would keep the BOTTOM-left corner still (AppKit's origin is bottom-left), and
	// the title bar would jump; keeping the top edge still is what a user expects.
	const NSRect old_frame = impl->window.frame;
	NSRect frame = [impl->window frameRectForContentRect:NSMakeRect(0, 0, p_width, p_height)];
	frame.origin.x = old_frame.origin.x;
	frame.origin.y = old_frame.origin.y + old_frame.size.height - frame.size.height;
	[impl->window setFrame:frame display:YES];
}

bool CocoaWindow::poll_close_requested() {
	// Closed by its button: no longer on screen, though not minimised, nor merely hidden with the
	// whole app (Cmd+H).
	return impl->window && impl->shown && !impl->window.visible && !impl->window.miniaturized && !NSApp.hidden;
}

} // namespace gaph

#endif
