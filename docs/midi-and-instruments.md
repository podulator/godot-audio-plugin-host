# MIDI and instruments: design

Status: proposal (26.10.2026). Nothing here is built yet.

## Goal

Play CLAP and VST3 **instruments** in Godot, driven by a **real-time MIDI driver** that scripts talk to directly, in the spirit of GDSiON's `SiONDriver`: notes, controllers and pitch bend sent ad hoc from gameplay code, with tempo, quantising and beat signals, sample-accurate in the audio.

MIDI file playback, external MIDI hardware and VST3 instruments come later, on the same foundations.

## What Godot provides (checked in the 4.7 source)

- **MIDI input only:** `OS.open_midi_inputs()` delivers `InputEventMIDI` on Linux (ALSA), Windows (WinMM) and macOS (CoreMIDI). The events arrive on the main thread as ordinary input events, without timestamps, so they are up to one frame late (about 16 ms at 60 fps).
- **No MIDI output, no MIDI file support, no MIDI on Android.**
- **An extension can be a sound source:** `AudioStreamPlayback::_mix` can be overridden from GDExtension (`scene/resources/audio/audio_stream.h`), so an instrument can be an `AudioStream` played by `AudioStreamPlayer`, `AudioStreamPlayer2D` or `AudioStreamPlayer3D`, on any bus.

## Architecture

Three addons in this repository, one per concern, as now (Mat, 26.10.2026: MIDI belongs in GAPH, as a third extension beside the two formats):

| addon | new classes | depends on |
| --- | --- | --- |
| `midi_host` (new) | `MidiDriver`, `MidiNote`, later `MidiClip`, `MidiInput`, `MidiOutput` | nothing: format-neutral |
| `clap_host` | `ClapInstrument` (an `AudioStream`) | the existing CLAP host |
| `vst3_host` | `Vst3Instrument` (an `AudioStream`) | the existing VST3 host |

Two extensions cannot share C++ classes (each library has its own copy of the core), so the driver and the instruments talk through Godot's object API: the driver calls a small set of methods on its targets. Anything implementing them is a valid target, including a GDScript synth or one of our effects that takes notes (a vocoder's carrier, for instance).

```gdscript
# What a MIDI target implements. time_us: when the event should be heard, in microseconds on the
# monotonic clock both extensions read (std::chrono::steady_clock, as src/core/block_clock.h).
# Values always at MIDI 2.0 resolution (see MIDI versions), whatever the driver's version.
#   source:  the sending driver's id (several drivers may share a target)
#   kind:    note_on, note_off, control, pitch_bend, poly_pressure, channel_pressure, program
#   channel: 1-16;  index: the note or controller number;  value: 16-bit velocity, 32-bit otherwise
#   note_id: the MidiNote's id, or -1
func midi_event(time_us: int, source: int, kind: int, channel: int, index: int, value: int, note_id: int) -> void
# From time_us on, for this source: drop every event not yet heard, and end every note still
# sounding (a note-off: tails ring out; with cut, at once).
func midi_stop(time_us: int, source: int, cut: bool) -> void
```

One entry point rather than a method per message keeps the protocol small, and new kinds (note expressions, later) fit without changing it; the driver offers the friendly methods. Each target converts to what its plugin speaks: CLAP note events, MIDI 1.0, MIDI 2.0.

**Which notes are playing** is kept in two places. Each target holds the authoritative list (only the audio side knows what is really sounding, and what it holds but has not played yet), per source. The driver keeps a mirror: every `MidiNote`, and per channel the last controller, pitch bend, pressure and program values, which pause, seek and loop need. CLAP plugins report when a voice has really ended, release tail included (`CLAP_EVENT_NOTE_END`), so `MidiNote.is_sounding()` can be exact there.

## The driver

