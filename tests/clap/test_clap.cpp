// Tests the CLAP host against real plugins, with no Godot involved.
// Build and run: tests/clap/run.sh
//
// Each test drives a ClapProcessor the way EffectRuntime does: planar buffers, 512-frame blocks,
// parameter events at frame 0. If one fails here, the bug is in the CLAP layer, not in Godot.
#include "clap/clap_processor.h"


#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace gaph;

static int failures = 0;
static int skipped = 0;
#define CHECK(cond, ...)                          \
	do {                                          \
		std::printf((cond) ? "PASS  " : "FAIL  "); \
		std::printf(__VA_ARGS__);                 \
		std::printf("\n");                        \
		if (!(cond)) {                            \
			failures++;                           \
		}                                         \
	} while (0)

static const double RATE = 48000.0;
static const uint32_t BLOCK = 512;

// Owns planar stereo in/out buffers for one block and runs it through a processor.
struct Harness {
	std::vector<float> in_l, in_r, out_l, out_r;
	const float *in_ptrs[2];
	float *out_ptrs[2];
	Harness() :
			in_l(BLOCK), in_r(BLOCK), out_l(BLOCK), out_r(BLOCK) {
		in_ptrs[0] = in_l.data();
		in_ptrs[1] = in_r.data();
		out_ptrs[0] = out_l.data();
		out_ptrs[1] = out_r.data();
	}
	bool run(PluginProcessor &p, const std::vector<ParamEvent> &events = {}) {
		AudioBlock b;
		b.frames = BLOCK;
		b.channels = 2;
		b.inputs = in_ptrs;
		b.outputs = out_ptrs;
		b.param_events = events.data();
		b.param_event_count = uint32_t(events.size());
		return p.process(b);
	}
};

static std::unique_ptr<ClapProcessor> load(const std::string &path, std::string &err) {
	auto lib = ClapLibrary::open(path, err);
	if (!lib) {
		return nullptr;
	}
	return ClapProcessor::create(lib, "", RATE, BLOCK, true, err);
}

static bool exists(const std::string &p) {
	if (!std::filesystem::exists(p)) {
		std::printf("SKIP  %s not found\n", p.c_str());
		skipped++;
		return false;
	}
	return true;
}

// A reverb fed one click must keep ringing after the input goes silent, and must react when a
// parameter changes. Proves process() really runs the plugin, and that events reach it.
static void test_reverb(const std::string &path) {
	if (!exists(path)) {
		return;
	}
	std::string err;
	auto p = load(path, err);
	CHECK(p != nullptr, "%s loads%s%s", std::filesystem::path(path).filename().c_str(), err.empty() ? "" : ": ", err.c_str());
	if (!p) {
		return;
	}
	std::printf("      descriptor: %s (%s), GUI: %s\n", p->get_descriptor()->name, p->get_descriptor()->id, p->gui_available() ? "yes" : "no");
	auto params = p->read_params();
	std::printf("      %zu params:", params.size());
	for (size_t i = 0; i < params.size() && i < 6; i++) {
		std::printf(" '%s'", params[i].name.c_str());
	}
	std::printf("%s\n", params.size() > 6 ? " ..." : "");

	Harness h;
	// Block 0: a single click at frame 0. Blocks 1..: silence.
	double tail_energy = 0;
	for (int block = 0; block < 100; block++) {
		std::fill(h.in_l.begin(), h.in_l.end(), 0.f);
		std::fill(h.in_r.begin(), h.in_r.end(), 0.f);
		if (block == 0) {
			h.in_l[0] = h.in_r[0] = 1.f;
		}
		if (!h.run(*p)) {
			CHECK(false, "reverb process() returned false at block %d", block);
			return;
		}
		if (block >= 20) { // ~0.2 s after the click: well past any dry signal.
			for (uint32_t i = 0; i < BLOCK; i++) {
				tail_energy += h.out_l[i] * h.out_l[i] + h.out_r[i] * h.out_r[i];
			}
		}
	}
	CHECK(tail_energy > 1e-6, "reverb tail still ringing 0.2-1 s after a click (energy %.3g)", tail_energy);

	// Second pass on a fresh instance: turn every "Level" except Dry down to its minimum through
	// parameter events, the same path the Godot effect uses. The wet tail should all but vanish.
	auto q = load(path, err);
	std::vector<ParamEvent> events;
	for (const ParamInfo &info : q->read_params()) {
		if (info.name.find("Level") != std::string::npos && info.name.find("Dry") == std::string::npos) {
			events.push_back(ParamEvent{ info.id, 0, info.min_value });
			std::printf("      set '%s' to %g\n", info.name.c_str(), info.min_value);
		}
	}
	double quiet_energy = 0;
	for (int block = 0; block < 100; block++) {
		std::fill(h.in_l.begin(), h.in_l.end(), 0.f);
		std::fill(h.in_r.begin(), h.in_r.end(), 0.f);
		if (block == 0) {
			h.in_l[0] = h.in_r[0] = 1.f;
		}
		h.run(*q, block == 0 ? events : std::vector<ParamEvent>{});
		if (block >= 20) {
			for (uint32_t i = 0; i < BLOCK; i++) {
				quiet_energy += h.out_l[i] * h.out_l[i] + h.out_r[i] * h.out_r[i];
			}
		}
	}
	CHECK(!events.empty() && quiet_energy < tail_energy * 0.01,
			"parameter events reach the plugin: wet levels at minimum cut the tail %.3g -> %.3g", tail_energy, quiet_energy);
}

