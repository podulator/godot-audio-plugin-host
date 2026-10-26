extends Node
# MIDI driver -> CLAP instrument -> Godot audio, measured in the audio itself:
#   godot --headless --path demo res://tests/instrument_test.tscn
# The test instrument (tests/plugins/gaph_test_instrument.c) adds velocity x 0.25 per sounding
# note and scales by CC 7, with no oscillator: each note start and end is an exact step in the
# captured audio, on the very sample it was planned for.

const TestUtilScript = preload("res://tests/test_util.gd")
const CLAP_ID := "org.godot-audio-plugin-host.test-instrument"
const MIDI_ID := "org.godot-audio-plugin-host.test-instrument-midi"
const TRANSPORT_ID := "org.godot-audio-plugin-host.test-transport"
const DRUMS_ID := "org.godot-audio-plugin-host.test-drums"

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
		await _test_two_players(path)
		await _test_outputs(path, DRUMS_ID, "CLAP")
		await _test_sleep(path)
		await _test_transport(path)
	var vst3_notes := TestUtilScript.own_plugin("gaph_test_vst3_instrument.vst3")
	var vst3_transport := TestUtilScript.own_plugin("gaph_test_vst3_transport.vst3")
	if not DirAccess.dir_exists_absolute(vst3_notes) or not DirAccess.dir_exists_absolute(vst3_transport):
		print("SKIP  VST3 test instruments not built in " + vst3_notes.get_base_dir())
	else:
		await _test_vst3_notes(vst3_notes)
		await _test_vst3_transport(vst3_transport)
		await _test_saving(vst3_notes, "VST3")
		var vst3_drums := TestUtilScript.own_plugin("gaph_test_vst3_drums.vst3")
		if DirAccess.dir_exists_absolute(vst3_drums):
			await _test_outputs(vst3_drums, "", "VST3")
	await _test_saving(path, "CLAP")
	_test_scan(path.get_base_dir())
	print("INSTRUMENT %s" % ("OK" if failures == 0 else "FAILED (%d)" % failures))
	get_tree().quit(1 if failures else 0)


# An instrument on a bus of its own, with a capture after it; the player is started, and the
# runtime's clock given time to settle (see platform_test.gd: a stream under ~1 s old can still
# move its anchor).
func _rig(path: String, id: String, bus_name: String) -> Dictionary:
	# A .vst3 path makes a Vst3Instrument (its bundle's first class: id stays empty), else a CLAP one.
	var vst3 := path.get_extension() == "vst3"
	var inst: AudioStream = Vst3Instrument.new() if vst3 else ClapInstrument.new()
	inst.plugin_path = path
	if not vst3:
		inst.plugin_id = id
	_check(inst.is_plugin_loaded(), "%s loads (%s)" % [path.get_file() if vst3 else id.get_slice(".", 2), inst.get_load_error()])
	return await _rig_with(inst, bus_name)


# The same, for an instrument made already (a loaded resource, say).
func _rig_with(inst: AudioStream, bus_name: String) -> Dictionary:
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


