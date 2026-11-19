#pragma once

// Note expressions: a value for ONE note (by its id), not for the whole channel. Plain C++, no
// Godot.
//
// The seven are CLAP's (clap/events.h), which VST3's Note Expression and MIDI 2.0's per-note
// messages largely share. Scripts give plain floats: tuning in semitones, volume as a linear gain
// 0..4 (1 = unchanged), the rest 0..1. Inside they travel as Kind::NOTE_EXPRESSION events: index
// is key + 256 * expression, value the float packed into 32 bits (pack/unpack below), so a value
// set on a MIDI 1.0 driver loses nothing before the edge.
//
// AT THE EDGE, the best each target understands:
// 1. Natively: CLAP note expressions, VST3 Note Expression. Nothing lost.
// 2. MPE (a MidiOutput with mpe on): one channel per sounding note, so channel messages become
//    per-note ones (mpe_message).
// 3. Plain MIDI ("exact or nothing", Mat 19.11.2026): pressure becomes polyphonic aftertouch, a
//    per-note message MIDI 1.0 already has. The others exist only for a whole channel, so they are
//    sent only while that note is the only one sounding on its channel, where the channel message
//    is exact; otherwise dropped, with one notice (plain_message).

#include "midi_values.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gaph::midi {

enum class Expression : uint8_t { VOLUME, PAN, TUNING, VIBRATO, EXPRESSION, BRIGHTNESS, PRESSURE, COUNT };

// Tuning: semitones, 1/65536 steps, +-32767 semitones. Volume: 0..4 over the 32 bits. Others 0..1.
inline uint32_t pack_expression(Expression p_e, double p_value) {
	switch (p_e) {
		case Expression::TUNING: {
			const double v = std::clamp(p_value, -32767.0, 32767.0);
			return uint32_t(int32_t(std::llround(v * 65536.0)));
		}
		case Expression::VOLUME:
			return uint32_t(std::llround(std::clamp(p_value, 0.0, 4.0) / 4.0 * 4294967295.0));
		default:
			return uint32_t(std::llround(std::clamp(p_value, 0.0, 1.0) * 4294967295.0));
	}
}

inline double unpack_expression(Expression p_e, uint32_t p_value) {
	switch (p_e) {
		case Expression::TUNING:
			return double(int32_t(p_value)) / 65536.0;
		case Expression::VOLUME:
			return double(p_value) / 4294967295.0 * 4.0;
		default:
			return double(p_value) / 4294967295.0;
	}
}

inline uint16_t expression_index(uint16_t p_key, Expression p_e) { return uint16_t((p_key & 0xFF) | (uint16_t(p_e) << 8)); }
inline uint16_t expression_key(uint16_t p_index) { return uint16_t(p_index & 0xFF); }
inline Expression expression_of(uint16_t p_index) { return Expression(std::min<int>(p_index >> 8, int(Expression::COUNT) - 1)); }

// A plain channel message standing in for an expression: kind, index (controller), value inside.
struct Stand_in {
	Kind kind;
	uint16_t index;
	uint32_t value;
};

// The channel message for an expression, where one exists. Tuning becomes pitch bend over a
// range of p_bend_range semitones (MIDI's customary 2; clamped beyond); volume CC 7 (gain 0..1),
// pan CC 10, vibrato the mod wheel (CC 1), expression CC 11, brightness CC 74 (the MPE timbre).
// Pressure is not here: it has its per-note message (polyphonic aftertouch), or channel pressure
// under MPE.
inline bool channel_stand_in(Expression p_e, double p_value, double p_bend_range, Stand_in &r_out) {
	auto cc = [&](uint16_t p_cc, double p_v) {
		r_out = Stand_in{ Kind::CONTROL, p_cc, uint32_t(std::llround(std::clamp(p_v, 0.0, 1.0) * 4294967295.0)) };
		return true;
	};
	switch (p_e) {
		case Expression::TUNING: {
			const double bend = std::clamp(p_value / p_bend_range, -1.0, 1.0); // -1..1 of the range
			r_out = Stand_in{ Kind::PITCH_BEND, 0, uint32_t(std::llround(2147483648.0 + bend * (bend < 0 ? 2147483648.0 : 2147483647.0))) };
			return true;
		}
		case Expression::VOLUME: return cc(7, p_value);
		case Expression::PAN: return cc(10, p_value);
		case Expression::VIBRATO: return cc(1, p_value);
		case Expression::EXPRESSION: return cc(11, p_value);
		case Expression::BRIGHTNESS: return cc(74, p_value);
		default: return false;
	}
}

} // namespace gaph::midi
