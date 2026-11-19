#include "midi_clip_resource.h"

#include "core/midi_file.h"
#include "core/midi_values.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;
using gaph::midi::ClipEvent;
using gaph::midi::Kind;

namespace {

// Back from the inside resolution to a version's range, as the driver takes values (signed for
// pitch bend).
int64_t from_inside(Kind p_kind, uint32_t p_value, int p_version) {
	if (p_kind == Kind::PROGRAM) {
		return p_value;
	}
	const unsigned inside = gaph::midi::midi2_bits(p_kind);
	const unsigned bits = p_version == 1 ? gaph::midi::midi1_bits(p_kind) : inside;
	const int64_t v = int64_t(gaph::midi::scale_down(p_value, inside, bits));
	return p_kind == Kind::PITCH_BEND ? v - (int64_t(1) << (bits - 1)) : v;
}

const int FIELDS = 7; // beat, length, track, kind, channel, index, value

} // namespace

MidiClip::MidiClip() :
		clip(std::make_shared<gaph::midi::Clip>()) {}

gaph::midi::Clip &MidiClip::edit() {
	if (clip.use_count() > 1) {
		clip = std::make_shared<gaph::midi::Clip>(*clip);
	}
	return *clip;
}

void MidiClip::set_clip(gaph::midi::Clip &&p_clip) {
	clip = std::make_shared<gaph::midi::Clip>(std::move(p_clip));
	emit_changed();
}

// --- Files ----------------------------------------------------------------------------------------

Ref<MidiClip> MidiClip::from_bytes(const PackedByteArray &p_bytes) {
	gaph::midi::Clip c;
	std::string err;
	ERR_FAIL_COND_V_MSG(!gaph::midi::read_midi_file(p_bytes.ptr(), size_t(p_bytes.size()), c, err), Ref<MidiClip>(),
			String("MidiClip: ") + String::utf8(err.c_str()) + ".");
	Ref<MidiClip> out;
	out.instantiate();
	out->set_clip(std::move(c));
	return out;
}

Ref<MidiClip> MidiClip::load_file(const String &p_path) {
	const PackedByteArray bytes = FileAccess::get_file_as_bytes(p_path);
	ERR_FAIL_COND_V_MSG(bytes.is_empty(), Ref<MidiClip>(), vformat("MidiClip: cannot read '%s' (%s).", p_path, UtilityFunctions::error_string(FileAccess::get_open_error())));
	Ref<MidiClip> out = from_bytes(bytes);
	ERR_FAIL_COND_V_MSG(out.is_null(), out, vformat("MidiClip: '%s' is not a MIDI file it can read.", p_path));
	return out;
}

// --- Building -------------------------------------------------------------------------------------

bool MidiClip::check(int p_channel, int p_index, int p_track) const {
	ERR_FAIL_COND_V_MSG(p_channel < 1 || p_channel > 16, false, vformat("MidiClip: channel %d is not 1-16.", p_channel));
	ERR_FAIL_COND_V_MSG(p_index < 0 || p_index > 127, false, vformat("MidiClip: %d is not 0-127.", p_index));
	ERR_FAIL_COND_V_MSG(p_track < 0 || p_track > 65535, false, "MidiClip: a track is 0 or more.");
	return true;
}

void MidiClip::add_note(double p_beat, int p_channel, int p_key, int64_t p_velocity, double p_length, int p_track) {
	bool ok = false;
	const uint32_t v = gaph::midi::to_inside(Kind::NOTE_ON, p_velocity, clip->midi_version, ok);
	ERR_FAIL_COND_MSG(!ok || p_velocity == 0, vformat("MidiClip: velocity %d is outside 1 to %d (MIDI %d.0).", p_velocity, gaph::midi::max_value(Kind::NOTE_ON, clip->midi_version), clip->midi_version));
	ERR_FAIL_COND_MSG(!(p_beat >= 0) || !(p_length >= 0), "MidiClip: a note's beat and length are 0 or more.");
	if (!check(p_channel, p_key, p_track)) {
		return;
	}
	ClipEvent e;
	e.beat = p_beat, e.length = p_length, e.track = uint16_t(p_track), e.kind = Kind::NOTE_ON, e.channel = uint8_t(p_channel), e.index = uint16_t(p_key), e.value = v;
	gaph::midi::Clip &c = edit();
	c.events.push_back(e);
	while (c.track_names.size() <= size_t(p_track)) {
		c.track_names.emplace_back();
	}
	c.length = std::max(c.length, p_beat + p_length);
	c.sort();
	emit_changed();
}

