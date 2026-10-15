// Standalone tests for src/core. Build and run: tests/core/run.sh
#include "core/effect_runtime.h"
#include "core/hosted_effect.h"
#include "core/rt_queue.h"
#include "core/sidechain.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>

using namespace gaph;

static int failures = 0;
#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                 \
		}                                                               \
	} while (0)

// Gain with parameter 7 as the factor; swaps left and right so channel order is checked too.
struct SwapGain : PluginProcessor {
	double gain = 1.0;
	int calls = 0;
	uint32_t max_frames_seen = 0;
	int events_seen = 0;
	bool fail = false;
	bool *detached = nullptr;

	bool process(const AudioBlock &b) override {
		calls++;
		if (b.frames > max_frames_seen) {
			max_frames_seen = b.frames;
		}
		for (uint32_t i = 0; i < b.param_event_count; i++) {
			events_seen++;
			if (b.param_events[i].param_id == 7) {
				gain = b.param_events[i].value;
			}
		}
		if (fail) {
			return false;
		}
		for (uint32_t i = 0; i < b.frames; i++) {
			b.outputs[0][i] = float(b.inputs[1][i] * gain);
			b.outputs[1][i] = float(b.inputs[0][i] * gain);
		}
		return true;
	}
	void detach_from_audio() override {
		if (detached) {
			*detached = true;
		}
	}
};

static std::vector<float> ramp(uint32_t frames) {
	std::vector<float> v(frames * 2);
	for (uint32_t i = 0; i < frames; i++) {
		v[i * 2] = float(i);
		v[i * 2 + 1] = -float(i);
	}
	return v;
}

static void test_queue_basics() {
	RtQueue<int> q(3);
	CHECK(q.capacity() >= 3);
	int v = 0;
	CHECK(!q.pop(v));
	for (int round = 0; round < 10; round++) {
		CHECK(q.push(round));
		CHECK(q.pop(v));
		CHECK(v == round);
	}
	size_t pushed = 0;
	while (q.push(1)) {
		pushed++;
	}
	CHECK(pushed == q.capacity());
}

static void test_queue_threads() {
	RtQueue<uint32_t> q(64);
	const uint32_t N = 200000;
	bool ordered = true;
	std::thread consumer([&] {
		uint32_t expect = 0, v = 0;
		while (expect < N) {
			if (q.pop(v)) {
				if (v != expect) {
					ordered = false;
				}
				expect++;
			}
		}
	});
	for (uint32_t i = 0; i < N;) {
		if (q.push(i)) {
			i++;
		}
	}
	consumer.join();
	CHECK(ordered);
}

static void test_passthrough_without_processor() {
	EffectRuntime rt;
	auto src = ramp(512);
	std::vector<float> dst(src.size(), 99.f);
	rt.process(src.data(), dst.data(), 512);
	CHECK(dst == src);
}

static void test_chunking_and_params() {
	EffectRuntime rt;
	auto *p = new SwapGain();
	rt.exchange_processor(std::unique_ptr<PluginProcessor>(p));
	CHECK(rt.queue_param(7, 2.0));

	const uint32_t frames = 1300; // 512 + 512 + 276
	auto src = ramp(frames);
	std::vector<float> dst(src.size());
	rt.process(src.data(), dst.data(), frames);

	CHECK(p->calls == 3);
	CHECK(p->max_frames_seen == EffectRuntime::MAX_BLOCK);
	CHECK(p->events_seen == 1);
	bool ok = true;
	for (uint32_t i = 0; i < frames; i++) {
		ok = ok && dst[i * 2] == -2.f * float(i) && dst[i * 2 + 1] == 2.f * float(i);
	}
	CHECK(ok);
}

static void test_failure_passes_through_and_detach() {
	EffectRuntime rt;
	bool detached = false;
	auto *p = new SwapGain();
	p->fail = true;
	p->detached = &detached;
	rt.exchange_processor(std::unique_ptr<PluginProcessor>(p));
	auto src = ramp(100);
	std::vector<float> dst(src.size());
	rt.process(src.data(), dst.data(), 100);
	CHECK(dst == src);

	auto old = rt.exchange_processor(nullptr);
	CHECK(detached);
	CHECK(old.get() == p);
	CHECK(!rt.has_processor());
}

