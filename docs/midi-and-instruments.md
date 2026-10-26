# MIDI and instruments: design

Status: phase 1 built (26-27.10.2026): the driver (`midi_host`), CLAP and VST3 instruments, both saved with the scene. What is built and what is open: see [Phases](#phases).

## Goal

Play CLAP and VST3 **instruments** in Godot, driven by a **real-time MIDI driver** that scripts talk to directly, in the spirit of GDSiON's `SiONDriver`: notes, controllers and pitch bend sent ad hoc from gameplay code, with tempo, quantising and beat signals, sample-accurate in the audio.

MIDI file playback and external MIDI hardware come later, on the same foundations.

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
# From time_us on, for this source: drop every event not yet heard, and, by `mode`:
#   STOP_WITHDRAW (0): that only, sounding notes go on (a tempo or loop change re-plans);
#   STOP_RELEASE (1): and a note-off for every sounding note, so tails ring out (pause, stop, seek);
#   STOP_CUT (2): and silence them at once (panic).
func midi_stop(time_us: int, source: int, mode: int) -> void
# Optional: the transport, when a target is bound, whenever it changes, and at each loop jump ahead
# of time. In between, the position is song_beat plus the time since at bpm. loop_end <= loop_start
# means no loop. An instrument passes it on to its plugin.
func midi_transport(time_us: int, source: int, playing: bool, bpm: float, song_beat: float, loop_start: float, loop_end: float) -> void
```

One entry point rather than a method per message keeps the protocol small, and new kinds (note expressions, later) fit without changing it; the driver offers the friendly methods. Each target converts to what its plugin speaks: CLAP note events, MIDI 1.0, MIDI 2.0.

**Which notes are playing** is kept in two places. Each target holds the authoritative list (only the audio side knows what is really sounding, and what it holds but has not played yet), per source. The driver keeps a mirror: every `MidiNote`, and per channel the last controller, pitch bend, pressure and program values, which pause, seek and loop need. CLAP plugins report when a voice has really ended, release tail included (`CLAP_EVENT_NOTE_END`), so `MidiNote.is_sounding()` can be exact there.

## The driver

```gdscript
var midi := MidiDriver.create(1)        # a node: clock, tempo, routing; MIDI 1.0 value ranges
add_child(midi)
midi.bpm = 120
midi.set_channel_target(1, $Piano.stream) # the ClapInstrument (or Vst3Instrument) a player plays
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
midi.play()                             # resumes: controllers sent again, then on from the same beat
midi.seek(32.0)
midi.loop(16.0, 32.0)                   # bars 5 to 8, in 4/4
midi.stop()                             # back to the start; midi.panic() cuts everything at once

midi.beat.connect(_on_beat)             # gameplay in time with the music
midi.paused.connect(_on_paused)         # also started, resumed, stopped, position_changed, tempo_changed
```

- **When** an event plays is one optional last argument, `at`, on every message: `MidiDriver.NOW` (the default) means now, one block ahead. Helpers on the driver make the others: `midi.next(q)` is the next multiple of `q` beats on the transport's grid (`next(1.0)` the next beat, `next(4.0)` the next 4/4 bar, `next(0.25)` the next sixteenth), `midi.after(b)` is `b` beats from now, and `midi.at_beat(b)` an exact transport position. GDScript has no named arguments, so one `at` reads better than separate `delay` and `quantize` arguments, which every call would have to spell out in order.
- `at` is a **beat position** on the transport, a plain `float`: `midi.at_beat(16.5)` is simply 16.5, and it can be stored, compared and handed to any message. Not microseconds, because pause, seek, loop and tempo changes move the music against real time: an event at beat 16.5 already turned into a time would be wrong the moment the transport paused.
- With the transport stopped there is no grid: `next()` and `at_beat()` return now. A time already past (a planner that fell behind) plays now, late, rather than being dropped.
- `length` is in beats; 0 (the default) holds the note until it is ended. The note's end is a beat position too, so it follows tempo changes, as in a DAW.
- `note_on` returns a `MidiNote` handle. MIDI 1.0 knows a note only by channel and key, so two C4s on one channel cannot be told apart; the handle can, and maps onto CLAP's `note_id`. A note with a `length` returns one too, to end it early. Per-note expressions attach to it later (see below).
- **Channels are 1-16**, as on MIDI gear. Channel 0 is an error, which catches code copied from Godot's 0-based `InputEventMIDI`. One instrument may be the target of several channels; every event keeps its channel, so multitimbral instruments work.
- `beat` is emitted on the main thread, so it can be a frame late; it is for gameplay, not for timing notes. Notes scheduled from it with `at = midi.next(...)` still land exactly.
- Several drivers may exist (one per song, say); each has its own transport. Every note a driver plays is its own, those played "now" too: its pause and stop end them. Sound effects that should keep playing while a song pauses use a driver of their own.
- `note_on` with velocity 0 is a note-off, as in MIDI.
- **Signals** for the transport, each emitted after its change: `started`, `paused(song_beat)`, `resumed(song_beat)`, `stopped`, `position_changed(song_beat)` (on a seek, and at each loop jump), `tempo_changed(bpm)`. Like `beat`, they come on the main thread.

### The transport: stop, pause, resume, seek, loop

- `pause()`: the position freezes. Everything handed out but not yet heard is withdrawn (`midi_stop`), and sounding notes get their note-off, so tails ring out.
- `play()` after a pause resumes: the beat-to-time map is anchored afresh at that moment. First the driver sends each channel's last controller, pitch bend, pressure and program values again ("chasing"), so the instruments are in the state the music expects; then it carries on from the same beat. Optionally (`chase_notes`, off by default, as in most DAWs) the notes the pause ended start again: the same notes (ids, velocities), each still ending at its planned beat.
- `seek(beat)` and `loop(start, end)` are the same steps: withdraw, end the sounding notes, chase, go on from the new position. At a loop's end the driver jumps back on its own, scheduled ahead, so the jump is exact.
- `stop()` is `pause()` plus a seek to the start; `panic()` cuts every note at once (`midi_stop` with `STOP_CUT`), for emergencies. A loop or tempo change withdraws what was handed out (`STOP_WITHDRAW`: sounding notes go on) and hands it out again, re-timed.
- This is the musical pause, a DAW's. Freezing the sound itself, mid-note and mid-tail (a game's pause menu), is Godot's own pause of the player, and needs no MIDI at all; the instrument's clock must then cope with the gap (it does since 1.1.1, see Timing).

## MIDI versions

The driver is created for a MIDI version, `MidiDriver.create(1)` or `MidiDriver.create(2)` (a GDExtension class takes no constructor arguments, so not `new(1)`; GDSiON's `SiONDriver.create()` does the same). The version is also the `midi_version` property, for a driver placed in a scene. The API is the same; the version sets the value ranges:

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

An instrument holds at most the lookahead's worth of events, in the runtime's fixed-size waiting list (1024 events, allocated before audio runs). Should it ever be full, an event is delivered at once rather than dropped, as for parameter changes: a note slightly early beats a note never heard.

## Instruments

`ClapInstrument` and `Vst3Instrument` are `AudioStream`s; each playback runs one plugin instance in its `_mix`, through the same `EffectRuntime` effects use, with silence as its input. What both formats share is in `godot_common/instrument_common.h`. Most of it is the existing host:

| reused as is | new |
| --- | --- |
| loading, plugin ids, parameters, `param_values`, `plugin_state` | an `AudioStream` / `AudioStreamPlayback` pair instead of an `AudioEffect` |
| sample-accurate timestamps, the lock-free queue | MIDI on the parameters' path (`ParamEvent` types MIDI, DROP, TRANSPORT), with one time-to-sample mapping per MIDI source, so spacing stays exact to the sample |
| plugin windows (`EditorWindow`) | an **Open Plugin Window** button in the instrument's inspector: an instrument in a player's `stream` property has no click of its own |
| Android (CLAP) | no audio input; the main output port only |

- **The target** is the instrument resource itself: `midi.set_channel_target(1, $Piano.stream)`.
- **One plugin instance per instrument,** however many players play it (`godot_common/shared_voice.h`): the first playback mixed in a mix step makes the plugin render that step's block, the others copy it, so all are in step to the sample in whatever order Godot mixes them. The step is identified by when the sound card last asked for audio (`AudioServer.get_time_since_last_mix()`, as Godot keeps its own step count to itself) and which step of that request it is. A playback starting between two steps of one request is a block off until the next request (about 23 ms at most), then in step.
- **midi_stop:** the audio thread only learns "drop this source's events from time T". Which notes still sound at T is worked out on the main thread, which queued them all (`core/midi_target.h`), and they get a note-off (RELEASE) or a choke (CUT) at T.
- **Events into CLAP:** by the plugin's `note-ports` dialects: CLAP note events (with the `note_id`, velocity 0..1) where taken; else MIDI 2.0 (`CLAP_EVENT_MIDI2`, full resolution); else MIDI 1.0 (`CLAP_EVENT_MIDI`), converted down. Controllers, pitch bend, pressure and program change always go as MIDI.
- **Events into VST3:** notes and poly pressure as VST3 events on the first event input bus. VST3 has no controller events: the plugin's `IMidiMapping` is read into a table when it starts (a main-thread call), and controllers, channel pressure, pitch bend and program change become changes of the parameters it maps them to.
- **The transport** (`midi_transport`) reaches CLAP plugins as `proc.transport` plus a transport event on the very sample a snapshot lands, and VST3 plugins in the `ProcessContext`, which VST3 has once per `process()` call: the runtime starts a new call wherever a snapshot lands inside a block (`PluginProcessor::wants_transport_splits`), so a loop jump reaches VST3 plugins on its sample too.
- **Saving,** as for effects: `plugin_state` and `param_values`, in that order after the plugin; parameters as inspector sliders.
- **Several outputs** (drum machines, multi-out samplers): the plugin renders all of them in the same block; `get_output(i)` is a small `AudioStream` (`ClapInstrumentOutput`, `Vst3InstrumentOutput`) that plays output i, in a player of its own, on any bus; `bind_outputs([players])` does it for a list. Output 0 is the main one, the instrument itself; the others are the plugin's other output ports (CLAP) or aux output buses (VST3), in their order, by their names (`get_output_names()`). An output stream holds its instrument, not the other way round, so no reference cycle keeps either.
- **Mix rate:** the playback runs at the `AudioServer`'s mix rate, so Godot does not resample.
- **Sleep (CLAP):** a plugin returning `CLAP_PROCESS_SLEEP` (or `CONTINUE_IF_NOT_QUIET` with a silent output) is not called again until a block brings an event for it, its input is not silent, or it asks (`request_process`); the host outputs silence meanwhile, so an idle instrument costs nothing. The same holds for effects. VST3 has no such status.
- **Not built yet:**
  - instruments listed by name (per-plugin classes, as the Add Effect menu has for effects);

## Phases

1. **Driver + CLAP instruments: built.** `MidiDriver` with tempo, `at` and its helpers, length, the transport (stop, pause and resume with chasing, seek, loop, panic), its signals, note tracking and per-channel routing; `ClapInstrument` with notes, MIDI and the transport. Tested with deterministic test instruments (each note adds a fixed level, so its start and end are exact steps in the captured audio): notes to the sample on the grid (11025 and 22050 samples at 44.1 kHz), the MIDI 1.0 path, pause, two channels, loop jumps. Run on Linux; CI and the other platforms have not run it yet. Not built: `chase_notes` (restarting notes that span a resume point).
2. **Patterns and files:** `sequence_on(pattern)` on the audio thread, `MidiClip` resources and an importer for Standard MIDI Files (`.mid`, MIDI 1.0) and MIDI Clip Files (MIDI 2.0), played through the same driver. A pattern entered by hand has no format to state its version: its values decide (anything beyond MIDI 1.0's ranges makes it a MIDI 2.0 pattern).
3. **VST3 instruments: built.** `Vst3Instrument`, VST3 note events and `IMidiMapping`, the transport per block; and saving for both instrument types. Tested as for CLAP, plus a controller through `IMidiMapping`, and a saved instrument loaded fresh sounding as saved.
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
