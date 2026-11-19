#pragma once

// Reading MIDI files into a Clip (midi_clip.h). Plain C++, no Godot, so the core tests can feed it
// made-up bytes.
//
// STANDARD MIDI FILES (.mid, MIDI 1.0): read_smf. Formats 0 and 1 (format 2, independent
// patterns, is read like 1: its tracks play together). Ticks per quarter note become beats; a
// time-based (SMPTE) division becomes beats at 120 bpm, with that tempo stored. Running status,
// SysEx and unknown chunks are handled and skipped; meta events kept: tempo, time signature,
// track name, end of track. A note-on with velocity 0 is a note-off. Notes are paired first in,
// first out, per track, channel and key; one never ended ends with the clip. The length is the
// end of the longest track, rounded up to a whole bar of the first time signature, so a repeat
// starts on a bar.
//
// MIDI CLIP FILES (MIDI 2.0, M2-116-U): read_clip_file, further down.

#include "midi_clip.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace gaph::midi {

namespace file_detail {

struct Reader {
	const uint8_t *p;
	size_t size, pos = 0;
	bool failed = false;

	bool has(size_t n) const { return !failed && pos + n <= size; }
	uint8_t u8() {
		if (!has(1)) {
			failed = true;
			return 0;
		}
		return p[pos++];
	}
	uint32_t be(int n) {
		uint32_t v = 0;
		for (int i = 0; i < n; i++) {
			v = (v << 8) | u8();
		}
		return v;
	}
	// A variable-length quantity: at most 4 bytes of 7 bits, the high bit set on all but the last.
	uint32_t vlq() {
		uint32_t v = 0;
		for (int i = 0; i < 4; i++) {
			const uint8_t b = u8();
			v = (v << 7) | (b & 0x7F);
			if (!(b & 0x80)) {
				return v;
			}
		}
		failed = true;
		return 0;
	}
};

// Pairs note-ons with their note-offs, first in, first out, per track, channel and key.
struct NotePairer {
	Clip &clip;
	std::map<std::tuple<uint16_t, uint8_t, uint16_t>, std::deque<size_t>> open = {};

	void on(uint16_t p_track, uint8_t p_channel, uint16_t p_key, uint32_t p_velocity, double p_beat) {
		ClipEvent e;
		e.beat = p_beat, e.track = p_track, e.kind = Kind::NOTE_ON, e.channel = p_channel, e.index = p_key, e.value = p_velocity;
		open[{ p_track, p_channel, p_key }].push_back(clip.events.size());
		clip.events.push_back(e);
	}
	void off(uint16_t p_track, uint8_t p_channel, uint16_t p_key, double p_beat) {
		auto it = open.find({ p_track, p_channel, p_key });
		if (it == open.end() || it->second.empty()) {
			return; // a note-off for no note
		}
		ClipEvent &e = clip.events[it->second.front()];
		e.length = p_beat - e.beat;
		it->second.pop_front();
	}
	// Notes never ended end with the clip.
	void close(double p_end) {
		for (auto &entry : open) {
			for (size_t i : entry.second) {
				clip.events[i].length = std::max(0.0, p_end - clip.events[i].beat);
			}
		}
		open.clear();
	}
};

inline void push(Clip &r_clip, uint16_t p_track, Kind p_kind, uint8_t p_channel, uint16_t p_index, uint32_t p_value, double p_beat) {
	ClipEvent e;
	e.beat = p_beat, e.track = p_track, e.kind = p_kind, e.channel = p_channel, e.index = p_index, e.value = p_value;
	r_clip.events.push_back(e);
}

// The end, rounded up to a whole bar (a file ending exactly on a bar keeps its length).
inline double round_to_bar(const Clip &p_clip, double p_end) {
	const double bar = p_clip.bar_beats();
	if (!(bar > 0) || p_end <= 0) {
		return p_end;
	}
	return std::ceil(p_end / bar - 1e-9) * bar;
}

// After reading: the notes closed, the length set, everything sorted.
inline void finish(Clip &r_clip, NotePairer &p_notes, double p_end) {
	for (const ClipEvent &e : r_clip.events) {
		p_end = std::max(p_end, e.beat + e.length);
	}
	p_notes.close(p_end);
	r_clip.length = round_to_bar(r_clip, p_end);
	r_clip.sort();
}

} // namespace file_detail

