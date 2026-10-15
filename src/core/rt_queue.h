#pragma once

// RtQueue: how the main thread and the audio thread talk without ever waiting on each other.
//
// THE PROBLEM
// The audio thread has a hard deadline: Godot hands it 512 frames, and at 48 kHz it must hand them
// back within ~10.7 ms, every time, forever. If it misses, you hear a click. So the audio thread
// must never do anything whose duration is unbounded:
//   - lock a mutex      (the main thread might hold it while it does something slow)
//   - allocate memory   (malloc can take a lock internally, or ask the OS for pages)
//   - do I/O or log     (both can block)
// But the main thread still has to send it things, such as "set parameter 7 to 0.5".
//
// THE SOLUTION: a single-producer, single-consumer (SPSC) ring buffer
// A fixed array, allocated once up front, plus two counters. Exactly one thread writes (pushes)
// and exactly one thread reads (pops). Each counter is written by only one side:
//   write_pos: where the producer will put the next item. Only the producer changes it.
//   read_pos:  where the consumer will take the next item. Only the consumer changes it.
// Neither side ever waits. If the queue is full, push() returns false and the caller decides what
// to do; if it is empty, pop() returns false.
//
//     slots:  [ . | A | B | C | . | . | . | . ]
//                   ^           ^
//               read_pos    write_pos        -> A, B, C are waiting to be popped
//
// One slot is always left empty, so "full" (write is just behind read) can be told apart from
// "empty" (write == read). That is why the constructor allocates p_min_capacity + 1.

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace gaph {

template <typename T>
class RtQueue {
	// Items are copied with plain assignment, and nothing runs when one is overwritten. That is
	// only safe for "plain data" types: no std::string, no pointers that own memory.
	static_assert(std::is_trivially_copyable_v<T>, "RtQueue holds trivially copyable values only");

public:
	explicit RtQueue(size_t p_min_capacity) {
		// Round the size up to a power of two, so wrapping around is `index & mask` (one
		// instruction) instead of `index % size` (a division).
		size_t cap = 2;
		while (cap < p_min_capacity + 1) {
			cap <<= 1;
		}
		slots.resize(cap); // The only allocation, on the main thread, before any audio runs.
		mask = cap - 1;
	}

	// Copying a queue would let two objects share one set of counters; forbid it.
	RtQueue(const RtQueue &) = delete;
	RtQueue &operator=(const RtQueue &) = delete;

	// Producer side. Returns false when full: the value is dropped, never overwritten.
	bool push(const T &p_value) {
		// MEMORY ORDERING, the subtle part.
		// CPUs and compilers reorder memory operations for speed. Within one thread you never
		// notice, but another thread can see your writes in a different order from the one you
		// wrote them in. Atomics take an "order" argument that limits this:
		//   relaxed: just make this one load/store indivisible, no ordering promises.
		//   release (on a store): every write I did BEFORE this store becomes visible to any
		//            thread that later sees this store with acquire.
		//   acquire (on a load): once I see the value, I also see everything the other thread
		//            did before its matching release.
		// Here: we write the item into slots[w], THEN publish write_pos with release. The consumer
		// loads write_pos with acquire, so if it sees the new position it is guaranteed to also
		// see the item. Without that pairing it could read a half-written slot.

		// Our own counter: only we write it, so relaxed is enough to read it.
		const size_t w = write_pos.load(std::memory_order_relaxed);
		const size_t next = (w + 1) & mask;
		// The other side's counter: acquire, so we see the slot it freed as really free.
		if (next == read_pos.load(std::memory_order_acquire)) {
			return false; // Full.
		}
		slots[w] = p_value;
		write_pos.store(next, std::memory_order_release); // Publish the item.
		return true;
	}

	// Consumer side: the mirror image of push().
	bool pop(T &r_value) {
		const size_t r = read_pos.load(std::memory_order_relaxed);
		if (r == write_pos.load(std::memory_order_acquire)) {
			return false; // Empty.
		}
		r_value = slots[r];
		// Release: our read of slots[r] is finished before the producer can see that slot as free
		// and overwrite it.
		read_pos.store((r + 1) & mask, std::memory_order_release);
		return true;
	}

	size_t capacity() const { return mask; }

private:
	std::vector<T> slots;
	size_t mask = 0;
	// alignas(64) puts each counter on its own CPU cache line (64 bytes on x86 and most ARM).
	// Two threads writing variables that share a cache line make the cores keep passing that line
	// back and forth ("false sharing"), which is slow even though they never touch the same
	// variable.
	alignas(64) std::atomic<size_t> write_pos{ 0 };
	alignas(64) std::atomic<size_t> read_pos{ 0 };
};

} // namespace gaph