void MidiClip::add_event(double p_beat, int p_kind, int p_channel, int p_index, int64_t p_value, int p_track) {
	const Kind kind = Kind(p_kind);
	ERR_FAIL_COND_MSG(kind != Kind::CONTROL && kind != Kind::PITCH_BEND && kind != Kind::POLY_PRESSURE && kind != Kind::CHANNEL_PRESSURE && kind != Kind::PROGRAM,
			"MidiClip: add_event() takes EVENT_CONTROL, EVENT_PITCH_BEND, EVENT_POLY_PRESSURE, EVENT_CHANNEL_PRESSURE or EVENT_PROGRAM (notes: add_note()).");
	ERR_FAIL_COND_MSG(!(p_beat >= 0), "MidiClip: an event's beat is 0 or more.");
	if (!check(p_channel, kind == Kind::PROGRAM ? int(p_value & 0x7F) : p_index, p_track)) {
		return;
	}
	uint32_t v = 0;
	if (kind == Kind::PROGRAM) {
		ERR_FAIL_COND_MSG(p_value < 0 || p_value > 127, "MidiClip: a program is 0-127.");
		v = uint32_t(p_value);
	} else {
		bool ok = false;
		v = gaph::midi::to_inside(kind, p_value, clip->midi_version, ok);
		ERR_FAIL_COND_MSG(!ok, vformat("MidiClip: %d is outside MIDI %d.0's range for this message.", p_value, clip->midi_version));
	}
	ClipEvent e;
	e.beat = p_beat, e.track = uint16_t(p_track), e.kind = kind, e.channel = uint8_t(p_channel), e.index = uint16_t(p_index), e.value = v;
	gaph::midi::Clip &c = edit();
	c.events.push_back(e);
	while (c.track_names.size() <= size_t(p_track)) {
		c.track_names.emplace_back();
	}
	c.length = std::max(c.length, p_beat);
	c.sort();
	emit_changed();
}

void MidiClip::set_tempo(double p_beat, double p_bpm) {
	ERR_FAIL_COND_MSG(!(p_bpm > 0) || !(p_beat >= 0), "MidiClip: a tempo is above 0 bpm, at beat 0 or later.");
	gaph::midi::Clip &c = edit();
	c.tempos.erase(std::remove_if(c.tempos.begin(), c.tempos.end(), [&](const gaph::midi::ClipTempo &t) { return t.beat == p_beat; }), c.tempos.end());
	c.tempos.push_back(gaph::midi::ClipTempo{ p_beat, uint32_t(std::llround(6e9 / p_bpm)) });
	c.sort();
	emit_changed();
}

void MidiClip::set_meter(double p_beat, int p_numerator, int p_denominator) {
	ERR_FAIL_COND_MSG(p_numerator < 1 || p_numerator > 255 || p_denominator < 1 || p_denominator > 128 || (p_denominator & (p_denominator - 1)),
			"MidiClip: a meter is 1-255 over a power of two (1-128).");
	gaph::midi::Clip &c = edit();
	c.meters.erase(std::remove_if(c.meters.begin(), c.meters.end(), [&](const gaph::midi::ClipMeter &m) { return m.beat == p_beat; }), c.meters.end());
	c.meters.push_back(gaph::midi::ClipMeter{ p_beat, uint8_t(p_numerator), uint8_t(p_denominator) });
	c.sort();
	emit_changed();
}

void MidiClip::clear() {
	gaph::midi::Clip &c = edit();
	const int version = c.midi_version;
	c = gaph::midi::Clip{};
	c.midi_version = version;
	emit_changed();
}