```gdscript
var midi := MidiDriver.new(1)           # a node: clock, tempo, routing; MIDI 1.0 value ranges
add_child(midi)
midi.bpm = 120
midi.set_channel_target(1, $Piano)      # an AudioStreamPlayer playing a ClapInstrument
midi.set_channel_target(10, $Drums)     # channels 1-16, as on MIDI gear: 10 is the GM drum channel
midi.set_channel_target(11, $Drums)     # one instrument may listen on several channels
midi.play()                             # starts the transport: beat position, beat signals

var n := midi.note_on(1, 60, 100)       # channel, note, velocity: now, held until off()
n.off()                                 # this exact note (off(at) works too)
midi.note_on(1, 64, 90, 0.5)            # length in beats: the note-off is scheduled too
midi.note_on(10, 36, 127, 0, midi.next(1.0))    # on the next beat
midi.note_on(10, 38, 127, 0, midi.next(4.0))    # on the next bar (4/4)
midi.note_on(1, 64, 90, 0, midi.after(0.25))    # a sixteenth from now
midi.note_on(1, 67, 90, 0, midi.at_beat(16.5))  # at an exact transport position
midi.note_off(1, 60)                    # the MIDI way: every note with this channel and key
midi.control_change(1, 1, 64)           # mod wheel; every message takes the same optional `at`
midi.control_change(1, 74, 100, midi.next(1.0))
midi.pitch_bend(1, 2048)                # -8192..8191 (MIDI 1.0)
midi.poly_aftertouch(1, 60, 80)
midi.channel_pressure(1, 80)
midi.program_change(1, 5)
midi.all_notes_off()

midi.pause()                            # position kept; sounding notes end, tails ring out
midi.resume()                           # controllers sent again, then on from the same beat
midi.seek(32.0)
midi.loop(16.0, 32.0)                   # bars 5 to 8, in 4/4
midi.stop()                             # back to the start; midi.panic() cuts everything at once

midi.beat.connect(_on_beat)             # gameplay in time with the music
```

- **When** an event plays is one optional last argument, `at`, on every message: `MidiDriver.NOW` (the default) means now, one block ahead. Helpers on the driver make the others: `midi.next(q)` is the next multiple of `q` beats on the transport's grid (`next(1.0)` the next beat, `next(4.0)` the next 4/4 bar, `next(0.25)` the next sixteenth), `midi.after(b)` is `b` beats from now, and `midi.at_beat(b)` an exact transport position. GDScript has no named arguments, so one `at` reads better than separate `delay` and `quantize` arguments, which every call would have to spell out in order.
- `at` is a **beat position** on the transport, a plain `float`: `midi.at_beat(16.5)` is simply 16.5, and it can be stored, compared and handed to any message. Not microseconds, because pause, seek, loop and tempo changes move the music against real time: an event at beat 16.5 already turned into a time would be wrong the moment the transport paused.
- With the transport stopped there is no grid: `next()` and `at_beat()` return now. A time already past (a planner that fell behind) plays now, late, rather than being dropped.
- `length` is in beats; 0 (the default) holds the note until it is ended. The note's end is a beat position too, so it follows tempo changes, as in a DAW.
- `note_on` returns a `MidiNote` handle. MIDI 1.0 knows a note only by channel and key, so two C4s on one channel cannot be told apart; the handle can, and maps onto CLAP's `note_id`. A note with a `length` returns one too, to end it early. Per-note expressions attach to it later (see below).
- **Channels are 1-16**, as on MIDI gear. Channel 0 is an error, which catches code copied from Godot's 0-based `InputEventMIDI`. One instrument may be the target of several channels; every event keeps its channel, so multitimbral instruments work.
- `beat` is emitted on the main thread, so it can be a frame late; it is for gameplay, not for timing notes. Notes scheduled from it with `at = midi.next(...)` still land exactly.
- Several drivers may exist (one per song, say); each has its own transport. Every note a driver plays is its own, those played "now" too: its pause and stop end them. Sound effects that should keep playing while a song pauses use a driver of their own.