// The stale-data guard: a reader that finds no new write since its last read must get silence,
// never the old block again. Without it, a source bus that Godot stops calling mid-sound would
// leave the plugin hearing that block on a loop.
static void test_sidechain_stale_guard() {
	auto ch = sidechain_channel("test");
	CHECK(ch == sidechain_channel("test")); // Same name, same channel.
	SidechainReader reader;
	reader.channel = ch;
	std::vector<float> src(2 * 64, 0.75f), l(64), r(64);

	CHECK(!reader.read(l.data(), r.data(), 64)); // Nothing written yet.
	ch->write(src.data(), 64);
	CHECK(reader.read(l.data(), r.data(), 64));
	CHECK(l[10] == 0.75f && r[10] == 0.75f);
	CHECK(!reader.read(l.data(), r.data(), 64)); // No new write: stale.
	CHECK(l[10] == 0.0f && r[10] == 0.0f);
	ch->write(src.data(), 64);
	CHECK(reader.read(l.data(), r.data(), 64)); // New write: real again.

	// A runtime with a sidechain hands it to the processor as aux input.
	struct AuxProbe : PluginProcessor {
		float seen = -1;
		bool process(const AudioBlock &b) override {
			seen = b.aux_inputs ? b.aux_inputs[0][5] : -1.0f;
			for (uint32_t i = 0; i < b.frames; i++) {
				b.outputs[0][i] = b.inputs[0][i];
				b.outputs[1][i] = b.inputs[1][i];
			}
			return true;
		}
	};
	EffectRuntime rt;
	auto *probe = new AuxProbe();
	rt.exchange_processor(std::unique_ptr<PluginProcessor>(probe));
	rt.set_sidechain(ch);
	auto in = ramp(64);
	std::vector<float> out(in.size());
	ch->write(src.data(), 64);
	rt.process(in.data(), out.data(), 64);
	CHECK(probe->seen == 0.75f);
	rt.process(in.data(), out.data(), 64); // No write in between.
	CHECK(probe->seen == 0.0f);
}

// A plugin whose parameter list changes while it runs. One flag object is shared by every
// instance the factory makes, so the test can flip it from outside.
struct ListShared {
	bool second_list = false;
	bool changed = false;
};
struct ListPlugin : PluginProcessor {
	ListShared *shared;
	explicit ListPlugin(ListShared *p_shared) :
			shared(p_shared) {}
	bool process(const AudioBlock &) override { return false; }
	std::vector<ParamInfo> read_params() const override {
		std::vector<ParamInfo> v(2);
		v[0].id = 1;
		v[0].name = "Kept";
		v[1].id = shared->second_list ? 3 : 2;
		v[1].name = shared->second_list ? "New" : "Removed";
		return v;
	}
	bool take_params_changed() override {
		const bool c = shared->changed;
		shared->changed = false;
		return c;
	}
};

static void test_param_list_change() {
	ListShared shared;
	HostedEffect h(HostedEffect::AudioLock{ [] {}, [] {} });
	ListPlugin probe(&shared);
	h.set_reference(&probe);
	h.set_plugin([&](std::string &) { return std::unique_ptr<PluginProcessor>(new ListPlugin(&shared)); }, probe.read_params());
	EffectRuntime rt;
	h.attach(rt);
	CHECK(!rt.has_processor()); // No spare yet: attach must not build one under the lock.
	h.idle(); // ...it is built here, outside the lock, and a spare is made.
	CHECK(rt.has_processor() && h.active_instance_count() == 1);
	EffectRuntime rt2;
	h.attach(rt2);
	CHECK(rt2.has_processor()); // Took the spare: plugin from the very first block.
	h.detach(rt2);
	CHECK(!rt2.has_processor()); // Moved to the graveyard, destroyed in the next idle().
	h.set_param(1, 0.25);
	h.set_param(2, 0.75);
	h.idle();
	CHECK(!h.take_params_list_changed());

	shared.second_list = true;
	shared.changed = true; // As if the plugin had called params.rescan(INFO).
	h.idle();
	CHECK(h.take_params_list_changed());
	CHECK(!h.take_params_list_changed()); // Reported once.
	CHECK(h.find_param(3) != nullptr && h.find_param(2) == nullptr);
	CHECK(h.is_param_set(1) && h.get_param(1) == 0.25); // Survives.
	CHECK(!h.is_param_set(2)); // Gone with its parameter.
	h.detach(rt);
}

// Records the absolute sample position of every event it receives.
struct EventRecorder : PluginProcessor {
	uint64_t position = 0;
	std::vector<int64_t> at;
	bool process(const AudioBlock &b) override {
		for (uint32_t i = 0; i < b.param_event_count; i++) {
			at.push_back(int64_t(position + b.param_events[i].frame_offset));
		}
		for (uint32_t i = 0; i < b.frames; i++) {
			b.outputs[0][i] = b.inputs[0][i];
			b.outputs[1][i] = b.inputs[1][i];
		}
		position += b.frames;
		return true;
	}
};

