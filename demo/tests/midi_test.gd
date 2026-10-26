extends Node
# The MIDI driver through Godot:  godot --headless --path demo res://tests/midi_test.tscn
# Timing is checked as the targets see it: event times exactly where the beat grid puts them.

const Recorder = preload("res://tests/midi_recorder.gd")
const BEAT_US := 500000 # 120 bpm

var failures := 0
var beats: Array[int] = []


func _check(ok: bool, what: String) -> void:
	print(("PASS  " if ok else "FAIL  ") + what)
	if not ok:
		failures += 1


func _ready() -> void:
	_test_values()
	await _test_grid_and_handles()
	await _test_pause_resume()
	await _test_signals()
	await _test_transport()
	await _test_chase_notes()
	print("MIDI %s" % ("OK" if failures == 0 else "FAILED (%d)" % failures))
	get_tree().quit(1 if failures else 0)


func _frames(seconds: float) -> void:
	await get_tree().create_timer(seconds).timeout

# Versions, ranges and scaling, with the transport stopped (everything goes out at once).
func _test_values() -> void:
	var v1 := MidiDriver.create(1)
	var v2 := MidiDriver.create(2)
	add_child(v1)
	add_child(v2)
	_check(v1.midi_version == 1 and v2.midi_version == 2, "create(1) and create(2) set midi_version")
	_check(v1.get_velocity_max() == 127 and v2.get_velocity_max() == 65535, "velocity range: 127 (MIDI 1.0), 65535 (MIDI 2.0)")
	_check(v1.get_pitch_bend_max() == 8191 and v2.get_pitch_bend_max() == 2147483647, "pitch bend range: 8191, 2147483647")
	var rec := Recorder.new()
	v1.set_channel_target(1, rec)
	v1.note_on(1, 60, 127)
	v1.pitch_bend(1, 0)
	v1.control_change(1, 74, 64)
	_check(rec.events.size() == 3, "stopped: messages go out at once (%d)" % rec.events.size())
	if rec.events.size() == 3:
		_check(rec.events[0].value == 0xFFFF, "velocity 127 arrives as 0xFFFF (%X)" % rec.events[0].value)
		_check(rec.events[1].value == 0x80000000, "pitch bend 0 arrives as the centre, 0x80000000 (%X)" % rec.events[1].value)
		_check(rec.events[2].value == 0x80000000, "controller 64 arrives as 0x80000000 (%X)" % rec.events[2].value)
		_check(rec.events[0].source == v1.get_instance_id(), "source is the driver's instance id")
	_check(v1.note_on(0, 60, 100) == null, "channel 0 is refused (channels are 1-16)")
	_check(v1.note_on(1, 60, 200) == null, "velocity 200 is refused on a MIDI 1.0 driver")
	_check(v2.note_on(1, 60, 200) != null, "... and accepted on a MIDI 2.0 driver, where it is a valid (very quiet) velocity")
	v1.queue_free()
	v2.queue_free()

# Playing: notes on the grid, exactly a beat apart and half a beat long, and the note handle.
func _test_grid_and_handles() -> void:
	var midi := MidiDriver.create(1)
	add_child(midi)
	midi.bpm = 120
	var rec := Recorder.new()
	midi.set_channel_target(10, rec)
	midi.beat.connect(func(running: int, _song: float) -> void: beats.append(running))
	midi.play()
	var kick := midi.note_on(10, 36, 100, 0.5, midi.at_beat(1.0))
	midi.note_on(10, 38, 100, 0.5, midi.at_beat(2.0))
	var held := midi.note_on(10, 42, 100)
	_check(kick.is_active() and not kick.is_sounding(), "a planned note is active, not sounding yet")
	await _frames(0.05)
	_check(held.is_sounding(), "a note played now is sounding a frame later")
	held.off()
	await _frames(1.3)
	var on := rec.of(MidiDriver.EVENT_NOTE_ON)
	var off := rec.of(MidiDriver.EVENT_NOTE_OFF)
	_check(on.size() == 3 and off.size() == 3, "3 notes on, 3 off (%d, %d)" % [on.size(), off.size()])
	if on.size() == 3 and off.size() == 3:
		_check(on[2].time_us - on[1].time_us == BEAT_US, "beats 1 and 2 exactly a beat apart (%d us)" % (on[2].time_us - on[1].time_us))
		_check(off[2].time_us - on[2].time_us == BEAT_US / 2, "length 0.5 is exactly half a beat (%d us)" % (off[2].time_us - on[2].time_us))
		_check(off[0].note_id == held.get_id(), "off() ended that very note")
	_check(not kick.is_active(), "after its length the note has ended")
	_check(beats.slice(0, 3) == [0, 1, 2], "beat signals 0, 1, 2 (%s)" % [beats])
	midi.queue_free()