// Our own test plugin (tests/plugins/gaph_test_plugin.c): exact arithmetic, so every check is
// exact equality. It is built with the tests, so these never skip, on any platform, CI included.
//   out = main * gain + sidechain * mix;  gain id 1 (default 1), mix id 2 (default 0)
static void test_gaph_plugin(const std::string &path) {
	if (!exists(path)) {
		return;
	}
	std::string err;
	auto p = load(path, err);
	CHECK(p != nullptr, "test plugin loads%s%s", err.empty() ? "" : ": ", err.c_str());
	if (!p) {
		return;
	}
	CHECK(p->read_params().size() == 2 && p->get_input_ports().size() == 2, "test plugin: 2 params, main + sidechain inputs");

	std::vector<float> il(BLOCK), ir(BLOCK), al(BLOCK), ar(BLOCK), ol(BLOCK), orr(BLOCK);
	for (uint32_t i = 0; i < BLOCK; i++) {
		il[i] = 0.25f + 0.001f * float(i);
		ir[i] = -0.5f + 0.0005f * float(i);
		al[i] = 0.125f;
		ar[i] = -0.0625f;
	}
	const float *ins[2] = { il.data(), ir.data() };
	const float *aux[2] = { al.data(), ar.data() };
	float *outs[2] = { ol.data(), orr.data() };
	auto run = [&](const std::vector<ParamEvent> &ev) {
		AudioBlock b;
		b.frames = BLOCK;
		b.channels = 2;
		b.inputs = ins;
		b.outputs = outs;
		b.aux_inputs = aux;
		b.aux_channels = 2;
		b.param_events = ev.data();
		b.param_event_count = uint32_t(ev.size());
		return p->process(b);
	};
	auto all = [&](auto fn) {
		for (uint32_t i = 0; i < BLOCK; i++) {
			if (!fn(i)) {
				return false;
			}
		}
		return true;
	};

	run({});
	CHECK(all([&](uint32_t i) { return ol[i] == il[i] && orr[i] == ir[i]; }), "test plugin: defaults are an exact null test");

	run({ ParamEvent{ 1, 0, 0.5, 0 } });
	CHECK(all([&](uint32_t i) { return ol[i] == il[i] * 0.5f && orr[i] == ir[i] * 0.5f; }), "test plugin: gain 0.5 is exact");

	run({ ParamEvent{ 1, 0, 1.0, 0 }, ParamEvent{ 2, 0, 1.0, 0 } });
	CHECK(all([&](uint32_t i) { return ol[i] == il[i] + al[i] && orr[i] == ir[i] + ar[i]; }), "test plugin: sidechain reaches the plugin, exactly");

	// A single event, at sample 100: the change must land exactly there (no workaround needed).
	run({ ParamEvent{ 2, 0, 0.0, 0 } });
	run({ ParamEvent{ 1, 100, 0.0, 0 } });
	CHECK(all([&](uint32_t i) { return i < 100 ? ol[i] == il[i] : ol[i] == 0.0f; }), "test plugin: an event at sample 100 takes effect at sample 100");

	// The instance's gain is 0 now (set at sample 100 above); a fresh instance starts at 1, so a
	// restored 0 can only have come from the saved state.
	std::vector<uint8_t> state;
	CHECK(p->save_state(state) && state.size() == 16, "test plugin: state saves (%zu bytes)", state.size());
	auto q = load(path, err);
	double g = -1;
	CHECK(q && q->get_param_value(1, g) && g == 1.0, "test plugin: a fresh instance starts at gain 1");
	CHECK(q && q->load_state(state) && q->get_param_value(1, g) && g == 0.0, "test plugin: state restores into a fresh instance (gain %.2f)", g);
	std::string text;
	CHECK(p->value_to_text(2, 0.5, text) && text == "50%", "test plugin: value text '%s'", text.c_str());
}

// argv[1]: our CLAP test plugin, argv[2]: the CLAP example (both built by run.sh). The reverb tests
// use Dragonfly Reverb, a third-party plugin, and skip themselves when it is not installed.
int main(int argc, char **argv) {
	test_gaph_plugin(argc > 1 ? argv[1] : "");
	// The CLAP example (examples/clap-gain): load it and check the null test and a gain change.
	if (argc > 2 && exists(argv[2])) {
		std::string err;
		auto g = load(argv[2], err);
		CHECK(g && g->read_params().size() == 1, "clap example gain loads, one parameter");
		if (g) {
			std::vector<float> l(BLOCK, 0.5f), r(BLOCK, -0.25f), ol(BLOCK), orr(BLOCK);
			const float *ins[2] = { l.data(), r.data() };
			float *outs[2] = { ol.data(), orr.data() };
			std::vector<ParamEvent> ev = { ParamEvent{ 0, 100, 1.5, 0 } };
			AudioBlock b;
			b.frames = BLOCK;
			b.channels = 2;
			b.inputs = ins;
			b.outputs = outs;
			b.param_events = ev.data();
			b.param_event_count = 1;
			bool ok = g->process(b);
			for (uint32_t i = 0; i < BLOCK; i++) {
				ok = ok && ol[i] == (i < 100 ? 0.5f : 0.75f) && orr[i] == (i < 100 ? -0.25f : -0.375f);
			}
			CHECK(ok, "clap example gain: unity, then 1.5x from sample 100, exactly");
		}
	}
	test_reverb("/usr/lib/clap/DragonflyHallReverb.clap");
	test_reverb("/usr/lib/clap/DragonflyPlateReverb.clap");
	std::printf(failures ? "%d FAILURES (%d skipped)\n" : "all CLAP tests passed (%d skipped)\n", failures ? failures : skipped, skipped);
	return failures ? 1 : 0;
}
