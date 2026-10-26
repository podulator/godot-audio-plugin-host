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
	// Internal, never sent by a driver: an instrument's "silence this note at once", for
	// midi_stop's CUT (CLAP_EVENT_NOTE_CHOKE, or MIDI's All Sound Off where a plugin only speaks MIDI).
	CHOKE,
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

// --- On the wire, for plugins that speak MIDI ----------------------------------------------------

// A MIDI 1.0 message: status byte (kind and channel 0-15), then two data bytes, each 7 bits.
// Values come at the inside resolution and are scaled down. CHOKE becomes All Sound Off (CC 120).
inline void encode_midi1(Kind p_kind, uint8_t p_channel, uint16_t p_index, uint32_t p_value, uint8_t r_bytes[3]) {
	const uint8_t ch = uint8_t((p_channel - 1) & 0x0F);
	const uint8_t index = uint8_t(p_index & 0x7F);
	const uint8_t v7 = uint8_t(scale_down(p_value, midi2_bits(p_kind), 7) & 0x7F);
	switch (p_kind) {
		case Kind::NOTE_ON:
			r_bytes[0] = 0x90 | ch, r_bytes[1] = index, r_bytes[2] = v7 ? v7 : 1; // 0 would mean note-off
			return;
		case Kind::NOTE_OFF:
			r_bytes[0] = 0x80 | ch, r_bytes[1] = index, r_bytes[2] = v7;
			return;
		case Kind::POLY_PRESSURE:
			r_bytes[0] = 0xA0 | ch, r_bytes[1] = index, r_bytes[2] = v7;
			return;
		case Kind::CONTROL:
			r_bytes[0] = 0xB0 | ch, r_bytes[1] = index, r_bytes[2] = v7;
			return;
		case Kind::PROGRAM:
			r_bytes[0] = 0xC0 | ch, r_bytes[1] = uint8_t(p_value & 0x7F), r_bytes[2] = 0;
			return;
		case Kind::CHANNEL_PRESSURE:
			r_bytes[0] = 0xD0 | ch, r_bytes[1] = v7, r_bytes[2] = 0;
			return;
		case Kind::PITCH_BEND: {
			const uint32_t v14 = scale_down(p_value, 32, 14); // LSB first, then MSB, 7 bits each
			r_bytes[0] = 0xE0 | ch, r_bytes[1] = uint8_t(v14 & 0x7F), r_bytes[2] = uint8_t((v14 >> 7) & 0x7F);
			return;
		}
		case Kind::CHOKE:
			r_bytes[0] = 0xB0 | ch, r_bytes[1] = 120, r_bytes[2] = 0;
			return;
	}
}

// A MIDI 2.0 channel voice message as a Universal MIDI Packet (M2-104-UM): message type 4, two
// 32-bit words. Word 0: type, group (0), status, channel, then two bytes (note or controller
// number, and for notes an attribute type, 0 for none); word 1: the value at full resolution.
inline void encode_midi2(Kind p_kind, uint8_t p_channel, uint16_t p_index, uint32_t p_value, uint32_t r_words[2]) {
	const uint32_t ch = uint32_t((p_channel - 1) & 0x0F);
	const uint32_t index = uint32_t(p_index & 0x7F);
	auto head = [&](uint32_t p_status, uint32_t p_byte3) { return (0x4u << 28) | (p_status << 20) | (ch << 16) | (p_byte3 << 8); };
	switch (p_kind) {
		case Kind::NOTE_ON:
			r_words[0] = head(0x9, index), r_words[1] = p_value << 16; // velocity: the upper 16 bits
			return;
		case Kind::NOTE_OFF:
			r_words[0] = head(0x8, index), r_words[1] = p_value << 16;
			return;
		case Kind::POLY_PRESSURE:
			r_words[0] = head(0xA, index), r_words[1] = p_value;
			return;
		case Kind::CONTROL:
			r_words[0] = head(0xB, index), r_words[1] = p_value;
			return;
		case Kind::PROGRAM:
			r_words[0] = head(0xC, 0), r_words[1] = (p_value & 0x7F) << 24; // no bank: option flags 0
			return;
		case Kind::CHANNEL_PRESSURE:
			r_words[0] = head(0xD, 0), r_words[1] = p_value;
			return;
		case Kind::PITCH_BEND:
			r_words[0] = head(0xE, 0), r_words[1] = p_value;
			return;
		case Kind::CHOKE:
			r_words[0] = head(0xB, 120), r_words[1] = 0;
			return;
	}
}

} // namespace gaph::midi