// --- Properties and reading -----------------------------------------------------------------------

void MidiClip::set_midi_version(int p_version) {
	ERR_FAIL_COND_MSG(p_version != 1 && p_version != 2, "MidiClip: midi_version is 1 or 2.");
	if (p_version != clip->midi_version) {
		edit().midi_version = p_version; // values are held at full resolution: nothing to convert
		emit_changed();
	}
}

void MidiClip::set_length(double p_beats) {
	ERR_FAIL_COND_MSG(!(p_beats >= 0), "MidiClip: a length is 0 or more beats.");
	edit().length = p_beats;
	emit_changed();
}

void MidiClip::set_track_names(const PackedStringArray &p_names) {
	gaph::midi::Clip &c = edit();
	c.track_names.clear();
	for (int i = 0; i < p_names.size(); i++) {
		c.track_names.push_back(p_names[i].utf8().get_data());
	}
	emit_changed();
}

PackedStringArray MidiClip::get_track_names() const {
	PackedStringArray out;
	for (const std::string &n : clip->track_names) {
		out.push_back(String::utf8(n.c_str()));
	}
	return out;
}

TypedArray<Dictionary> MidiClip::get_events() const {
	TypedArray<Dictionary> out;
	for (const ClipEvent &e : clip->events) {
		Dictionary d;
		d["beat"] = e.beat;
		d["length"] = e.length;
		d["track"] = int(e.track);
		d["kind"] = int(e.kind);
		d["channel"] = int(e.channel);
		d["index"] = int(e.index);
		d["value"] = from_inside(e.kind, e.value, clip->midi_version);
		out.push_back(d);
	}
	return out;
}

TypedArray<Dictionary> MidiClip::get_tempo_map() const {
	TypedArray<Dictionary> out;
	for (const gaph::midi::ClipTempo &t : clip->tempos) {
		Dictionary d;
		d["beat"] = t.beat;
		d["bpm"] = t.bpm();
		out.push_back(d);
	}
	return out;
}

double MidiClip::get_bpm() const {
	return clip->tempos.empty() ? 120.0 : clip->tempos.front().bpm();
}

// --- Storage --------------------------------------------------------------------------------------

void MidiClip::set_event_data(const PackedFloat64Array &p_data) {
	gaph::midi::Clip &c = edit();
	c.events.clear();
	for (int64_t i = 0; i + FIELDS <= p_data.size(); i += FIELDS) {
		ClipEvent e;
		e.beat = p_data[i], e.length = p_data[i + 1], e.track = uint16_t(p_data[i + 2]);
		const int kind = int(p_data[i + 3]);
		if (kind < 0 || kind > int(Kind::PROGRAM)) {
			continue;
		}
		e.kind = Kind(kind), e.channel = uint8_t(std::clamp(int(p_data[i + 4]), 1, 16)), e.index = uint16_t(int(p_data[i + 5]) & 0x7F), e.value = uint32_t(p_data[i + 6]);
		c.events.push_back(e);
	}
	c.sort();
}

PackedFloat64Array MidiClip::get_event_data() const {
	PackedFloat64Array out;
	out.resize(int64_t(clip->events.size()) * FIELDS);
	int64_t i = 0;
	for (const ClipEvent &e : clip->events) {
		const double row[FIELDS] = { e.beat, e.length, double(e.track), double(int(e.kind)), double(e.channel), double(e.index), double(e.value) };
		for (double v : row) {
			out.set(i++, v);
		}
	}
	return out;
}

void MidiClip::set_tempo_data(const PackedFloat64Array &p_data) {
	gaph::midi::Clip &c = edit();
	c.tempos.clear();
	for (int64_t i = 0; i + 2 <= p_data.size(); i += 2) {
		if (p_data[i + 1] >= 1) {
			c.tempos.push_back(gaph::midi::ClipTempo{ p_data[i], uint32_t(p_data[i + 1]) });
		}
	}
	c.sort();
}