# The plugin follows the driver's transport (the transport test plugin plays back what it is told:
# left = song position x 0.001, right = tempo x 0.001 while playing). A loop over beats 1 to 2 at
# 120 bpm: the position rises evenly, jumps back to 1 exactly a beat (22050 samples) apart, and
# after a pause stands still with the tempo gone.
func _test_transport(path: String) -> void:
	var r := await _rig(path, TRANSPORT_ID, "Inst_Transport")
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, r.inst)
	midi.bpm = 120
	midi.set_loop(1.0, 2.0)
	midi.play()
	var frames := await _collect(r.cap, 2.2)
	var drops: Array[int] = []
	for i in range(1, frames.size()):
		if frames[i].x < frames[i - 1].x - 0.0005: # back by more than half a beat
			drops.append(i)
	var beat := int(round(rate * 0.5))
	_check(drops.size() >= 2, "the position folds back at the loop's end (%d jumps)" % drops.size())
	if drops.size() >= 2:
		_check(drops[1] - drops[0] == beat, "loop jumps exactly a beat apart: %d samples, want %d" % [drops[1] - drops[0], beat])
		_check(absf(frames[drops[0]].x * 1000.0 - 1.0) < 1e-4, "each jump lands on beat 1 (%.5f)" % (frames[drops[0]].x * 1000.0))
		var step := (frames[drops[0] + 100].x - frames[drops[0]].x) * 1000.0 / 100.0
		_check(absf(step - 2.0 / rate) < 1e-7, "the position moves 2 beats a second, sample by sample")
	var playing := frames[frames.size() - 1]
	_check(absf(playing.y - 0.12) < 1e-6, "the plugin is told 120 bpm, playing (%.4f)" % playing.y)
	midi.pause()
	await get_tree().create_timer(0.3).timeout
	var still := await _collect(r.cap, 0.3)
	_check(still.size() > 0 and still[0].y == 0.0 and still[0].x == still[still.size() - 1].x, "paused: not playing, and the position stands still")
	await _unrig(r)


# VST3: notes as VST3 events on the exact sample (half a beat long, a beat apart), and controller
# 7 through the plugin's IMidiMapping to its Volume parameter. VST3 gets normalised values, so
# 64 arrives as exactly the MIDI 2.0 centre, 0.5 (CLAP's MIDI 1.0 path keeps 64/127).
func _test_vst3_notes(path: String) -> void:
	var r := await _rig(path, "", "Inst_Vst3")
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, r.inst)
	midi.bpm = 120
	midi.play()
	midi.note_on(1, 60, 127, 0.5, midi.at_beat(1.0))
	midi.control_change(1, 7, 64, midi.at_beat(1.5))
	midi.note_on(1, 64, 127, 0.5, midi.at_beat(2.0))
	var steps := _steps(await _collect(r.cap, 1.9))
	var beat := int(round(rate * 0.5))
	_check(steps.size() == 4, "VST3: two notes, four steps (%d: %s)" % [steps.size(), str(steps.slice(0, 6))])
	if steps.size() == 4:
		_check(is_equal_approx(steps[0][1], 0.25), "VST3: velocity 127 sounds at 0.25 (%s)" % steps[0][1])
		_check(steps[1][0] - steps[0][0] == beat / 2, "VST3: a note lasts exactly half a beat: %d samples, want %d" % [steps[1][0] - steps[0][0], beat / 2])
		_check(steps[2][0] - steps[0][0] == beat, "VST3: the notes start exactly a beat apart: %d samples, want %d" % [steps[2][0] - steps[0][0], beat])
		_check(absf(steps[2][1] - 0.125) < 1e-6, "VST3: CC 7 at 64 reaches Volume through IMidiMapping: 0.25 x 0.5 (%f)" % steps[2][1])
	await _unrig(r)


# VST3's transport is the ProcessContext, once per process() call; the host starts a new call
# wherever a snapshot lands inside a block, so a loop jump reaches the plugin on its sample, as for
# CLAP: jumps exactly a beat apart, landing exactly on the loop's start, and the tempo exact.
func _test_vst3_transport(path: String) -> void:
	var r := await _rig(path, "", "Inst_Vst3_Transport")
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, r.inst)
	midi.bpm = 120
	midi.set_loop(1.0, 2.0)
	midi.play()
	var frames := await _collect(r.cap, 2.2)
	var drops: Array[int] = []
	for i in range(1, frames.size()):
		if frames[i].x < frames[i - 1].x - 0.0005:
			drops.append(i)
	var beat := int(round(rate * 0.5))
	_check(drops.size() >= 2, "VST3: the position folds back at the loop's end (%d jumps)" % drops.size())
	if drops.size() >= 2:
		_check(drops[1] - drops[0] == beat, "VST3: loop jumps exactly a beat apart: %d samples, want %d" % [drops[1] - drops[0], beat])
		_check(absf(frames[drops[0]].x * 1000.0 - 1.0) < 1e-4, "VST3: each jump lands on beat 1 (%.5f)" % (frames[drops[0]].x * 1000.0))
	_check(absf(frames[frames.size() - 1].y - 0.12) < 1e-6, "VST3: the plugin is told 120 bpm, playing (%.4f)" % frames[frames.size() - 1].y)
	await _unrig(r)