# Pause for 0.5 s between two notes planned a beat apart: after resuming, the second still comes
# a beat (of music) after the first, so 0.5 s plus a beat later in real time; the mod wheel heard
# before the pause is sent again on resume, and the target was told to release at the pause.
func _test_pause_resume() -> void:
	var midi := MidiDriver.create(1)
	add_child(midi)
	var rec := Recorder.new()
	midi.set_channel_target(1, rec)
	midi.play()
	midi.control_change(1, 1, 100)
	midi.note_on(1, 60, 100, 0.25, midi.at_beat(0.5))
	midi.note_on(1, 64, 100, 0.25, midi.at_beat(1.5))
	await _frames(0.5) # beat 1: the first note has played
	midi.pause()
	_check(rec.stops.size() == 1 and rec.stops[0].mode == MidiDriver.STOP_RELEASE, "pause tells the target to release (%d stops)" % rec.stops.size())
	var paused_at := MidiDriver.get_time_us()
	await _frames(0.5)
	midi.play()
	var resumed_at := MidiDriver.get_time_us()
	var cc := rec.of(MidiDriver.EVENT_CONTROL)
	_check(cc.size() == 2 and cc[1].value == cc[0].value, "the mod wheel is chased on resume (%d)" % cc.size())
	await _frames(0.6)
	var on := rec.of(MidiDriver.EVENT_NOTE_ON)
	_check(on.size() == 2, "both notes played (%d)" % on.size())
	if on.size() == 2:
		var paused_for := resumed_at - paused_at
		var gap: int = on[1].time_us - on[0].time_us
		_check(abs(gap - (BEAT_US + paused_for)) < 1000, "a beat of music apart, plus the pause (%d us, want %d)" % [gap, BEAT_US + paused_for])
	midi.queue_free()



# The transport's signals, each after its change: started, paused and resumed with the song
# position, position_changed on a seek and at each loop jump, tempo_changed, stopped.
func _test_signals() -> void:
	var midi := MidiDriver.create(1)
	add_child(midi)
	var got: Array[String] = []
	midi.started.connect(func() -> void: got.append("started"))
	midi.paused.connect(func(b: float) -> void: got.append("paused %.1f" % b))
	midi.resumed.connect(func(b: float) -> void: got.append("resumed %.1f" % b))
	midi.stopped.connect(func() -> void: got.append("stopped"))
	midi.position_changed.connect(func(b: float) -> void: got.append("position %.1f" % b))
	midi.tempo_changed.connect(func(bpm: float) -> void: got.append("tempo %d" % bpm))
	midi.play()
	await _frames(0.25)
	midi.pause()
	midi.play()
	midi.seek(8.0)
	midi.bpm = 240 # 250 ms a beat
	midi.set_loop(8.0, 9.0) # one beat, from where the song is now
	await _frames(0.4) # past the loop's end once
	midi.stop()
	var want: Array[String] = ["started", "paused 0.5", "resumed 0.5", "position 8.0", "tempo 240", "position 8.0", "stopped"]
	_check(got == want, "transport signals in order (%s)" % [got])
	midi.queue_free()


# Targets with midi_transport hear the transport: told at once when bound, then at play, pause
# (with the song position) and tempo changes.
func _test_transport() -> void:
	var midi := MidiDriver.create(1)
	add_child(midi)
	var rec := Recorder.new()
	midi.set_channel_target(1, rec)
	_check(rec.transports.size() == 1 and not rec.transports[0].playing, "a new target is told the transport at once")
	midi.play()
	await _frames(0.25)
	midi.pause()
	midi.bpm = 90
	var t := rec.transports
	_check(t.size() == 4, "play, pause and tempo each send a snapshot (%d)" % t.size())
	if t.size() == 4:
		_check(t[1].playing and t[1].song_beat == 0.0 and t[1].bpm == 120.0, "play: playing from beat 0 at 120 bpm")
		_check(not t[2].playing and absf(t[2].song_beat - 0.5) < 0.05, "pause: stopped at about beat 0.5 (%.3f)" % t[2].song_beat)
		_check(t[3].bpm == 90.0 and t[3].source == midi.get_instance_id(), "tempo: 90 bpm, from this driver")
	midi.queue_free()


# chase_notes: a held note the pause ended starts again on resume, the same note (its id).
func _test_chase_notes() -> void:
	var midi := MidiDriver.create(1)
	add_child(midi)
	var rec := Recorder.new()
	midi.set_channel_target(1, rec)
	midi.chase_notes = true
	midi.play()
	var n := midi.note_on(1, 60, 100)
	await _frames(0.2)
	midi.pause()
	_check(n.is_active() and not n.is_sounding(), "chase_notes: paused, the note waits to start again")
	midi.play()
	await _frames(0.1)
	var on := rec.of(MidiDriver.EVENT_NOTE_ON)
	_check(on.size() == 2 and on[1].note_id == n.get_id() and n.is_sounding(), "chase_notes: on resume the same note starts again (%d note-ons)" % on.size())
	midi.queue_free()
