#pragma once
// Shared by the GUI tests (clap_gui_test.cpp, vst3_gui_test.cpp): run the platform's event loop a
// little, as Godot's main loop does each frame, and look at what a plugin put inside our window.

#include <cstdint>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#import <AppKit/AppKit.h>
#endif

// The platform part: run the event loop a little, and look at what the plugin put in our window.
struct Child {
	bool exists = false; // there is one, inside our window
	bool shown = false;
	int w = 0, h = 0;
};

#if defined(_WIN32)
static void pump() {
	MSG msg;
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}
static Child child_of(uint64_t p_window) {
	HWND parent = reinterpret_cast<HWND>(uintptr_t(p_window));
	HWND c = GetWindow(parent, GW_CHILD);
	Child r;
	if (c && GetParent(c) == parent) {
		RECT rc;
		GetClientRect(c, &rc);
		r = { true, IsWindowVisible(c) != 0, int(rc.right), int(rc.bottom) };
	}
	return r;
}
static void content_size(uint64_t p_window, int &r_w, int &r_h) {
	RECT rc;
	GetClientRect(reinterpret_cast<HWND>(uintptr_t(p_window)), &rc);
	r_w = rc.right;
	r_h = rc.bottom;
}
#elif defined(__APPLE__)
static void pump() {
	for (int i = 0; i < 10; i++) {
		NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
										   inMode:NSDefaultRunLoopMode
										  dequeue:YES];
		if (e) {
			[NSApp sendEvent:e];
		}
	}
}
static NSView *view_of(uint64_t p_window) {
	return (__bridge NSView *)reinterpret_cast<void *>(uintptr_t(p_window));
}
static Child child_of(uint64_t p_window) {
	NSView *content = view_of(p_window);
	Child r;
	if (content.subviews.count > 0) {
		NSView *c = content.subviews[0];
		r = { c.superview == content, !c.hidden && content.window.visible, int(c.frame.size.width), int(c.frame.size.height) };
	}
	return r;
}
static void content_size(uint64_t p_window, int &r_w, int &r_h) {
	NSView *content = view_of(p_window);
	r_w = int(content.frame.size.width);
	r_h = int(content.frame.size.height);
}
#endif
