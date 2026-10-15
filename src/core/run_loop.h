#pragma once

// RunLoop: timers and file-descriptor watches a plugin GUI asks the host to run for it.
//
// On Windows and macOS a GUI hooks into the OS's own event loop. Linux has no single loop every
// toolkit shares, so the plugin APIs ask the HOST for one: CLAP through its timer-support and
// posix-fd-support extensions, VST3 through IRunLoop. Both come down to the same two services,
// done here once:
//   timers: "call me every N ms"            -> checked every frame
//   fds:    "call me when this fd is readable" -> poll() with a zero timeout, every frame
// Pumped from HostedEffect::idle(), i.e. once per Godot frame on the main thread. So a 16 ms
// frame is the effective resolution: fine for GUI repaint timers, which is what they are for.

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#if defined(__linux__) || defined(__FreeBSD__)
#include <poll.h>
#endif

namespace gaph {

class RunLoop {
public:
	using Callback = std::function<void()>;

	// Returns an id for unregister_timer().
	uint32_t register_timer(uint32_t p_period_ms, Callback p_fn) {
		const uint32_t id = next_id++;
		timers.push_back(Timer{ id, p_period_ms ? p_period_ms : 1, now_ms() + p_period_ms, std::move(p_fn) });
		return id;
	}

	bool unregister_timer(uint32_t p_id) {
		for (size_t i = 0; i < timers.size(); i++) {
			if (timers[i].id == p_id) {
				timers.erase(timers.begin() + long(i));
				return true;
			}
		}
		return false;
	}

	void register_fd(int p_fd, Callback p_fn) {
		unregister_fd(p_fd);
		fds.push_back(Fd{ p_fd, std::move(p_fn) });
	}

	bool unregister_fd(int p_fd) {
		for (size_t i = 0; i < fds.size(); i++) {
			if (fds[i].fd == p_fd) {
				fds.erase(fds.begin() + long(i));
				return true;
			}
		}
		return false;
	}

	// MAIN THREAD, once per frame. Callbacks may (un)register timers or fds, so each pass works
	// on a copy of the list, and an entry removed meanwhile is skipped.
	void pump() {
		const uint64_t now = now_ms();
		std::vector<Timer> due;
		for (Timer &t : timers) {
			if (now >= t.next_due) {
				// Re-armed from now, not from the missed due time: after a long frame, one call,
				// not a burst catching up.
				t.next_due = now + t.period_ms;
				due.push_back(t);
			}
		}
		for (Timer &t : due) {
			if (has_timer(t.id)) {
				t.fn();
			}
		}
#if defined(__linux__) || defined(__FreeBSD__)
		if (fds.empty()) {
			return;
		}
		std::vector<pollfd> polled;
		for (const Fd &f : fds) {
			polled.push_back(pollfd{ f.fd, POLLIN, 0 });
		}
		if (poll(polled.data(), polled.size(), 0) <= 0) { // 0: never wait
			return;
		}
		std::vector<Fd> ready;
		for (size_t i = 0; i < polled.size(); i++) {
			if (polled[i].revents & (POLLIN | POLLHUP | POLLERR)) {
				ready.push_back(fds[i]);
			}
		}
		for (Fd &f : ready) {
			if (has_fd(f.fd)) {
				f.fn();
			}
		}
#endif
	}

	bool empty() const { return timers.empty() && fds.empty(); }

private:
	struct Timer {
		uint32_t id;
		uint32_t period_ms;
		uint64_t next_due;
		Callback fn;
	};
	struct Fd {
		int fd;
		Callback fn;
	};

	static uint64_t now_ms() {
		return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}
	bool has_timer(uint32_t p_id) const {
		for (const Timer &t : timers) {
			if (t.id == p_id) {
				return true;
			}
		}
		return false;
	}
	bool has_fd(int p_fd) const {
		for (const Fd &f : fds) {
			if (f.fd == p_fd) {
				return true;
			}
		}
		return false;
	}

	std::vector<Timer> timers;
	std::vector<Fd> fds;
	uint32_t next_id = 1;
};

} // namespace gaph
