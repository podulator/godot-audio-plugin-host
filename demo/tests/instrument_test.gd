extends Node
# MIDI driver -> CLAP instrument -> Godot audio, measured in the audio itself:
#   godot --headless --path demo res://tests/instrument_test.tscn
# The test instrument (tests/plugins/gaph_test_instrument.c) adds velocity x 0.25 per sounding
# note and scales by CC 7, with no oscillator: each note start and end is an exact step in the
# captured audio, on the very sample it was planned for.

const TestUtilScript = preload("res://tests/test_util.gd")
const CLAP_ID := "org.godot-audio-plugin-host.test-instrument"
const MIDI_ID := "org.godot-audio-plugin-host.test-instrument-midi"

var failures := 0
var rate := 44100.0


func _check(ok: bool, what: String) -> void:
	print(("PASS  " if ok else "FAIL  ") + what)
	if not ok:
		failures += 1


func _ready() -> void:
	rate = AudioServer.get_mix_rate()
	var path := TestUtilScript.own_plugin("gaph_test_instrument.clap")
	if not FileAccess.file_exists(path) and not DirAccess.dir_exists_absolute(path):
		print("SKIP  test instrument not built in " + path.get_base_dir())
	else:
		await _test_grid(path)
		await _test_midi_dialect(path)
		await _test_pause(path)
		await _test_two_channels(path)
	print("INSTRUMENT %s" % ("OK" if failures == 0 else "FAILED (%d)" % failures))
	get_tree().quit(1 if failures else 0)


# An instrument on a bus of its own, with a capture after it; the player is started, and the
# runtime's clock given time to settle (see platform_test.gd: a stream under ~1 s old can still
# move its anchor).
func _rig(path: String, id: String, bus_name: String) -> Dictionary:
	var inst := ClapInstrument.new()
	inst.plugin_path = path
	inst.plugin_id = id
	_check(inst.is_plugin_loaded(), "%s loads (%s)" % [id.get_slice(".", 2), inst.get_load_error()])
	var bus := AudioServer.bus_count
	AudioServer.add_bus(bus)
	AudioServer.set_bus_name(bus, bus_name)
	var cap := AudioEffectCapture.new()
	cap.buffer_length = 4.0
	AudioServer.add_bus_effect(bus, cap)
	var player := AudioStreamPlayer.new()
	player.stream = inst
	player.bus = bus_name
	add_child(player)
	player.play()
	var midi := MidiDriver.create(1)
	add_child(midi)
	await get_tree().create_timer(1.2).timeout
	return {"inst": inst, "cap": cap, "player": player, "midi": midi, "bus": bus_name}


func _unrig(r: Dictionary) -> void:
	r.midi.queue_free()
	r.player.stop()
	await get_tree().create_timer(0.1).timeout
	r.player.queue_free()
	await get_tree().process_frame
	AudioServer.remove_bus(AudioServer.get_bus_index(r.bus))

# Every frame the capture receives during p_seconds.
func _collect(cap: AudioEffectCapture, p_seconds: float) -> PackedVector2Array:
	var frames := PackedVector2Array()
	cap.clear_buffer()
	var t0 := Time.get_ticks_usec()
	while Time.get_ticks_usec() - t0 < p_seconds * 1e6:
		await get_tree().process_frame
		frames.append_array(cap.get_buffer(cap.get_frames_available()))
	return frames


# Where the level changes: [[frame, new level], ...] (left channel; both are the same).
func _steps(frames: PackedVector2Array) -> Array:
	var out := []
	var prev := 0.0
	for i in frames.size():
		if not is_equal_approx(frames[i].x, prev):
			out.append([i, frames[i].x])
			prev = frames[i].x
	return out


