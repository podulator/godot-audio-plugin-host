#pragma once

// DynLib: load a shared library at runtime and look up functions in it.
//
// A plugin is a shared library (.so on Linux, .dll on Windows, a bundle on macOS) that the host
// did not link against at build time. The host opens it by path while running, then asks for a
// known symbol by name: CLAP's is "clap_entry", VST3's is "GetPluginFactory". That symbol is the
// plugin's front door, and everything else is reached through it.
//
// Every OS has its own API for this, so the #ifdef blocks choose one at compile time:
// _WIN32 is defined by every Windows compiler, and everything else here is POSIX (Linux, macOS).

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // Leaves rarely used parts out of windows.h; it is huge otherwise.
#endif
#ifndef NOMINMAX
#define NOMINMAX // Otherwise windows.h defines min and max as macros, breaking std::min/max.
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace gaph {

// Closed automatically on destruction (RAII again). Main thread.
class DynLib {
public:
	DynLib() = default;
	~DynLib() { close(); }
	// Copying would close the same handle twice; forbid it.
	DynLib(const DynLib &) = delete;
	DynLib &operator=(const DynLib &) = delete;

	bool open(const std::string &p_path, std::string &r_error) {
		close();
#ifdef _WIN32
		// Windows' wide-character API is the only one that copes with non-ASCII paths, so the
		// UTF-8 path is converted to UTF-16 first.
		int n = MultiByteToWideChar(CP_UTF8, 0, p_path.c_str(), -1, nullptr, 0);
		std::wstring wide(n, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, p_path.c_str(), -1, wide.data(), n);
		handle = LoadLibraryW(wide.c_str());
		if (!handle) {
			r_error = "LoadLibrary failed with error " + std::to_string(GetLastError());
		}
#else
		// RTLD_NOW: resolve every symbol immediately, so a broken plugin fails here with a clear
		// error rather than crashing later on its first call into a missing function.
		// RTLD_LOCAL: keep the plugin's symbols private. Every CLAP plugin exports a symbol called
		// `clap_entry`; with RTLD_GLOBAL the second plugin loaded could end up calling the first
		// one's.
		handle = dlopen(p_path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!handle) {
			const char *e = dlerror();
			r_error = e ? e : "dlopen failed";
		}
#endif
		return handle != nullptr;
	}

	// The address of an exported symbol, or nullptr. The caller casts it to the right type.
	void *symbol(const char *p_name) const {
		if (!handle) {
			return nullptr;
		}
#ifdef _WIN32
		return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(handle), p_name));
#else
		return dlsym(handle, p_name);
#endif
	}

	void close() {
		if (!handle) {
			return;
		}
#ifdef _WIN32
		FreeLibrary(static_cast<HMODULE>(handle));
#else
		dlclose(handle);
#endif
		handle = nullptr;
	}

private:
	void *handle = nullptr;
};

} // namespace gaph
