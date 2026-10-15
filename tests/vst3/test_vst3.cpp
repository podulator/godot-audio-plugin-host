// Tests the VST3 host against real plugins, with no Godot involved. Run: tests/vst3/run.sh
// Mirrors tests/clap/test_clap.cpp, so a difference between the two points at the format layer.
#include "vst3/vst3_processor.h"
#include "public.sdk/source/vst/moduleinfo/moduleinfocreator.h"

#include <fstream>
#include <sstream>


#include <cmath>
#include <cstdio>
#include <filesystem>

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

// Our VST3 test plugin (tests/plugins/gaph_test_vst3.cpp): out = main * gain + sidechain * mix,
// with Gain normalised (0.5 = unity), sample-accurate changes, and state. Built with the tests,
// so this never skips. Every check is exact equality, as with the CLAP test plugin.
static void test_gaph_vst3(const std::string &path) {
	if (!std::filesystem::exists(path)) {
		std::printf("SKIP  %s not found\n", path.c_str());
		skipped++;
		return;
	}
	std::string err;
	auto mod = vst3_open_module(path, err);
	auto p = mod ? Vst3Processor::create(mod, "", RATE, BLOCK, true, err) : nullptr;
	CHECK(p != nullptr, "test plugin: component + controller created and started%s%s", err.empty() ? "" : ": ", err.c_str());
	if (!p) {
		return;
	}
	auto params = p->read_params();
	CHECK(params.size() == 2 && p->get_input_buses().size() == 2, "test plugin: 2 params, main + sidechain buses");

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
	run({ ParamEvent{ 1, 0, 0.25, 0 } });
	CHECK(all([&](uint32_t i) { return ol[i] == il[i] * 0.5f && orr[i] == ir[i] * 0.5f; }), "test plugin: gain 0.5 (normalised 0.25) is exact");
	run({ ParamEvent{ 1, 0, 0.5, 0 }, ParamEvent{ 2, 0, 1.0, 0 } });
	CHECK(all([&](uint32_t i) { return ol[i] == il[i] + al[i] && orr[i] == ir[i] + ar[i]; }), "test plugin: the sidechain bus reaches the plugin, exactly");
	run({ ParamEvent{ 2, 0, 0.0, 0 } });
	run({ ParamEvent{ 1, 100, 0.0, 0 } });
	CHECK(all([&](uint32_t i) { return i < 100 ? ol[i] == il[i] : ol[i] == 0.0f; }), "test plugin: a change at sample 100 takes effect at sample 100");

	// Gain is 0 now; a fresh instance starts at unity, so a restored 0 can only come from the state.
	std::vector<uint8_t> state;
	CHECK(p->save_state(state) && !state.empty(), "test plugin: state saves (%zu bytes, component + controller)", state.size());
	auto q = Vst3Processor::create(mod, "", RATE, BLOCK, true, err);
	double g = -1;
	CHECK(q && q->get_param_value(1, g) && g == 0.5, "test plugin: a fresh instance starts at unity (0.5)");
	CHECK(q && q->load_state(state) && q->get_param_value(1, g) && g == 0.0, "test plugin: state restores into a fresh instance (gain %.2f)", g);
	std::string text;
	CHECK(p->value_to_text(2, 0.5, text) && text == "50%", "test plugin: value text '%s'", text.c_str());
	CHECK(p->value_to_text(1, 0.5, text) && text == "1.000 x", "test plugin: value text with units joined '%s'", text.c_str());
}

// An instrument has no kVstAudioEffectClass class. The host must say so, not crash.
static void test_instrument_rejected(const std::string &path) {
	if (!std::filesystem::exists(path)) {
		std::printf("SKIP  %s not found\n", path.c_str());
		skipped++;
		return;
	}
	std::string err;
	auto mod = vst3_open_module(path, err);
	CHECK(mod != nullptr, "%s loads as a module", std::filesystem::path(path).filename().c_str());
	if (!mod) {
		return;
	}
	for (const auto &c : mod->getFactory().classInfos()) {
		std::printf("      class: %-20s %s  [%s]\n", c.name().c_str(), c.category().c_str(), c.subCategoriesString().c_str());
	}
	auto p = Vst3Processor::create(mod, "", RATE, BLOCK, true, err);
	CHECK(p == nullptr && !err.empty(), "instrument refused cleanly: '%s'", err.c_str());
}

namespace fs = std::filesystem;