// A Standard MIDI File, from its bytes. False, with r_error set, if it is not one or is damaged.
inline bool read_smf(const uint8_t *p_data, size_t p_size, Clip &r_clip, std::string &r_error) {
	using namespace file_detail;
	r_clip = Clip{};
	r_clip.midi_version = 1;
	if (p_size < 14 || std::memcmp(p_data, "MThd", 4) != 0) {
		r_error = "not a Standard MIDI File (no MThd header)";
		return false;
	}
	Reader in{ p_data, p_size };
	in.pos = 4;
	const uint32_t header_len = in.be(4);
	const uint32_t format = in.be(2);
	const uint32_t track_count = in.be(2);
	const uint32_t division = in.be(2);
	if (header_len < 6 || format > 2 || division == 0) {
		r_error = "unsupported MIDI file header (format " + std::to_string(format) + ")";
		return false;
	}
	in.pos = 8 + header_len;
	// Ticks to beats. A SMPTE division (high bit set: minus the frames per second, then ticks per
	// frame) counts time, not beats, so it is read at a fixed 120 bpm (2 beats a second).
	const bool smpte = division & 0x8000;
	double ticks_per_beat = double(division);
	if (smpte) {
		const int fps = -int(int8_t(division >> 8));
		ticks_per_beat = (fps == 29 ? 29.97 : double(fps)) * double(division & 0xFF) * 0.5;
		r_clip.tempos.push_back(ClipTempo{ 0.0, 50000000 });
	}
	if (!(ticks_per_beat > 0)) {
		r_error = "MIDI file has no valid time division";
		return false;
	}
	NotePairer notes{ r_clip };
	double end = 0;
	uint16_t track = 0;
	while (track < track_count && in.has(8)) {
		const size_t chunk = in.pos;
		in.pos += 4;
		const uint32_t len = in.be(4);
		if (std::memcmp(p_data + chunk, "MTrk", 4) != 0) {
			in.pos += len; // an unknown chunk: skipped, as the format asks
			continue;
		}
		if (!in.has(len)) {
			r_error = "MIDI file is cut short in track " + std::to_string(track + 1);
			return false;
		}
		Reader tr{ p_data + in.pos, len };
		in.pos += len;
		uint64_t tick = 0;
		uint8_t status = 0;
		std::string name;
		while (tr.has(1)) {
			tick += tr.vlq();
			const double beat = double(tick) / ticks_per_beat;
			const uint8_t b = tr.u8();
			if (b == 0xFF) { // a meta event
				const uint8_t type = tr.u8();
				const uint32_t mlen = tr.vlq();
				if (!tr.has(mlen)) {
					tr.failed = true;
					break;
				}
				const uint8_t *m = tr.p + tr.pos;
				if (type == 0x51 && mlen >= 3 && !smpte) {
					const uint32_t us = (uint32_t(m[0]) << 16) | (uint32_t(m[1]) << 8) | m[2];
					if (us > 0) {
						r_clip.tempos.push_back(ClipTempo{ beat, us * 100 });
					}
				} else if (type == 0x58 && mlen >= 2) {
					r_clip.meters.push_back(ClipMeter{ beat, m[0], uint8_t(m[1] < 8 ? 1u << m[1] : 4) });
				} else if (type == 0x03 && name.empty()) {
					name.assign(reinterpret_cast<const char *>(m), mlen);
				} else if (type == 0x2F) {
					end = std::max(end, beat);
				}
				tr.pos += mlen;
				status = 0; // meta and SysEx events cancel running status
				continue;
			}
			if (b == 0xF0 || b == 0xF7) { // SysEx: skipped
				tr.pos += tr.vlq();
				status = 0;
				continue;
			}
			uint8_t d1;
			if (b & 0x80) {
				status = b;
				d1 = tr.u8();
			} else if (status) {
				d1 = b; // running status: this is already the first data byte
			} else {
				tr.failed = true;
				break;
			}
			const uint8_t kind = status & 0xF0;
			const uint8_t ch = uint8_t((status & 0x0F) + 1);
			const uint8_t d2 = (kind != 0xC0 && kind != 0xD0) ? tr.u8() : 0;
			switch (kind) {
				case 0x90:
					if (d2) {
						notes.on(track, ch, d1, scale_up(d2, 7, 16), beat);
					} else {
						notes.off(track, ch, d1, beat);
					}
					break;
				case 0x80:
					notes.off(track, ch, d1, beat);
					break;
				case 0xA0:
					push(r_clip, track, Kind::POLY_PRESSURE, ch, d1, scale_up(d2, 7, 32), beat);
					break;
				case 0xB0:
					push(r_clip, track, Kind::CONTROL, ch, d1, scale_up(d2, 7, 32), beat);
					break;
				case 0xC0:
					push(r_clip, track, Kind::PROGRAM, ch, 0, d1 & 0x7F, beat);
					break;
				case 0xD0:
					push(r_clip, track, Kind::CHANNEL_PRESSURE, ch, 0, scale_up(d1, 7, 32), beat);
					break;
				case 0xE0:
					push(r_clip, track, Kind::PITCH_BEND, ch, 0, scale_up(uint32_t(d1 & 0x7F) | (uint32_t(d2 & 0x7F) << 7), 14, 32), beat);
					break;
				default:
					break;
			}
			end = std::max(end, beat);
		}
		if (tr.failed) {
			r_error = "MIDI file is damaged in track " + std::to_string(track + 1);
			return false;
		}
		r_clip.track_names.push_back(name);
		track++;
	}
	finish(r_clip, notes, end);
	return true;
}