# Saving, as a scene does: an instrument with its parameter (CLAP Master, VST3 Volume: id 1) at
# 0.5, written to a .tres and read back fresh (no cache), sounds at 0.25 x 0.5 = 0.125. Then the
# plugin's state alone (param_values emptied) brings the value back too.
func _test_saving(path: String, format: String) -> void:
	var vst3 := format == "VST3"
	var inst: AudioStream = Vst3Instrument.new() if vst3 else ClapInstrument.new()
	inst.plugin_path = path
	if not vst3:
		inst.plugin_id = CLAP_ID
	_check(inst.is_plugin_loaded(), "%s: the instrument loads for saving" % format)
	inst.set_parameter(1, 0.5)
	_check(inst.get_plugin_state().size() > 0, "%s: it has a plugin state (%d bytes)" % [format, inst.get_plugin_state().size()])
	var file := "user://instrument_%s.tres" % format.to_lower()
	_check(ResourceSaver.save(inst, file) == OK, "%s: saved to %s" % [format, file])
	var loaded: AudioStream = ResourceLoader.load(file, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(loaded != null and loaded.is_plugin_loaded(), "%s: loaded back" % format)
	_check(is_equal_approx(loaded.get_parameter(1), 0.5), "%s: the parameter comes back as 0.5 (%s)" % [format, loaded.get_parameter(1)])
	var level := await _note_level(loaded, "Inst_Save_" + format)
	_check(is_equal_approx(level, 0.125), "%s: and the loaded instrument sounds at 0.25 x 0.5 (%s)" % [format, level])

	# The state alone: a fresh instrument given only the saved bytes, before its plugin loads.
	var bare: AudioStream = Vst3Instrument.new() if vst3 else ClapInstrument.new()
	bare.plugin_path = path
	if not vst3:
		bare.plugin_id = CLAP_ID
	bare.plugin_state = inst.get_plugin_state()
	_check(bare.get_param_values().is_empty(), "%s: no hand-set values on the copy" % format)
	var bare_level := await _note_level(bare, "Inst_State_" + format)
	_check(is_equal_approx(bare_level, 0.125), "%s: the plugin's state alone brings it back (%s)" % [format, bare_level])
	DirAccess.remove_absolute(ProjectSettings.globalize_path(file))


# The level one note (velocity 127) makes on this instrument.
func _note_level(inst: AudioStream, bus_name: String) -> float:
	var r := await _rig_with(inst, bus_name)
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, inst)
	midi.play()
	midi.note_on(1, 60, 127, 0.5, midi.at_beat(0.5))
	var steps := _steps(await _collect(r.cap, 0.9))
	await _unrig(r)
	return steps[0][1] if steps.size() > 0 else -1.0



