// CocoaWindow on its own, as a plugin uses it, without Godot: run on macOS (CI's runner) by
// tests/macos/run.sh. A plain NSView stands in for a plugin's GUI, added as plugins add theirs.
#include "core/cocoa_window.h"

#import <AppKit/AppKit.h>

#include <cstdio>

static int failures = 0;
#define CHECK(cond, what)                                        \
	do {                                                         \
		std::printf("%s  %s\n", (cond) ? "PASS" : "FAIL", what); \
		failures += (cond) ? 0 : 1;                              \
	} while (0)

// What Godot's main loop does each frame on macOS: hand pending events to AppKit.
static void pump() {
	for (int i = 0; i < 20; i++) {
		NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
										   inMode:NSDefaultRunLoopMode
										  dequeue:YES];
		if (e) {
			[NSApp sendEvent:e];
		}
	}
}

int main() {
	@autoreleasepool {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory]; // no Dock icon
		std::string err;
		gaph::CocoaWindow w;
		CHECK(w.create("Test plugin window", 640, 480, 0, err), "creates");
		NSView *content = (__bridge NSView *)reinterpret_cast<void *>(uintptr_t(w.get_id()));
		CHECK(content && content.window, "get_id() is the window's content view");
		CHECK(content.frame.size.width == 640 && content.frame.size.height == 480, "the content area is the size asked for (points)");

		// The plugin's side: CLAP "cocoa" and VST3 "NSView" plugins add their view as a subview.
		NSView *plugin_view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 640, 480)];
		[content addSubview:plugin_view];
		CHECK(plugin_view.window == content.window, "a plugin's view sits inside it");

		w.show();
		pump();
		CHECK(content.window.visible, "show() puts it on screen");
		const NSRect before = content.window.frame;
		w.resize(900, 720);
		CHECK(content.frame.size.width == 900 && content.frame.size.height == 720, "resize() sets the content area exactly");
		const NSRect after = content.window.frame;
		CHECK(NSMaxY(after) == NSMaxY(before), "... keeping the title bar where it was");

		CHECK(!w.poll_close_requested(), "no close request before one is made");
		[content.window performClose:nil]; // the close button
		pump();
		CHECK(w.poll_close_requested(), "the close button is seen");
		CHECK(w.is_open(), "... and the window is still ours until destroy()");
		w.destroy();
		CHECK(!w.is_open(), "destroy() releases it");
		CHECK(w.create("Again", 300, 200, 0, err), "a second window after the first");
		w.destroy();
	}
	std::printf("COCOA WINDOW %s\n", failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
}