# Two notes planned on beats 1 and 2 of the transport, half a beat long, at 120 bpm: two steps of
# 0.25 (velocity 127), each exactly half a beat long and exactly a beat apart, in samples.
func _test_grid(path: String) -> void:
	var r := await _rig(path, CLAP_ID, "Inst_Grid")
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, r.inst)
	midi.bpm = 120
	midi.play()
	midi.note_on(1, 60, 127, 0.5, midi.at_beat(1.0))
	midi.note_on(1, 64, 127, 0.5, midi.at_beat(2.0))
	var frames := await _collect(r.cap, 1.9)
	var steps := _steps(frames)
	var beat := int(round(rate * 0.5))
	_check(steps.size() == 4, "two notes: four level steps (%d: %s)" % [steps.size(), str(steps.slice(0, 6))])
	if steps.size() == 4:
		_check(is_equal_approx(steps[0][1], 0.25) and is_equal_approx(steps[2][1], 0.25), "velocity 127 sounds at exactly 0.25 (%s)" % steps[0][1])
		_check(steps[1][1] == 0.0 and steps[3][1] == 0.0, "each note ends in silence")
		_check(steps[1][0] - steps[0][0] == beat / 2, "a note lasts exactly half a beat: %d samples, want %d" % [steps[1][0] - steps[0][0], beat / 2])
		_check(steps[2][0] - steps[0][0] == beat, "the notes start exactly a beat apart: %d samples, want %d" % [steps[2][0] - steps[0][0], beat])
	await _unrig(r)

# The MIDI 1.0 path (the plugin that takes only MIDI): velocity 100 arrives as 100/127, and
# channel volume (CC 7) halves the level on the next note.
func _test_midi_dialect(path: String) -> void:
	var r := await _rig(path, MIDI_ID, "Inst_Midi")
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, r.inst)
	midi.play()
	midi.note_on(1, 60, 100, 0.25, midi.at_beat(1.0))
	midi.control_change(1, 7, 127, midi.at_beat(1.5))
	midi.note_on(1, 62, 100, 0.25, midi.at_beat(2.0))
	midi.control_change(1, 7, 64, midi.at_beat(2.75))
	midi.note_on(1, 64, 100, 0.25, midi.at_beat(3.0))
	var frames := await _collect(r.cap, 2.0)
	var steps := _steps(frames)
	var full := 100.0 / 127.0 * 0.25
	_check(steps.size() == 6, "MIDI 1.0: three notes, six steps (%d)" % steps.size())
	if steps.size() == 6:
		_check(absf(steps[0][1] - full) < 1e-6, "velocity 100 sounds at 100/127 x 0.25 (%f, want %f)" % [steps[0][1], full])
		_check(absf(steps[4][1] - full * 64.0 / 127.0) < 1e-6, "CC 7 at 64 scales it by 64/127 (%f)" % steps[4][1])
		var beat := int(round(rate * 0.5))
		_check(steps[2][0] - steps[0][0] == beat, "MIDI notes keep the grid too: %d samples, want %d" % [steps[2][0] - steps[0][0], beat])
	await _unrig(r)


# Pause with a held note: it goes silent at the pause (a note-off from the instrument's own
# bookkeeping), and a note planned after the pause point plays once the transport resumes.
func _test_pause(path: String) -> void:
	var r := await _rig(path, CLAP_ID, "Inst_Pause")
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, r.inst)
	midi.play()
	var held := midi.note_on(1, 48, 127)
	midi.note_on(1, 72, 127, 0.25, midi.at_beat(2.0))
	r.cap.clear_buffer()
	await get_tree().create_timer(0.5).timeout
	midi.pause()
	await get_tree().create_timer(0.3).timeout
	var before: PackedVector2Array = r.cap.get_buffer(r.cap.get_frames_available())
	var steps := _steps(before)
	_check(steps.size() == 2 and steps[1][1] == 0.0, "the held note sounds, then goes silent at the pause (%s)" % str(steps))
	_check(not held.is_active(), "the held note has ended")
	midi.play()
	var after := await _collect(r.cap, 1.0)
	var later := _steps(after)
	_check(later.size() == 2 and is_equal_approx(later[0][1], 0.25), "the planned note plays after the resume (%s)" % str(later))
	await _unrig(r)


# One instrument on two channels: two notes at once add up.
func _test_two_channels(path: String) -> void:
	var r := await _rig(path, CLAP_ID, "Inst_Two")
	var midi: MidiDriver = r.midi
	midi.set_channel_target(10, r.inst)
	midi.set_channel_target(11, r.inst)
	midi.play()
	var at := midi.at_beat(1.0)
	midi.note_on(10, 36, 127, 0.5, at)
	midi.note_on(11, 38, 127, 0.5, at)
	var steps := _steps(await _collect(r.cap, 1.2))
	_check(steps.size() == 2 and is_equal_approx(steps[0][1], 0.5), "channels 10 and 11, same instrument, same sample: 0.5 (%s)" % str(steps))
	await _unrig(r)
