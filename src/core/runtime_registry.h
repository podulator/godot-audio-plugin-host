#pragma once

// RuntimeRegistry: the list of live EffectRuntimes made from one effect resource.
//
// Why it exists: you set a parameter on the *resource* (the thing in the inspector), but the
// sound is made by the *instances*, and there can be several of them (one per stereo pair, per
// bus the resource is on). The resource needs a list to send each change to all of them.
//
// Raw pointers, not ownership: each instance owns its own runtime and removes itself from this
// list in its destructor. A runtime in the list is therefore always alive.

#include "effect_runtime.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace gaph {

class RuntimeRegistry {
public:
	// The mutex is a precaution: everything here is main-thread in practice, but the editor can
	// touch resources from worker threads (resource previews, for instance). It is never taken on
	// the audio thread; the audio thread does not know this registry exists.
	//
	// std::lock_guard locks the mutex in its constructor and unlocks it in its destructor, at the
	// closing brace. This is the "RAII" pattern: a resource tied to an object's lifetime, so it
	// is released even if the function returns early or throws.
	void add(EffectRuntime *p_runtime) {
		std::lock_guard<std::mutex> guard(mutex);
		runtimes.push_back(p_runtime);
	}

	void remove(EffectRuntime *p_runtime) {
		std::lock_guard<std::mutex> guard(mutex);
		// The "erase-remove idiom": std::remove shuffles every non-matching element to the front
		// and returns where the leftovers start; erase then chops the leftovers off.
		runtimes.erase(std::remove(runtimes.begin(), runtimes.end(), p_runtime), runtimes.end());
	}

	// Calls p_fn(runtime) for each live runtime. `template <typename F>` lets the caller pass any
	// callable, usually a lambda such as [&](EffectRuntime &r) { ... }.
	template <typename F>
	void for_each(F &&p_fn) {
		std::lock_guard<std::mutex> guard(mutex);
		for (EffectRuntime *r : runtimes) {
			p_fn(*r);
		}
	}

	size_t size() {
		std::lock_guard<std::mutex> guard(mutex);
		return runtimes.size();
	}

private:
	std::mutex mutex;
	std::vector<EffectRuntime *> runtimes;
};

} // namespace gaph