# One instrument in two players on one bus: one plugin instance, heard twice. In step, the two add
# up: a note goes from 0 straight to 0.5 on one sample (a block apart, there would be a step at 0.25
# for 512 samples). The plugin renders once: its frames grow at one stream's rate, not two. And a
# player joining later, mid-music, falls in step the same way.
func _test_two_players(path: String) -> void:
	var r := await _rig(path, CLAP_ID, "Inst_Two_Players")
	var second := AudioStreamPlayer.new()
	second.stream = r.inst
	second.bus = r.bus
	add_child(second)
	second.play()
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, r.inst)
	midi.play()
	await get_tree().create_timer(0.3).timeout
	var before: int = r.inst.get_frames_processed()
	var t0 := Time.get_ticks_usec()
	midi.note_on(1, 60, 127, 0.5, midi.after(0.5))
	var steps := _steps(await _collect(r.cap, 1.0))
	var seconds := float(Time.get_ticks_usec() - t0) / 1e6
	var per_second := float(r.inst.get_frames_processed() - before) / seconds
	_check(steps.size() == 2 and is_equal_approx(steps[0][1], 0.5), "two players, one instrument: the note at 0.5 on one sample (%s)" % str(steps))
	_check(per_second < rate * 1.3, "one plugin instance renders: %.0f frames a second, about %d" % [per_second, int(rate)])

	var late := AudioStreamPlayer.new() # joins mid-music
	late.stream = r.inst
	late.bus = r.bus
	add_child(late)
	late.play()
	await get_tree().create_timer(0.3).timeout
	midi.note_on(1, 62, 127, 0.5, midi.after(0.5))
	var three := _steps(await _collect(r.cap, 1.0))
	_check(three.size() == 2 and is_equal_approx(three[0][1], 0.75), "a third player, joined later, in step too: 0.75 on one sample (%s)" % str(three))
	second.queue_free()
	late.queue_free()
	await _unrig(r)



# Outputs: a drum machine with Main, Kick (key 36) and Snare (key 38), bound to three players on
# three buses with bind_outputs(). Three notes on one beat: each bus hears only its own drum, at
# 0.25, and all three start on the same mix step (the captures, cleared together, see the step
# at the same frame), since the outputs come from one plugin instance, rendered once a step.
func _test_outputs(path: String, id: String, format: String) -> void:
	var vst3 := format == "VST3"
	var inst: AudioStream = Vst3Instrument.new() if vst3 else ClapInstrument.new()
	inst.plugin_path = path
	if not vst3:
		inst.plugin_id = id
	_check(inst.is_plugin_loaded(), "%s drums load (%s)" % [format, inst.get_load_error()])
	var names: PackedStringArray = inst.get_output_names()
	_check(names == PackedStringArray(["Main", "Kick", "Snare"]), "%s: the plugin's outputs by name (%s)" % [format, names])
	var caps := []
	var players := []
	for i in 3:
		var bus_name := "Out_%s_%d" % [format, i]
		var bus := AudioServer.bus_count
		AudioServer.add_bus(bus)
		AudioServer.set_bus_name(bus, bus_name)
		var cap := AudioEffectCapture.new()
		cap.buffer_length = 4.0
		AudioServer.add_bus_effect(bus, cap)
		caps.append(cap)
		var player := AudioStreamPlayer.new()
		player.bus = bus_name
		add_child(player)
		players.append(player)
	inst.bind_outputs(players)
	_check(players[1].stream is AudioStream and players[1].stream != inst and players[0].stream == inst, "%s: output 0 is the instrument, the others streams of their own" % format)
	var midi := MidiDriver.create(1)
	add_child(midi)
	midi.set_channel_target(10, inst)
	await get_tree().create_timer(1.2).timeout
	midi.play()
	var at := midi.at_beat(0.5)
	midi.note_on(10, 42, 127, 0.5, at) # Main
	midi.note_on(10, 36, 127, 0.5, at) # Kick
	midi.note_on(10, 38, 127, 0.5, at) # Snare
	var frames := [PackedVector2Array(), PackedVector2Array(), PackedVector2Array()]
	for c in caps:
		c.clear_buffer()
	var t0 := Time.get_ticks_usec()
	while Time.get_ticks_usec() - t0 < 0.9e6:
		await get_tree().process_frame
		for i in 3:
			frames[i].append_array(caps[i].get_buffer(caps[i].get_frames_available()))
	var starts := []
	for i in 3:
		var steps := _steps(frames[i])
		_check(steps.size() == 2 and is_equal_approx(steps[0][1], 0.25), "%s: output %d (%s) plays only its own drum, at 0.25 (%s)" % [format, i, names[i] if i < names.size() else "?", str(steps)])
		starts.append(steps[0][0] if steps.size() > 0 else -1)
	_check(starts[0] == starts[1] and starts[1] == starts[2] and starts[0] >= 0, "%s: all three outputs start on the same sample (%s)" % [format, str(starts)])
	midi.queue_free()
	for p in players:
		p.stop()
	await get_tree().create_timer(0.1).timeout
	for i in 3:
		players[i].queue_free()
		await get_tree().process_frame
		AudioServer.remove_bus(AudioServer.get_bus_index("Out_%s_%d" % [format, i]))



