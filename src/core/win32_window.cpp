#include "win32_window.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace gaph {

namespace {

// Each extension (clap_host, vst3_host) is its own DLL with this same code, and a window class
// name is per process: so the name carries the module's address, and two DLLs never collide.
const wchar_t CLASS_PREFIX[] = L"GodotAudioPluginHostWindow_";

HMODULE this_module() {
	HMODULE m = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&this_module), &m);
	return m;
}

std::wstring widen(const std::string &p_utf8) {
	const int n = MultiByteToWideChar(CP_UTF8, 0, p_utf8.c_str(), -1, nullptr, 0);
	std::wstring w(n > 0 ? n : 1, L'\0');
	if (n > 0) {
		MultiByteToWideChar(CP_UTF8, 0, p_utf8.c_str(), -1, w.data(), n);
	}
	w.resize(w.empty() ? 0 : w.size() - 1); // drop the terminator MultiByteToWideChar counted
	return w;
}

// The style: a title bar with close and minimise, not resizable by the user (the window is sized
// to the plugin, as on Linux). WS_CLIPCHILDREN: our window never paints over the plugin's child.
const DWORD STYLE = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
const DWORD EX_STYLE = 0;

} // namespace

struct Win32Window::Impl {
	HWND hwnd = nullptr;
	bool close_requested = false;
	std::wstring class_name;

	// Called by Windows (from DispatchMessageW, in Godot's loop) for every message to our window.
	// The Impl is found through GWLP_USERDATA, set from CreateWindowExW's last argument.
	static LRESULT CALLBACK proc(HWND p_hwnd, UINT p_msg, WPARAM p_w, LPARAM p_l) {
		if (p_msg == WM_NCCREATE) {
			auto *cs = reinterpret_cast<CREATESTRUCTW *>(p_l);
			SetWindowLongPtrW(p_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
		}
		Impl *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(p_hwnd, GWLP_USERDATA));
		switch (p_msg) {
			case WM_CLOSE:
				// Not DefWindowProc, which would destroy the window under the plugin. Flag it, and
				// let the host close the plugin's GUI first, then the window (EditorWindow::tick).
				if (self) {
					self->close_requested = true;
				}
				return 0;
			case WM_SETFOCUS: {
				// Keyboard focus to the plugin: clicking the title bar or alt-tabbing back gives it to
				// our window, which has no use for keys; the plugin's child does.
				HWND child = GetWindow(p_hwnd, GW_CHILD);
				if (child) {
					SetFocus(child);
				}
				return 0;
			}
			case WM_ERASEBKGND:
				return 1; // The plugin covers all of it; erasing first only flickers.
			default:
				return DefWindowProcW(p_hwnd, p_msg, p_w, p_l);
		}
	}
};

Win32Window::Win32Window() :
		impl(new Impl) {}

Win32Window::~Win32Window() {
	destroy();
}

bool Win32Window::is_open() const {
	return impl->hwnd != nullptr;
}

uint64_t Win32Window::get_id() const {
	return uint64_t(reinterpret_cast<uintptr_t>(impl->hwnd));
}

bool Win32Window::create(const std::string &p_title, uint32_t p_width, uint32_t p_height, uint64_t p_owner, std::string &r_error) {
	if (impl->hwnd) {
		return true;
	}
	HMODULE module = this_module();
	if (impl->class_name.empty()) {
		wchar_t addr[32];
		wsprintfW(addr, L"%p", static_cast<void *>(module));
		impl->class_name = std::wstring(CLASS_PREFIX) + addr;
	}
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	if (!GetClassInfoExW(module, impl->class_name.c_str(), &wc)) { // not if another window has it
		wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = &Impl::proc;
		wc.hInstance = module;
		// IDC_ARROW is 32512; spelled out, because the macro is a narrow or a wide string
		// depending on whether UNICODE is defined, and only the wide one fits LoadCursorW.
		wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
		wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
		wc.lpszClassName = impl->class_name.c_str();
		if (!RegisterClassExW(&wc)) {
			r_error = "RegisterClassExW failed (error " + std::to_string(GetLastError()) + ")";
			return false;
		}
	}
	// The outer size that gives this client area, at our style.
	RECT r = { 0, 0, LONG(p_width), LONG(p_height) };
	AdjustWindowRectEx(&r, STYLE, FALSE, EX_STYLE);
	impl->close_requested = false;
	impl->hwnd = CreateWindowExW(EX_STYLE, impl->class_name.c_str(), widen(p_title).c_str(), STYLE,
			CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
			reinterpret_cast<HWND>(uintptr_t(p_owner)), nullptr, module, impl.get());
	if (!impl->hwnd) {
		r_error = "CreateWindowExW failed (error " + std::to_string(GetLastError()) + ")";
		return false;
	}
	return true;
}

void Win32Window::destroy() {
	if (impl->hwnd) {
		DestroyWindow(impl->hwnd);
		impl->hwnd = nullptr;
		// The class points at Impl::proc, code in this DLL. Unregistered as soon as its window is
		// gone, so it can never outlive the DLL (the editor unloads extensions it reloads).
		UnregisterClassW(impl->class_name.c_str(), this_module());
	}
	impl->close_requested = false;
}

void Win32Window::show() {
	if (!impl->hwnd) {
		return;
	}
	ShowWindow(impl->hwnd, IsIconic(impl->hwnd) ? SW_RESTORE : SW_SHOWNORMAL);
	SetForegroundWindow(impl->hwnd);
}

void Win32Window::resize(uint32_t p_width, uint32_t p_height) {
	if (!impl->hwnd || p_width == 0 || p_height == 0) {
		return;
	}
	RECT r = { 0, 0, LONG(p_width), LONG(p_height) };
	AdjustWindowRectEx(&r, STYLE, FALSE, EX_STYLE);
	SetWindowPos(impl->hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool Win32Window::poll_close_requested() {
	return impl->hwnd && impl->close_requested;
}

} // namespace gaph

#endif
