#pragma once

// MIDI values: the message kinds the driver sends, and conversion between MIDI 1.0 and MIDI 2.0
// resolution. Plain C++, no Godot, so the core tests can check it.
//
// ONE RESOLUTION INSIDE
// A MidiDriver is created for MIDI 1.0 or 2.0, and takes values in that version's ranges. Inside,
// every value is held at MIDI 2.0 resolution: velocity 16 bits, everything else 32 bits. So a
// MIDI 1.0 value is scaled UP when it enters, and scaled down again only for a plugin that speaks
// MIDI 1.0. Scaling follows the MIDI 2.0 specification (M2-104-UM, "Min-Center-Max Scaling"):
// 0, the centre and the maximum map exactly onto 0, the centre and the maximum, which a plain bit
// shift does not do for the maximum (127 << 9 is 0xFE00, not 0xFFFF). The centre matters most for
// pitch bend: MIDI 1.0's 8192 is "no bend", and must stay exactly "no bend".

#include <cstdint>

namespace gaph::midi {

enum class Kind : uint8_t {
	NOTE_ON,
	NOTE_OFF,
	CONTROL, // index: the controller number
	PITCH_BEND, // value: 32-bit, 0x80000000 is the centre (no bend)
	POLY_PRESSURE, // index: the note
	CHANNEL_PRESSURE,
	PROGRAM, // value: the program number, 0-127 in both versions
};

// One message, as it travels inside: channel 1-16, values at MIDI 2.0 resolution.
struct Event {
	Kind kind = Kind::NOTE_ON;
	uint8_t channel = 1;
	uint16_t index = 0; // the note, or the controller number
	uint32_t value = 0; // velocity (16 bits) or the 32-bit value
	int64_t note_id = -1; // the MidiNote's id, for notes; -1 otherwise
};

// p_value, p_src_bits wide, scaled up to p_dst_bits (the specification's upscaling algorithm).
// Values up to the centre are a plain shift; above it, the low bits are filled by repeating the
// value's own lower bits, so the maximum reaches all ones.
inline uint32_t scale_up(uint32_t p_value, unsigned p_src_bits, unsigned p_dst_bits) {
	if (p_src_bits >= p_dst_bits) {
		return p_value >> (p_src_bits - p_dst_bits);
	}
	const unsigned scale_bits = p_dst_bits - p_src_bits;
	uint64_t shifted = uint64_t(p_value) << scale_bits;
	const uint32_t center = 1u << (p_src_bits - 1);
	if (p_value <= center) {
		return uint32_t(shifted);
	}
	const unsigned repeat_bits = p_src_bits - 1;
	const uint32_t repeat_mask = (1u << repeat_bits) - 1;
	uint64_t repeat = p_value & repeat_mask;
	if (scale_bits > repeat_bits) {
		repeat <<= scale_bits - repeat_bits;
	} else {
		repeat >>= repeat_bits - scale_bits;
	}
	while (repeat != 0) {
		shifted |= repeat;
		repeat >>= repeat_bits;
	}
	return uint32_t(shifted);
}

// Down again: the specification simply drops the low bits.
inline uint32_t scale_down(uint32_t p_value, unsigned p_src_bits, unsigned p_dst_bits) {
	return p_dst_bits >= p_src_bits ? p_value << (p_dst_bits - p_src_bits) : p_value >> (p_src_bits - p_dst_bits);
}

// The bits a value has in MIDI 1.0 and inside (MIDI 2.0), per kind. Program numbers are 7 bits
// in both versions (MIDI 2.0 adds banks separately).
inline unsigned midi1_bits(Kind p_kind) {
	return p_kind == Kind::PITCH_BEND ? 14 : 7;
}
inline unsigned midi2_bits(Kind p_kind) {
	switch (p_kind) {
		case Kind::NOTE_ON:
		case Kind::NOTE_OFF:
			return 16;
		case Kind::PROGRAM:
			return 7;
		default:
			return 32;
	}
}

// A value as the driver's user gives it, for a driver of p_version (1 or 2), to the inside
// resolution. Pitch bend arrives signed (-8192..8191, or the 32-bit signed range) and is held
// unsigned around the centre, as both MIDI versions send it. r_ok is false if out of range.
inline uint32_t to_inside(Kind p_kind, int64_t p_value, int p_version, bool &r_ok) {
	const unsigned bits = p_version == 1 ? midi1_bits(p_kind) : midi2_bits(p_kind);
	int64_t raw = p_value;
	if (p_kind == Kind::PITCH_BEND) {
		raw += int64_t(1) << (bits - 1); // signed to unsigned around the centre
	}
	const int64_t max = (int64_t(1) << bits) - 1;
	r_ok = raw >= 0 && raw <= max;
	if (!r_ok) {
		return 0;
	}
	return p_version == 1 ? scale_up(uint32_t(raw), bits, midi2_bits(p_kind)) : uint32_t(raw);
}

// The largest value a driver of p_version takes for p_kind (pitch bend: the largest positive).
inline int64_t max_value(Kind p_kind, int p_version) {
	const unsigned bits = p_version == 1 ? midi1_bits(p_kind) : midi2_bits(p_kind);
	return p_kind == Kind::PITCH_BEND ? (int64_t(1) << (bits - 1)) - 1 : (int64_t(1) << bits) - 1;
}

} // namespace gaph::midi