// moduleinfo.json: proof that a scan reads it instead of loading the binary. A copy of our test
// plugin's bundle gets a moduleinfo.json made by the SDK's own creator, with the plugin renamed in it,
// and then loses its binary. If the scan still lists the plugin, under the new name, it cannot
// have loaded anything. A second copy with a broken moduleinfo.json must fall back to the binary.
static void test_moduleinfo(const std::string &path) {
	if (!fs::exists(path)) {
		std::printf("SKIP  %s not found\n", path.c_str());
		skipped++;
		return;
	}
	std::string err;
	auto mod = vst3_open_module(path, err);
	std::stringstream json;
	Steinberg::ModuleInfoLib::outputJson(Steinberg::ModuleInfoLib::createModuleInfo(*mod, false), json);
	std::string text = json.str();
	const std::string from = "\"Name\": \"GAPH Test VST3\"";
	const size_t at = text.find(from);
	CHECK(at != std::string::npos, "SDK creator wrote a moduleinfo.json naming the test plugin");
	if (at == std::string::npos) {
		return;
	}
	text.replace(at, from.size(), "\"Name\": \"GAPH Test VST3 (from moduleinfo)\"");

	const fs::path tmp = fs::temp_directory_path() / "gaph_moduleinfo_test";
	fs::remove_all(tmp);
	for (const char *variant : { "good", "broken" }) {
		const fs::path bundle = tmp / variant / "gaph_test_vst3.vst3";
		fs::create_directories(bundle.parent_path());
		fs::copy(path, bundle, fs::copy_options::recursive);
		fs::create_directories(bundle / "Contents" / "Resources");
		std::ofstream(bundle / "Contents" / "Resources" / "moduleinfo.json") << (std::string(variant) == "good" ? text : "{ not json");
	}
	fs::remove_all(tmp / "good" / "gaph_test_vst3.vst3" / "Contents" / "x86_64-linux"); // no binary at all

	bool from_info = false;
	auto good = vst3_scan_bundle((tmp / "good" / "gaph_test_vst3.vst3").string(), from_info, err);
	CHECK(from_info && good.size() == 1 && good[0].name == "GAPH Test VST3 (from moduleinfo)",
			"scan read moduleinfo.json, with the binary deleted: '%s'", good.empty() ? "" : good[0].name.c_str());
	CHECK(!good.empty() && good[0].id == "67617068746573747673743367756901", "  ...with the same class id as the binary reports");

	err.clear();
	auto broken = vst3_scan_bundle((tmp / "broken" / "gaph_test_vst3.vst3").string(), from_info, err);
	if (broken.size() != 1) {
		std::printf("      broken variant: from_info=%d, %zu entries, error '%s'\n", int(from_info), broken.size(), err.c_str());
	}
	CHECK(!from_info && broken.size() == 1 && broken[0].name == "GAPH Test VST3", "broken moduleinfo.json: falls back to loading the binary");
	fs::remove_all(tmp);
}

// The VST 3 example plugin (examples/vst3-gain): one parameter, gain = value * 2. Built with the
// tests, so this never skips on any platform. Every check is exact equality.
static void test_example_gain(const std::string &path) {
	if (!std::filesystem::exists(path)) {
		std::printf("SKIP  %s not found\n", path.c_str());
		skipped++;
		return;
	}
	std::string err;
	auto mod = vst3_open_module(path, err);
	auto p = mod ? Vst3Processor::create(mod, "", RATE, BLOCK, true, err) : nullptr;
	CHECK(p != nullptr, "example gain loads and starts%s%s", err.empty() ? "" : ": ", err.c_str());
	if (!p) {
		return;
	}
	auto params = p->read_params();
	CHECK(params.size() == 1 && params[0].name == "Gain" && params[0].default_value == 0.5, "example gain: one 'Gain' parameter, default 0.5");
	std::vector<float> l(BLOCK), r(BLOCK), ol(BLOCK), orr(BLOCK);
	for (uint32_t i = 0; i < BLOCK; i++) {
		l[i] = 0.25f + 0.001f * float(i);
		r[i] = -0.5f + 0.0005f * float(i);
	}
	const float *ins[2] = { l.data(), r.data() };
	float *outs[2] = { ol.data(), orr.data() };
	auto run = [&](std::vector<ParamEvent> ev) {
		AudioBlock b;
		b.frames = BLOCK;
		b.channels = 2;
		b.inputs = ins;
		b.outputs = outs;
		b.param_events = ev.data();
		b.param_event_count = uint32_t(ev.size());
		return p->process(b);
	};
	bool ok = run({});
	for (uint32_t i = 0; i < BLOCK; i++) {
		ok = ok && ol[i] == l[i] && orr[i] == r[i];
	}
	CHECK(ok, "example gain: the default is an exact null test");
	ok = run({ ParamEvent{ params[0].id, 100, 0.25, 0 } });
	for (uint32_t i = 0; i < BLOCK; i++) {
		ok = ok && ol[i] == (i < 100 ? l[i] : l[i] * 0.5f);
	}
	CHECK(ok, "example gain: a change at sample 100 lands at sample 100, exactly");
	std::string text;
	CHECK(p->value_to_text(params[0].id, 0.25, text) && text == "0.50 x", "example gain: value text '%s' (units joined by the host)", text.c_str());
}

int main(int argc, char **argv) {
	auto bundles = vst3_find_bundles({});
	std::printf("      scan found %zu bundles\n", bundles.size());
	for (const auto &b : bundles) {
		std::printf("      %s\n", b.c_str());
	}
	// argv[1]: our VST3 test plugin, argv[2]: the VST3 example (both built by run.sh). The
	// instrument test uses a third-party plugin and skips itself when it is not installed.
	test_example_gain(argc > 2 ? argv[2] : "");
	test_gaph_vst3(argc > 1 ? argv[1] : "");
	test_instrument_rejected("/usr/lib/vst3/OneTrick URCHIN.vst3");
	test_moduleinfo(argc > 1 ? argv[1] : "");
	std::printf(failures ? "%d FAILURES (%d skipped)\n" : "all VST3 tests passed (%d skipped)\n", failures ? failures : skipped, skipped);
	return failures ? 1 : 0;
}