### The transport: stop, pause, resume, seek, loop

- `pause()`: the position freezes. Everything handed out but not yet heard is withdrawn (`midi_stop`), and sounding notes get their note-off, so tails ring out.
- `resume()`: the beat-to-time map is anchored afresh at that moment. First the driver sends each channel's last controller, pitch bend, pressure and program values again ("chasing"), so the instruments are in the state the music expects; then it carries on from the same beat. Optionally (`chase_notes`, off by default, as in most DAWs) notes that span the resume point start again.
- `seek(beat)` and `loop(start, end)` are the same steps: withdraw, end the sounding notes, chase, go on from the new position. At a loop's end the driver jumps back on its own, scheduled ahead, so the jump is exact.
- `stop()` is `pause()` plus a seek to the start; `panic()` cuts every note at once (`midi_stop` with `cut`), for emergencies.
- This is the musical pause, a DAW's. Freezing the sound itself, mid-note and mid-tail (a game's pause menu), is Godot's own pause of the player, and needs no MIDI at all; the instrument's clock must then cope with the gap (it does since 1.1.1, see Timing).

## MIDI versions

The driver is created for a MIDI version, `MidiDriver.new(1)` or `MidiDriver.new(2)`. The API is the same; the version sets the value ranges:

| | MIDI 1.0 | MIDI 2.0 |
| --- | --- | --- |
| velocity | 0-127 | 0-65535 |
| controllers, pressure | 0-127 | 0-4294967295 |
| pitch bend | -8192..8191 | -2147483648..2147483647 |
| channels | 1-16 | 1-16 (MIDI 2.0's 16 groups: an optional parameter, later) |

- Inside, every value is held at MIDI 2.0 resolution, whatever the driver's version; conversion happens only at the edges, when a value enters the driver and when it reaches a plugin.
- To a plugin speaking the other version, values are converted as the MIDI 2.0 specification defines (it keeps 0, the centre and the maximum exact, rather than shifting bits). CLAP's own note events take velocity as 0..1, which both versions map onto without loss.
- Out-of-range values are refused with an error: a MIDI 1.0 driver given velocity 200 says so.
- **Files state their version.** A Standard MIDI File (`.mid`) is always MIDI 1.0: its bytes cannot hold more. MIDI 2.0's own format, the MIDI Clip File (M2-116-U, 2023, "SMF2"), stores Universal MIDI Packets, and each message carries its type (MIDI 1.0 or MIDI 2.0 channel message), so the parser knows each message's version exactly and records it as `clip.midi_version`. Since values are held at MIDI 2.0 resolution inside, a clip of either version plays on a driver of either version.

## Timing

The existing parameter timing does the hard part already: every change carries a `host_time_us()` timestamp, and `BlockClock` maps it to a sample in the audio, one block (about 11.6 ms at 44.1 kHz) later, with the spacing between changes exact. `set_parameter_at(id, value, delay)` schedules ahead with the same precision. MIDI events take the same path from the driver to the instruments.

- **Two clocks.** The driver plans in **beats**: its schedule holds every event at a beat position. It hands events to the instruments only a short **lookahead** ahead (about 100 ms, adjustable), turning each beat position into a heard time in microseconds at that moment. Pause, seek, loop and tempo changes therefore only ever have to withdraw that short stretch (`midi_stop`), never rewrite times already spread over the instruments.
- **The beat-to-time map:** beat `b` falls at `t0 + (b - b0) * 60e6 / bpm`, where `t0` is the heard time of beat `b0`. Play, resume, seek and loop set a new `(b0, t0)`; a tempo change starts a new segment at a beat position.
- **The lookahead runs on the main thread,** every frame. A frame drop shorter than the lookahead costs nothing; a longer one makes events late, never lost. A larger lookahead is safer against drops and makes pause and seek withdraw more; 100 ms covers several dropped frames.
- **"Now"** means one block from now: the earliest an event can be heard. Such events skip the schedule and go out at once.
- **Scheduled events** (any `at` ahead of now, the end of a note with a `length`) are exact to the sample, because their time is known in advance.
- **A sequencer in GDScript** plans slightly ahead: on each frame or beat signal it schedules the events of the next beat or two, at beat positions. GDScript never runs on the audio thread and does not need to. `sequence_on(pattern)` (later) hands a whole pattern to the driver's schedule at once.
- Across the extension boundary only microsecond timestamps travel, on a clock both libraries read the same way.
- **Gaps in the audio** (an Android app in the background, a suspended laptop): `BlockClock` re-anchors when a block arrives more than 200 ms later than it predicts, and events waiting in a runtime are placed again (1.1.1). The driver re-anchors its beat map the same way, or the transport would jump ahead by the gap.

To check before building: an instrument holds at most the lookahead's worth of events, so a fixed-size, sorted structure allocated off the audio thread suffices. What happens when it is full (drop, or refuse at the call) must still be decided.

## Instruments

`ClapInstrument` is an `AudioStream`; its playback runs the plugin in `_mix`. Most of it is the existing host:

| reused as is | new |
| --- | --- |
| loading, scanning, plugin ids, the scan guard | an `AudioStream` / `AudioStreamPlayback` pair instead of an `AudioEffect` |
| parameters, `param_values`, `plugin_state` | note and MIDI events into the plugin's input events |
| plugin windows in the editor | no audio input; the main output port only (version 1) |
| sample-accurate timestamps, the lock-free queue | the instrument filter inverted: instruments only |
| Android (CLAP) | notes still sounding when playback stops: all-notes-off, then let the tail out or cut it |

- **Events into CLAP:** by the plugin's `note-ports` dialects, in this order of preference: CLAP note events (`CLAP_EVENT_NOTE_ON`/`OFF` with the `note_id`, velocity 0..1) plus MIDI for controllers; MIDI 2.0 (`CLAP_EVENT_MIDI2`), at full resolution; MIDI 1.0 (`CLAP_EVENT_MIDI`), converted down.
- **Several audio outputs** (drum machines and samplers with one output pair per drum): a question for this step, left open until then. A player delivers one signal to one bus, so version 1 plays the main output; more would go to their own buses, like the sidechain sends in reverse.
- **Silence:** CLAP plugins may return `CLAP_PROCESS_SLEEP`; the playback then skips the plugin until the next event, so a silent instrument costs nothing.
- **Mix rate:** the playback runs at the `AudioServer`'s mix rate, so Godot does not resample.
- **The editor:** an instrument is chosen in the `stream` property of a player rather than from a bus's Add Effect menu. Per-plugin entries (`Clap_<Name>` instrument classes), as for effects, would list them by name there.
- **VST3 later:** notes go through an `IEventList` on an event input bus; controllers and pitch bend have no events of their own and go through `IMidiMapping` to parameter changes, which makes VST3 the bigger half.

## Phases

1. **Driver + CLAP instruments** (the first thing that makes sound): `MidiDriver` with tempo, `at` and its helpers, length, beat signals, per-channel routing, note tracking and the whole transport (stop, pause and resume with chasing, seek, loop); `ClapInstrument` with notes and raw MIDI. Test: our own test instrument (a sine per note, deterministic) in the platform test, checking that notes start and stop on the exact sample, quantised to the grid, on Linux, Windows, macOS and Android.
2. **Patterns and files:** `sequence_on(pattern)` on the audio thread, `MidiClip` resources and an importer for Standard MIDI Files (`.mid`, MIDI 1.0) and MIDI Clip Files (MIDI 2.0), played through the same driver. A pattern entered by hand has no format to state its version: its values decide (anything beyond MIDI 1.0's ranges makes it a MIDI 2.0 pattern).
3. **VST3 instruments:** `Vst3Instrument`, events and `IMidiMapping`.
4. **External MIDI:** `MidiOutput` (to hardware and other programs) and a timestamped `MidiInput`, per platform: ALSA, CoreMIDI, Windows MIDI and Android MIDI. Only if live playing or external gear matter.

## Note expressions (later)

MIDI 1.0's pitch bend and channel pressure apply to a whole channel: bend it, and every note on it bends. Two ways around that exist, and the driver would use the best one each plugin supports:

1. **CLAP note expressions:** a value sent straight to one `note_id`. CLAP defines seven: volume (0..4, linear), pan (0..1), tuning (semitones, fractional), vibrato, expression, brightness and pressure (0..1).
2. **MPE** (MIDI Polyphonic Expression, 2018): each sounding note gets a channel of its own, so the channel messages become per note: pitch bend sideways, channel pressure for pressing, CC 74 ("brightness", in MPE "timbre") for sliding along the key. Made for controllers such as the ROLI Seaboard, LinnStrument and Osmose.
3. **Plain MIDI 1.0** otherwise: the whole channel moves, a limitation the docs state.

MIDI 2.0 has per-note controllers and per-note pitch bend of its own, so a MIDI 2.0 driver and plugin need neither workaround.

**Ranges:** the same rule as everywhere: MIDI ranges set by the driver's version, MIDI 2.0 resolution inside. Most expressions are MIDI controllers under another name:

| on a `MidiNote` | MIDI 1.0 (with MPE, per note) | MIDI 2.0 (per note natively) |
| --- | --- | --- |
| `pressure()` | polyphonic aftertouch, 0-127 | per-note pressure, 32 bit |
| `brightness()` | CC 74, 0-127 | per-note controller 74, 32 bit |
| `volume()` | CC 7 | per-note controller 7 |
| `pan()` | CC 10 | per-note controller 10 |
| `expression()` | CC 11 | per-note controller 11 |
| `vibrato()` | CC 77 (vibrato depth, General MIDI 2) | per-note controller 77 |

**The exception: `tune()` is in semitones** (fractional) for both versions. A pitch bend value means nothing without the instrument's bend range (±2 semitones by default, ±48 under MPE), so the driver converts semitones using the range it knows or sets (RPN 0). MIDI 2.0 itself gives per-note pitch in semitones with fractions ("Pitch 7.25"), and CLAP's tuning expression is in semitones too. The raw value stays available as `midi.pitch_bend(channel, value)`, in the driver's range.

```gdscript
var n := midi.note_on(1, 48, 100)
n.tune(2.0, midi.after(0.5))   # up 2 semitones, reached half a beat from now
n.brightness(100)              # 0-127 on a MIDI 1.0 driver, 32 bit on a MIDI 2.0 one
n.pressure(80)
```

Phase 1 only has to get the handle right (`note_on` returns a `MidiNote` with an id), so this attaches later without changing the API.

## Open questions

- None at the moment.

## Decided (Mat, 26.10.2026)

- MIDI is part of GAPH: a third extension, `midi_host`, in this repository, beside `clap_host` and `vst3_host`.
- One `note_on` for now and for later: timing is one optional `at`, a beat position, made by `midi.next()`, `midi.after()` and `midi.at_beat()`; default `MidiDriver.NOW`. With the transport stopped the helpers mean now; a time already past plays now. `length` 0 holds the note until it is ended. `note_on` returns a `MidiNote` to end it with.
- Every playing note is tracked, so the transport can end them: stop, pause and resume, seek and loop, with controllers chased on resume.
- Values in MIDI ranges, set by the driver's MIDI version (1 or 2); held at MIDI 2.0 resolution inside. The same for note expressions, except `tune()`, in semitones.
- Channels 1-16, as on MIDI gear.
- One instrument may listen on several channels; several audio outputs per instrument are a question for the instrument host, later.