# A CLAP plugin that says it has nothing to do (CLAP_PROCESS_SLEEP: the test instrument with no
# note sounding) is not called until something arrives. Asleep while idle, awake while its note
# sounds, which lands on its sample (the grid test shows it to the sample: it runs asleep too),
# asleep again after; a parameter change wakes it as well.
func _test_sleep(path: String) -> void:
	var r := await _rig(path, CLAP_ID, "Inst_Sleep")
	var inst: ClapInstrument = r.inst
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, inst)
	midi.play()
	_check(inst.is_plugin_sleeping(), "idle: the plugin sleeps")
	midi.note_on(1, 60, 127, 0.5, midi.after(0.25))
	await get_tree().create_timer(0.25).timeout # into the note
	_check(not inst.is_plugin_sleeping(), "a note wakes it")
	var steps := _steps(await _collect(r.cap, 0.6))
	# Collected from inside the note: it sounds (0.25 from the first frame), then ends in silence.
	_check(steps.size() == 2 and is_equal_approx(steps[0][1], 0.25) and steps[1][1] == 0.0, "the woken plugin sounds, then ends in silence (%s)" % str(steps))
	await get_tree().create_timer(0.1).timeout
	_check(inst.is_plugin_sleeping(), "after the note: asleep again")
	inst.set_parameter(1, 0.8)
	await get_tree().process_frame
	await get_tree().process_frame
	var woke_for_parameter := not inst.is_plugin_sleeping()
	await get_tree().create_timer(0.1).timeout
	_check(woke_for_parameter or is_equal_approx(inst.get_parameter(1), 0.8), "a parameter change reaches it while it sleeps (%s)" % inst.get_parameter(1))
	_check(inst.is_plugin_sleeping(), "and it sleeps again after")
	inst.set_parameter(1, 1.0)
	await _unrig(r)



# scan_plugins() on the instrument classes, as the effects have: only instruments, each of which
# loads as one. Our own plugins' folder is passed in; installed ones may show up too.
func _test_scan(dir: String) -> void:
	for vst3 in [false, true]:
		var format := "VST3" if vst3 else "CLAP"
		var found: Array = Vst3Instrument.scan_plugins([dir]) if vst3 else ClapInstrument.scan_plugins([dir])
		var names := found.map(func(d): return d.get("name", ""))
		var want := ["GAPH Test VST3 Instrument", "GAPH Test VST3 Drums"] if vst3 else ["GAPH Test Instrument", "GAPH Test Drums"]
		for w in want:
			_check(w in names, "%s scan_plugins() lists %s" % [format, w])
		_check(not names.any(func(n): return n.contains("Gain") or n == "GAPH Test VST3" or n == "GAPH Test Plugin"), "%s scan_plugins() leaves effects out (%s)" % [format, ", ".join(names)])
		var all_load := true
		for d in found:
			if d.has("error"):
				continue
			var inst: AudioStream = Vst3Instrument.new() if vst3 else ClapInstrument.new()
			inst.plugin_path = d.path
			inst.plugin_id = d.id
			if not inst.is_plugin_loaded():
				all_load = false
				print("      %s did not load: %s" % [d.name, inst.get_load_error()])
		_check(all_load, "%s: every instrument it lists loads as an instrument (%d)" % [format, found.size()])
	var effects := Vst3PluginEffect.scan_plugins([dir]).map(func(d): return d.get("name", ""))
	_check(not effects.any(func(n): return n.contains("Instrument") or n.contains("Drums")), "Vst3PluginEffect.scan_plugins() still lists effects only (%s)" % ", ".join(effects))