PackedFloat64Array MidiClip::get_tempo_data() const {
	PackedFloat64Array out;
	for (const gaph::midi::ClipTempo &t : clip->tempos) {
		out.push_back(t.beat);
		out.push_back(double(t.ten_ns_per_beat));
	}
	return out;
}

void MidiClip::set_meter_data(const PackedFloat64Array &p_data) {
	gaph::midi::Clip &c = edit();
	c.meters.clear();
	for (int64_t i = 0; i + 3 <= p_data.size(); i += 3) {
		c.meters.push_back(gaph::midi::ClipMeter{ p_data[i], uint8_t(p_data[i + 1]), uint8_t(p_data[i + 2]) });
	}
	c.sort();
}

PackedFloat64Array MidiClip::get_meter_data() const {
	PackedFloat64Array out;
	for (const gaph::midi::ClipMeter &m : clip->meters) {
		out.push_back(m.beat);
		out.push_back(double(m.numerator));
		out.push_back(double(m.denominator));
	}
	return out;
}

void MidiClip::_bind_methods() {
	ClassDB::bind_static_method("MidiClip", D_METHOD("load_file", "path"), &MidiClip::load_file);
	ClassDB::bind_static_method("MidiClip", D_METHOD("from_bytes", "bytes"), &MidiClip::from_bytes);

	ClassDB::bind_method(D_METHOD("set_midi_version", "version"), &MidiClip::set_midi_version);
	ClassDB::bind_method(D_METHOD("get_midi_version"), &MidiClip::get_midi_version);
	ClassDB::bind_method(D_METHOD("set_length", "beats"), &MidiClip::set_length);
	ClassDB::bind_method(D_METHOD("get_length"), &MidiClip::get_length);
	ClassDB::bind_method(D_METHOD("set_track_names", "names"), &MidiClip::set_track_names);
	ClassDB::bind_method(D_METHOD("get_track_names"), &MidiClip::get_track_names);

	ClassDB::bind_method(D_METHOD("add_note", "beat", "channel", "key", "velocity", "length", "track"), &MidiClip::add_note, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("add_event", "beat", "kind", "channel", "index", "value", "track"), &MidiClip::add_event, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("set_tempo", "beat", "bpm"), &MidiClip::set_tempo);
	ClassDB::bind_method(D_METHOD("set_meter", "beat", "numerator", "denominator"), &MidiClip::set_meter);
	ClassDB::bind_method(D_METHOD("clear"), &MidiClip::clear);
	ClassDB::bind_method(D_METHOD("get_event_count"), &MidiClip::get_event_count);
	ClassDB::bind_method(D_METHOD("get_events"), &MidiClip::get_events);
	ClassDB::bind_method(D_METHOD("get_tempo_map"), &MidiClip::get_tempo_map);
	ClassDB::bind_method(D_METHOD("get_bpm"), &MidiClip::get_bpm);

	ClassDB::bind_method(D_METHOD("set_event_data", "data"), &MidiClip::set_event_data);
	ClassDB::bind_method(D_METHOD("get_event_data"), &MidiClip::get_event_data);
	ClassDB::bind_method(D_METHOD("set_tempo_data", "data"), &MidiClip::set_tempo_data);
	ClassDB::bind_method(D_METHOD("get_tempo_data"), &MidiClip::get_tempo_data);
	ClassDB::bind_method(D_METHOD("set_meter_data", "data"), &MidiClip::set_meter_data);
	ClassDB::bind_method(D_METHOD("get_meter_data"), &MidiClip::get_meter_data);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "midi_version", PROPERTY_HINT_ENUM, "MIDI 1.0:1,MIDI 2.0:2"), "set_midi_version", "get_midi_version");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "length", PROPERTY_HINT_RANGE, "0,4096,0.25,or_greater,suffix:beats"), "set_length", "get_length");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "track_names"), "set_track_names", "get_track_names");
	// Saved, not shown: the events would be thousands of numbers in the inspector.
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT64_ARRAY, "event_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_event_data", "get_event_data");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT64_ARRAY, "tempo_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_tempo_data", "get_tempo_data");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT64_ARRAY, "meter_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_meter_data", "get_meter_data");
}