// Godot's real cadence, replayed with exact times: the driver asks for 1024 frames every
// 21.33 ms, which AudioServer mixes as two 512-frame blocks back to back. Meanwhile the game
// makes a change every 5 ms. Each must land exactly 240 samples (5 ms at 48 kHz) after the last;
// at frame 0 they would land 0 or 512 apart.
static void test_sample_accurate_spacing() {
	const double rate = 48000;
	EffectRuntime rt;
	rt.set_sample_rate(rate);
	auto *rec = new EventRecorder();
	rt.exchange_processor(std::unique_ptr<PluginProcessor>(rec));
	std::vector<float> in(2 * 512), out(2 * 512);

	const int64_t t0 = 1000000;
	const int64_t period = int64_t(1024 * 1e6 / rate); // 21333 us per driver callback
	int64_t next_event = t0 + 3000;
	for (int cb = 0; cb < 60; cb++) {
		const int64_t now = t0 + cb * period + (cb % 3) * 700; // plus some scheduling jitter
		while (next_event <= now) {
			rt.queue_param(1, 0.5, next_event); // As if made at that moment.
			next_event += 5000;
		}
		rt.process_at(in.data(), out.data(), 512, now);
		rt.process_at(in.data(), out.data(), 512, now + 40); // the burst's second block
	}
	int bad = 0;
	for (size_t i = 20; i < rec->at.size(); i++) { // after the clock has settled
		const int64_t gap = rec->at[i] - rec->at[i - 1];
		if (gap < 239 || gap > 241) {
			bad++;
		}
	}
	CHECK(rec->at.size() > 200);
	CHECK(bad == 0);
	if (bad) {
		std::printf("      %d of %zu gaps off 240\n", bad, rec->at.size() - 20);
	}

	// A change scheduled 100 ms ahead lands 100 ms (4800 samples) after an immediate one.
	// Two more callbacks first, so changes still pending from above are delivered and not counted.
	for (int cb = 60; cb < 62; cb++) {
		rt.process_at(in.data(), out.data(), 512, t0 + cb * period);
		rt.process_at(in.data(), out.data(), 512, t0 + cb * period + 40);
	}
	rec->at.clear();
	const int64_t now = t0 + 62 * period;
	rt.queue_param(1, 0.1, now - 100);
	rt.queue_param(1, 0.2, now - 100 + 100000);
	for (int cb = 62; cb < 72; cb++) {
		const int64_t t = t0 + cb * period;
		rt.process_at(in.data(), out.data(), 512, t);
		rt.process_at(in.data(), out.data(), 512, t + 40);
	}
	if (rec->at.size() != 2) {
		std::printf("      scheduled test: %zu events:", rec->at.size());
		for (int64_t a : rec->at) {
			std::printf(" %lld", (long long)a);
		}
		std::printf("\n");
	}
	CHECK(rec->at.size() == 2 && std::abs((rec->at[1] - rec->at[0]) - 4800) <= 1);
}

// The clock still settling while changes wait: blocks arrive 1.5 ms late for a while, two changes
// are scheduled 100 ms apart, and then one block arrives on time, which lowers the clock's anchor
// by 1.5 ms between them. Each change is placed once, when it arrives, so the pair keeps its
// spacing exactly. (Re-placed every block, as before, the second landed 71 samples late here; on
// CI's Windows runner, 4 and 22 samples.)
static void test_spacing_while_clock_settles() {
	const double rate = 48000;
	EffectRuntime rt;
	rt.set_sample_rate(rate);
	auto *rec = new EventRecorder();
	rt.exchange_processor(std::unique_ptr<PluginProcessor>(rec));
	std::vector<float> in(2 * 512), out(2 * 512);
	const int64_t t0 = 5000000;
	const int64_t period = int64_t(512 * 1e6 / rate); // one block per callback here: 10667 us
	const int64_t late = 1500;
	int cb = 0;
	for (; cb < 20; cb++) {
		rt.process_at(in.data(), out.data(), 512, t0 + cb * period + late);
	}
	const int64_t now = t0 + cb * period + late;
	rt.queue_param(1, 0.0, now + 30000);
	rt.queue_param(1, 1.0, now + 130000);
	for (int i = 0; i < 20; i++, cb++) {
		const bool on_time = i == 5; // between the two changes: the anchor drops by 1.5 ms
		rt.process_at(in.data(), out.data(), 512, t0 + cb * period + (on_time ? 0 : late));
	}
	CHECK(rec->at.size() == 2 && rec->at[1] - rec->at[0] == 4800);
	if (rec->at.size() == 2 && rec->at[1] - rec->at[0] != 4800) {
		std::printf("      the dip lasted %lld samples, not 4800\n", (long long)(rec->at[1] - rec->at[0]));
	}
}

int main() {
	test_queue_basics();
	test_queue_threads();
	test_passthrough_without_processor();
	test_chunking_and_params();
	test_failure_passes_through_and_detach();
	test_sidechain_stale_guard();
	test_param_list_change();
	test_sample_accurate_spacing();
	test_spacing_while_clock_settles();
	std::printf(failures ? "%d FAILURES\n" : "all core tests passed\n", failures);
	return failures ? 1 : 0;
}