// MIDI CLIP FILES (MIDI 2.0's own format, M2-116-U, "SMF2"): the bytes "SMF2CLIP", then Universal
// MIDI Packets (big-endian 32-bit words). A header (up to Start of Clip) sets the Delta Clockstamp
// Ticks Per Quarter note; after it, each message follows a Delta Clockstamp that moves time on,
// until End of Clip, which also gives the clip its length. Kept: MIDI 1.0 channel voice messages
// in UMP (type 2), MIDI 2.0 channel voice messages (type 4: notes, controllers, pressure, pitch
// bend, program with bank), and the tempo and time signature (Flex Data, type 13). Skipped: groups
// (every message plays on channels 1-16), per-note controllers and per-note pitch bend (for note
// expressions, later), RPN/NRPN and everything else. The clip's midi_version is 2 if it holds any
// MIDI 2.0 channel voice message, else 1.

namespace file_detail {

// How many 32-bit words a packet of each message type takes.
inline int ump_words(uint32_t p_word0) {
	static const int SIZES[16] = { 1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4 };
	return SIZES[p_word0 >> 28];
}

} // namespace file_detail

// A MIDI Clip File, from its bytes. False, with r_error set, if it is not one or is damaged.
inline bool read_clip_file(const uint8_t *p_data, size_t p_size, Clip &r_clip, std::string &r_error) {
	using namespace file_detail;
	r_clip = Clip{};
	r_clip.midi_version = 1;
	if (p_size < 8 || std::memcmp(p_data, "SMF2CLIP", 8) != 0) {
		r_error = "not a MIDI Clip File (no SMF2CLIP header)";
		return false;
	}
	if ((p_size - 8) % 4 != 0) {
		r_error = "MIDI Clip File is damaged (not whole 32-bit words)";
		return false;
	}
	Reader in{ p_data, p_size };
	in.pos = 8;
	NotePairer notes{ r_clip };
	r_clip.track_names.push_back("");
	double ticks_per_beat = 0;
	uint64_t tick = 0;
	bool started = false, ended = false;
	double end = 0;
	while (in.has(4) && !ended) {
		uint32_t w[4] = { in.be(4), 0, 0, 0 };
		const int n = ump_words(w[0]);
		if (!in.has(size_t(n - 1) * 4)) {
			r_error = "MIDI Clip File is cut short";
			return false;
		}
		for (int i = 1; i < n; i++) {
			w[i] = in.be(4);
		}
		const uint32_t type = w[0] >> 28;
		const double beat = ticks_per_beat > 0 ? double(tick) / ticks_per_beat : 0.0;
		if (type == 0x0) { // utility: ticks per quarter note, delta clockstamps
			const uint32_t status = (w[0] >> 20) & 0xF;
			if (status == 0x3) {
				ticks_per_beat = double(w[0] & 0xFFFF);
			} else if (status == 0x4) {
				tick += w[0] & 0xFFFFF;
			}
			continue;
		}
		if (type == 0xF) { // UMP stream: start and end of the clip
			const uint32_t status = (w[0] >> 16) & 0x3FF;
			if (status == 0x20) {
				started = true;
			} else if (status == 0x21 && started) {
				end = std::max(end, beat);
				ended = true;
			}
			continue;
		}
		if (!started) {
			continue; // the header holds only configuration
		}
		if (!(ticks_per_beat > 0)) {
			r_error = "MIDI Clip File sets no ticks per quarter note before its first event";
			return false;
		}
		const uint8_t ch = uint8_t(((w[0] >> 16) & 0xF) + 1);
		if (type == 0x2) { // MIDI 1.0 channel voice, in a packet
			const uint8_t kind = (w[0] >> 16) & 0xF0, d1 = (w[0] >> 8) & 0x7F, d2 = w[0] & 0x7F;
			switch (kind) {
				case 0x90:
					if (d2) {
						notes.on(0, ch, d1, scale_up(d2, 7, 16), beat);
					} else {
						notes.off(0, ch, d1, beat);
					}
					break;
				case 0x80: notes.off(0, ch, d1, beat); break;
				case 0xA0: push(r_clip, 0, Kind::POLY_PRESSURE, ch, d1, scale_up(d2, 7, 32), beat); break;
				case 0xB0: push(r_clip, 0, Kind::CONTROL, ch, d1, scale_up(d2, 7, 32), beat); break;
				case 0xC0: push(r_clip, 0, Kind::PROGRAM, ch, 0, d1, beat); break;
				case 0xD0: push(r_clip, 0, Kind::CHANNEL_PRESSURE, ch, 0, scale_up(d1, 7, 32), beat); break;
				case 0xE0: push(r_clip, 0, Kind::PITCH_BEND, ch, 0, scale_up(uint32_t(d1) | (uint32_t(d2) << 7), 14, 32), beat); break;
				default: break;
			}
		} else if (type == 0x4) { // MIDI 2.0 channel voice
			r_clip.midi_version = 2;
			const uint32_t op = (w[0] >> 20) & 0xF, index = (w[0] >> 8) & 0x7F;
			switch (op) {
				case 0x9: { // a MIDI 2.0 note-on with velocity 0 is still a note-on: the quietest one
					const uint32_t velocity = w[1] >> 16;
					notes.on(0, ch, uint16_t(index), velocity ? velocity : 1, beat);
					break;
				}
				case 0x8: notes.off(0, ch, uint16_t(index), beat); break;
				case 0xA: push(r_clip, 0, Kind::POLY_PRESSURE, ch, uint16_t(index), w[1], beat); break;
				case 0xB: push(r_clip, 0, Kind::CONTROL, ch, uint16_t(index), w[1], beat); break;
				case 0xC:
					if (w[0] & 1) { // bank valid: as MIDI 1.0's bank select, before the program
						push(r_clip, 0, Kind::CONTROL, ch, 0, scale_up((w[1] >> 8) & 0x7F, 7, 32), beat);
						push(r_clip, 0, Kind::CONTROL, ch, 32, scale_up(w[1] & 0x7F, 7, 32), beat);
					}
					push(r_clip, 0, Kind::PROGRAM, ch, 0, (w[1] >> 24) & 0x7F, beat);
					break;
				case 0xD: push(r_clip, 0, Kind::CHANNEL_PRESSURE, ch, 0, w[1], beat); break;
				case 0xE: push(r_clip, 0, Kind::PITCH_BEND, ch, 0, w[1], beat); break;
				default: break; // per-note controllers and pitch bend, RPN, NRPN: later
			}
		} else if (type == 0xD) { // Flex Data: tempo and time signature
			const uint32_t bank = (w[0] >> 8) & 0xFF, status = w[0] & 0xFF;
			if (bank == 0 && status == 0x00 && w[1] > 0) {
				r_clip.tempos.push_back(ClipTempo{ beat, w[1] });
			} else if (bank == 0 && status == 0x01) {
				const uint32_t den = (w[1] >> 16) & 0xFF;
				r_clip.meters.push_back(ClipMeter{ beat, uint8_t(w[1] >> 24), uint8_t(den < 8 ? 1u << den : 4) });
			}
		}
		end = std::max(end, beat);
	}
	if (!started) {
		r_error = "MIDI Clip File has no Start of Clip";
		return false;
	}
	finish(r_clip, notes, end);
	return true;
}

// Either kind of file, told apart by its first bytes.
inline bool read_midi_file(const uint8_t *p_data, size_t p_size, Clip &r_clip, std::string &r_error) {
	if (p_size >= 8 && std::memcmp(p_data, "SMF2CLIP", 8) == 0) {
		return read_clip_file(p_data, p_size, r_clip, r_error);
	}
	return read_smf(p_data, p_size, r_clip, r_error);
}

} // namespace gaph::midi
