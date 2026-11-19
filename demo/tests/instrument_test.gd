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
const BEAT_US := 500000 # 120 bpm

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
		await _test_clip_audio(path)
		await _test_note_expressions(path)
		await _test_effect_tempo()
		await _test_ramps_and_modulation(path)
		await _test_presets(path)
		await _test_notes_out(path)
		await _test_midi_learn(path)
		await _test_dsp_monitor(path)
	var vst3_notes := TestUtilScript.own_plugin("gaph_test_vst3_instrument.vst3")
	var vst3_transport := TestUtilScript.own_plugin("gaph_test_vst3_transport.vst3")
	if not DirAccess.dir_exists_absolute(vst3_notes) or not DirAccess.dir_exists_absolute(vst3_transport):
		print("SKIP  VST3 test instruments not built in " + vst3_notes.get_base_dir())
	else:
		await _test_vst3_notes(vst3_notes)
		await _test_vst3_transport(vst3_transport)
		await _test_saving(vst3_notes, "VST3")
		await _test_vst3_presets(vst3_notes)
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



# A MIDI file through a real instrument, measured in the sound: the imported clip's notes on beats
# 0, 1, 2 and 3, with its tempo map (120 bpm, then 60 from beat 2), each note adding 0.25 for half
# a beat. In the audio: starts exactly a beat apart at 120 (rate / 2 samples), then at 60 (rate).
func _test_clip_audio(path: String) -> void:
	var r := await _rig(path, CLAP_ID, "Inst_Clip")
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, r.inst)
	midi.place_clip(load("res://tests/data/four_notes.mid"), 0.0)
	await get_tree().create_timer(0.3).timeout
	r.cap.clear_buffer()
	midi.play()
	var steps := _steps(await _collect(r.cap, 3.6))
	# The file's velocity is 100: each note adds 0.25 x 100/127. A start is a step up from silence.
	var starts := []
	for i in steps.size():
		if steps[i][1] > 0.1 and (i == 0 or steps[i - 1][1] == 0.0):
			starts.append(steps[i][0])
	var gaps := []
	for i in range(1, starts.size()):
		gaps.append(starts[i] - starts[i - 1])
	var beat := int(rate / 2.0)
	_check(starts.size() == 4 and gaps == [beat, beat, 2 * beat], "a MIDI file with a tempo change, to the sample: note starts %s apart, want [%d, %d, %d]" % [gaps, beat, beat, 2 * beat])
	await _unrig(r)


# Note expressions, measured in the sound. A plugin taking CLAP note events gets them natively:
# a chord of two notes (0.25 each), then the first note's volume doubled half a beat in, which
# lifts only that note, on its exact sample (0.5 -> 0.75, rate / 4 samples after the chord).
# A plugin taking only MIDI gets the "exact or nothing" stand-in: a lone note's volume 0.5 becomes
# CC 7 (its level halves); on a chord the expression is dropped (the level stays).
func _test_note_expressions(path: String) -> void:
	var r := await _rig(path, CLAP_ID, "Inst_Expr")
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, r.inst)
	midi.play()
	await get_tree().create_timer(0.1).timeout
	var at := midi.after(0.5)
	var a := midi.note_on(1, 60, 127, 1.0, at)
	midi.note_on(1, 64, 127, 1.0, at)
	a.volume(2.0, at + 0.5)
	var steps := _steps(await _collect(r.cap, 1.2))
	var quarter := int(rate / 4.0) # half a beat at 120 bpm
	_check(steps.size() >= 2 and is_equal_approx(steps[0][1], 0.5) and is_equal_approx(steps[1][1], 0.75) and steps[1][0] - steps[0][0] == quarter,
			"CLAP note expression: one note of a chord twice as loud, on its sample (%s, want +%d)" % [str(steps.slice(0, 3)), quarter])
	await _unrig(r)

	var m := await _rig(path, MIDI_ID, "Inst_Expr_Midi")
	midi = m.midi
	midi.set_channel_target(1, m.inst)
	midi.play()
	await get_tree().create_timer(0.1).timeout
	at = midi.after(0.5)
	var lone := midi.note_on(1, 60, 127, 0.5, at)
	lone.volume(0.5, at + 0.25)
	var chord := midi.note_on(1, 62, 127, 0.5, at + 1.0)
	midi.note_on(1, 65, 127, 0.5, at + 1.0)
	chord.volume(0.5, at + 1.25) # dropped: not alone on its channel
	var s2 := _steps(await _collect(m.cap, 1.8))
	var half := 64.0 / 127.0 * 0.25 # CC 7 = 64: 0.5 of the full range, scaled to 7 bits
	_check(s2.size() >= 3 and is_equal_approx(s2[0][1], 0.25) and absf(s2[1][1] - half) < 1e-4,
			"MIDI-only plugin: a lone note's volume as CC 7 (%s)" % str(s2.slice(0, 3)))
	var chord_level := 2 * 0.25 * 64.0 / 127.0 # CC 7 stays at 64 from the lone note
	_check(s2.size() >= 4 and s2.slice(2).all(func(st): return st[1] == 0.0 or absf(st[1] - chord_level) < 1e-4),
			"MIDI-only plugin: on a chord the expression is dropped, the level stays (%s)" % str(s2.slice(2)))
	await _unrig(m)


# Tempo for effects, heard by the test tempo effect (left: song beat x 0.001, right: bpm x 0.001).
# The float: bpm 140, and the position moving at that speed; sync_beat(8) puts it at beat 8. A
# driver: add_transport_target, a one-beat loop at 120 bpm, folding back exactly a beat apart.
func _test_effect_tempo() -> void:
	var path := TestUtilScript.own_plugin("gaph_test.clap")
	var fx := ClapPluginEffect.new()
	fx.plugin_path = path
	fx.plugin_id = "org.godot-audio-plugin-host.test-tempo"
	_check(fx.is_plugin_loaded(), "the test tempo effect loads (%s)" % fx.get_load_error())
	var bus := AudioServer.bus_count
	AudioServer.add_bus(bus)
	AudioServer.set_bus_name(bus, "Fx_Tempo")
	AudioServer.add_bus_effect(bus, fx)
	var cap := AudioEffectCapture.new()
	cap.buffer_length = 4.0
	AudioServer.add_bus_effect(bus, cap)
	var player := AudioStreamPlayer.new() # something on the bus, so Godot runs its effects
	var gen := AudioStreamGenerator.new()
	player.stream = gen
	player.bus = "Fx_Tempo"
	add_child(player)
	player.play()
	await get_tree().create_timer(0.3).timeout
	fx.bpm = 140.0
	await get_tree().create_timer(0.2).timeout
	var a := await _collect(cap, 0.6)
	var n := a.size()
	var slope := (a[n - 1].x - a[n - 1 - 22050].x) / 22050.0 * rate * 1000.0 * 60.0 # bpm, from the position
	_check(n > 22050 and is_equal_approx(a[n - 1].y, 0.140) and absf(slope - 140.0) < 0.01, "bpm = 140: the plugin hears 140 (%.4f), its position moving at %.3f bpm" % [a[n - 1].y * 1000.0, slope])
	fx.sync_beat(8.0)
	var b := await _collect(cap, 0.3)
	_check(b.size() > 0 and b[b.size() - 1].x > 0.008 and b[b.size() - 1].x < 0.0095, "sync_beat(8): the position goes on from beat 8 (%.5f)" % (b[b.size() - 1].x * 1000.0 if b.size() > 0 else -1.0))
	var midi := MidiDriver.create(1)
	add_child(midi)
	midi.bpm = 120
	midi.add_transport_target(fx)
	midi.set_loop(0.0, 1.0)
	midi.play()
	await get_tree().create_timer(0.2).timeout
	var c := await _collect(cap, 1.6)
	var drops := []
	for i in range(1, c.size()):
		if c[i].x < c[i - 1].x - 0.0005:
			drops.append(i)
	_check(drops.size() >= 2 and drops[1] - drops[0] == int(rate / 2.0) and is_equal_approx(c[drops[0]].y, 0.120), "a driver's one-beat loop folds the effect's position back exactly a beat apart (%s, want %d)" % [str(drops), int(rate / 2.0)])
	midi.queue_free()
	player.stop()
	await get_tree().create_timer(0.1).timeout
	player.queue_free()
	await get_tree().process_frame
	AudioServer.remove_bus(AudioServer.get_bus_index("Fx_Tempo"))


# Ramps and modulation, through the test effects on an instrument's bus (a held note: a steady
# 0.25 into the effect, so the output is 0.25 x its gain). CLAP: a ramp of Gain 1 -> 0 over half a
# second falls steadily, a point every 32 samples, and reaches exactly 0 after rate / 2 samples,
# with the stored value the target at once; modulation +0.5 (native, CLAP_EVENT_PARAM_MOD) gives
# 0.375 while the stored Gain stays 1. VST3 (emulated: value + offset sent, stored value kept): the
# same 0.375.
func _test_ramps_and_modulation(path: String) -> void:
	for vst3 in [false, true]:
		var format := "VST3" if vst3 else "CLAP"
		var fx_path := TestUtilScript.own_plugin("gaph_test_vst3.vst3" if vst3 else "gaph_test.clap")
		var fx: AudioEffect = Vst3PluginEffect.new() if vst3 else ClapPluginEffect.new()
		fx.plugin_path = fx_path
		var unity := 0.5 if vst3 else 1.0 # Gain 1 in the plugin's own units
		var r := await _rig(path, CLAP_ID, "Inst_Mod_" + format)
		AudioServer.add_bus_effect(AudioServer.get_bus_index("Inst_Mod_" + format), fx, 0)
		var midi: MidiDriver = r.midi
		midi.set_channel_target(1, r.inst)
		midi.note_on(1, 60, 127) # held
		await get_tree().create_timer(0.4).timeout
		if not vst3:
			fx.set_parameter_ramp(1, 0.0, 0.5)
			_check(fx.get_parameter(1) == 0.0, "CLAP: the stored value is the ramp's target at once")
			var a := await _collect(r.cap, 0.9)
			var first := -1
			var last := -1
			var steady := true
			for i in range(1, a.size()):
				if a[i].x < a[i - 1].x - 1e-7:
					if first < 0:
						first = i
					last = i
				elif a[i].x > a[i - 1].x + 1e-7:
					steady = false
			_check(first >= 0 and steady and a[a.size() - 1].x == 0.0 and absi(last - first - int(rate / 2.0)) <= 32,
					"CLAP: a ramp 1 -> 0 over 0.5 s falls steadily to exactly 0 in %d samples (want %d, to within 32)" % [last - first, int(rate / 2.0)])
			fx.set_parameter(1, 1.0)
			await get_tree().create_timer(0.2).timeout
		fx.set_parameter_modulation(1, 0.5 * unity)
		await get_tree().create_timer(0.2).timeout
		var m := await _collect(r.cap, 0.2)
		_check(m.size() > 0 and is_equal_approx(m[m.size() - 1].x, 0.375) and is_equal_approx(fx.get_parameter(1), unity) and is_equal_approx(fx.get_parameter_modulation(1), 0.5 * unity),
				"%s: modulation +0.5 gives 0.375, the stored Gain unchanged (%.4f, %.3f)" % [format, m[m.size() - 1].x if m.size() > 0 else -1.0, fx.get_parameter(1)])
		fx.set_parameter_modulation(1, 0.0)
		await get_tree().create_timer(0.2).timeout
		var back := await _collect(r.cap, 0.1)
		_check(back.size() > 0 and is_equal_approx(back[back.size() - 1].x, 0.25), "%s: modulation 0 ends it (%.4f)" % [format, back[back.size() - 1].x if back.size() > 0 else -1.0])
		midi.panic()
		await _unrig(r)


# Presets: the test instrument's preset-discovery factory lists Half and Quarter (and not Other,
# which names only its MIDI sibling), plus a .gtp file from the user folder; loading one changes
# the sound of the instrument already playing (Master: a note's level 0.25 x Master).
func _test_presets(path: String) -> void:
	DirAccess.make_dir_recursive_absolute("user://presets")
	var f := FileAccess.open("user://presets/Eighth.gtp", FileAccess.WRITE)
	f.store_string("0.125")
	f.close()
	OS.set_environment("GAPH_TEST_PRESET_DIR", ProjectSettings.globalize_path("user://presets"))
	var r := await _rig(path, CLAP_ID, "Inst_Presets")
	var inst: ClapInstrument = r.inst
	var presets: Array = inst.get_presets()
	var names := presets.map(func(p): return p.name)
	_check(names == ["Half", "Quarter", "Eighth"], "the plugin's presets, its own only, the user folder's too (%s)" % [names])
	var half: Dictionary = presets.filter(func(p): return p.name == "Half")[0] if names.has("Half") else {}
	_check(half.get("creator", "") == "GAPH" and half.get("tags", PackedStringArray()) == PackedStringArray(["quiet"]), "a preset's creator and tags (%s)" % [half])
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, inst)
	midi.note_on(1, 60, 127) # held
	await get_tree().create_timer(0.3).timeout
	_check(inst.load_preset(half.get("id", "")) and is_equal_approx(inst.get_parameter(1), 0.5), "load_preset(Half): Master 0.5 (%.3f)" % inst.get_parameter(1))
	await get_tree().create_timer(0.2).timeout
	var a := await _collect(r.cap, 0.1)
	_check(a.size() > 0 and is_equal_approx(a[a.size() - 1].x, 0.125), "... and the playing instrument sounds it: 0.125 (%.4f)" % (a[a.size() - 1].x if a.size() > 0 else -1.0))
	var eighth: Dictionary = presets.filter(func(p): return p.name == "Eighth")[0] if names.has("Eighth") else {}
	_check(inst.load_preset(eighth.get("id", "")), "load_preset(Eighth), a preset file")
	await get_tree().create_timer(0.2).timeout
	var b := await _collect(r.cap, 0.1)
	_check(b.size() > 0 and is_equal_approx(b[b.size() - 1].x, 0.03125), "... 0.03125 (%.5f)" % (b[b.size() - 1].x if b.size() > 0 else -1.0))
	_check(not inst.get_plugin_state().is_empty(), "the preset's sound is the saved state")
	midi.panic()
	await _unrig(r)
	var other := ClapInstrument.new()
	other.plugin_path = path
	other.plugin_id = MIDI_ID
	var other_names: Array = other.get_presets().map(func(p): return p.name)
	_check(other_names.has("Other") and not other_names.has("Half"), "the MIDI sibling lists only its own (%s)" % [other_names])
	OS.set_environment("GAPH_TEST_PRESET_DIR", "")


# VST3 presets: the test instrument's program list (Levels: Full, Half), and a .vstpreset file in a
# GAPH_VST3_PRESET_PATH folder, written here: the VST3 header with the plugin's class id, then its
# component state (Volume as a double) in a Comp chunk. Each changes the playing instrument.
func _test_vst3_presets(path: String) -> void:
	var inst := Vst3Instrument.new()
	inst.plugin_path = path
	var class_id: String = inst.get_plugin_info().get("id", "")
	var dir := ProjectSettings.globalize_path("user://vst3presets")
	DirAccess.make_dir_recursive_absolute(dir)
	var comp := PackedByteArray()
	comp.resize(8)
	comp.encode_double(0, 0.25)
	var file := PackedByteArray("VST3".to_ascii_buffer())
	var head := PackedByteArray()
	head.resize(4)
	head.encode_s32(0, 1)
	file.append_array(head)
	file.append_array(class_id.to_ascii_buffer())
	var list_at := 48 + comp.size()
	var at := PackedByteArray()
	at.resize(8)
	at.encode_s64(0, list_at)
	file.append_array(at)
	file.append_array(comp)
	var list := PackedByteArray("List".to_ascii_buffer())
	var entry := PackedByteArray()
	entry.resize(4 + 20)
	entry.encode_s32(0, 1)
	entry.encode_u32(4, 0x706D6F43) # "Comp", little-endian
	entry.encode_s64(8, 48)
	entry.encode_s64(16, comp.size())
	list.append_array(entry)
	file.append_array(list)
	var f := FileAccess.open(dir.path_join("Quarter.vstpreset"), FileAccess.WRITE)
	f.store_buffer(file)
	f.close()
	OS.set_environment("GAPH_VST3_PRESET_PATH", dir)
	var r := await _rig(path, "", "Inst_V3Presets")
	var v: Vst3Instrument = r.inst
	var presets: Array = v.get_presets()
	var names := presets.map(func(p): return p.name)
	_check(names.has("Full") and names.has("Half") and names.has("Quarter") and class_id.length() == 32, "VST3: the program list and the .vstpreset file (%s, class %s)" % [names, class_id])
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, v)
	midi.note_on(1, 60, 127) # held
	await get_tree().create_timer(0.3).timeout
	var half := presets.filter(func(p): return p.name == "Half")
	_check(half.size() == 1 and half[0].collection == "Levels" and v.load_preset(half[0].id), "VST3: load_preset(Half), a program")
	await get_tree().create_timer(0.2).timeout
	var a := await _collect(r.cap, 0.1)
	_check(a.size() > 0 and is_equal_approx(a[a.size() - 1].x, 0.125), "... the playing instrument at half: 0.125 (%.4f)" % (a[a.size() - 1].x if a.size() > 0 else -1.0))
	var quarter := presets.filter(func(p): return p.name == "Quarter")
	_check(quarter.size() == 1 and v.load_preset(quarter[0].id), "VST3: load_preset(Quarter), a .vstpreset file")
	await get_tree().create_timer(0.2).timeout
	var b := await _collect(r.cap, 0.1)
	_check(b.size() > 0 and is_equal_approx(b[b.size() - 1].x, 0.0625), "... at a quarter: 0.0625 (%.5f)" % (b[b.size() - 1].x if b.size() > 0 else -1.0))
	midi.panic()
	await _unrig(r)
	OS.set_environment("GAPH_VST3_PRESET_PATH", "")


# Notes out of a plugin: the test arp sends each note an octave up on its own sample, and CC 1 = 64
# per note-on. Two notes a beat apart: midi_output gives both, their times a beat apart (to a
# sample). Forwarded to a second driver, they play another instrument: in its sound, exactly a
# beat (rate / 2 samples) apart, an octave up arriving as notes there.
func _test_notes_out(path: String) -> void:
	var a := await _rig(path, "org.godot-audio-plugin-host.test-arp", "Inst_Arp")
	var arp: ClapInstrument = a.inst
	var got := []
	arp.midi_output.connect(func(kind: int, channel: int, index: int, value: int, t: int) -> void: got.append([kind, channel, index, value, t]))
	var b := await _rig(path, CLAP_ID, "Inst_Arp_Target")
	var far: MidiDriver = b.midi
	far.set_channel_target(1, b.inst)
	arp.set_midi_output_driver(far)
	# More than the trip: the audio buffer (the headless driver mixes in bursts of up to ~100 ms),
	# a frame, and the far block.
	arp.midi_output_latency_ms = 150.0
	var near: MidiDriver = a.midi
	near.set_channel_target(1, arp)
	near.play()
	await get_tree().create_timer(0.1).timeout
	b.cap.clear_buffer()
	var at := near.after(0.5)
	near.note_on(1, 60, 127, 0.5, at)
	near.note_on(1, 62, 127, 0.5, at + 1.0)
	var steps := _steps(await _collect(b.cap, 1.6))
	var ons := got.filter(func(e): return e[0] == MidiDriver.EVENT_NOTE_ON)
	var ccs := got.filter(func(e): return e[0] == MidiDriver.EVENT_CONTROL and e[2] == 1)
	_check(ons.size() == 2 and ons[0][2] == 72 and ons[1][2] == 74 and ccs.size() == 2, "midi_output: the arp's notes an octave up, and its mod wheel (%s)" % [got.slice(0, 4)])
	var spacing: int = ons[1][4] - ons[0][4] if ons.size() == 2 else 0
	_check(absi(spacing - BEAT_US) <= 25, "... their times a beat apart, to a sample (%d us)" % spacing)
	var starts := []
	for i in steps.size():
		if steps[i][1] > 0.1 and (i == 0 or steps[i - 1][1] == 0.0):
			starts.append(steps[i][0])
	_check(starts.size() == 2 and starts[1] - starts[0] == int(rate / 2.0), "forwarded through a driver, they play another instrument exactly a beat apart (%s)" % str(starts))
	await _unrig(a)
	await _unrig(b)


# MIDI learn, fed by a MidiInput (injected bytes: no hardware needed). learn_parameter(Master),
# then CC 21 on channel 3 moves: midi_learned(1, 3, 21), and from then on that controller moves
# Master over its range (64 -> 64/127), heard in a held note; another controller changes nothing;
# the mapping saves as midi_map and comes back.
func _test_midi_learn(path: String) -> void:
	var r := await _rig(path, CLAP_ID, "Inst_Learn")
	var inst: ClapInstrument = r.inst
	var input := MidiInput.new()
	add_child(input)
	input.add_learner(inst)
	var learned := []
	inst.midi_learned.connect(func(id: int, channel: int, controller: int) -> void: learned.append([id, channel, controller]))
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, inst)
	midi.note_on(1, 60, 127) # held: 0.25 x Master
	inst.learn_parameter(1)
	_check(inst.is_learning(), "learn_parameter: waiting for a controller")
	input.inject_bytes(PackedByteArray([0xB2, 21, 127]))
	await get_tree().create_timer(0.1).timeout
	_check(learned == [[1, 3, 21]] and not inst.is_learning(), "CC 21 on channel 3 is learned for Master (%s)" % [learned])
	input.inject_bytes(PackedByteArray([0xB2, 21, 64]))
	await get_tree().create_timer(0.3).timeout
	var a := await _collect(r.cap, 0.1)
	var want := 0.25 * 64.0 / 127.0
	_check(is_equal_approx(inst.get_parameter(1), 64.0 / 127.0) and a.size() > 0 and absf(a[a.size() - 1].x - want) < 1e-5, "the mapped controller moves Master over its range: 64 -> %.4f, heard %.5f (want %.5f)" % [inst.get_parameter(1), a[a.size() - 1].x if a.size() > 0 else -1.0, want])
	input.inject_bytes(PackedByteArray([0xB2, 22, 0])) # not mapped
	await get_tree().create_timer(0.1).timeout
	_check(is_equal_approx(inst.get_parameter(1), 64.0 / 127.0), "an unmapped controller changes nothing")
	_check(inst.midi_map == {"3:21": 1}, "the mapping is saved as midi_map (%s)" % [inst.midi_map])
	var copy := ClapInstrument.new()
	copy.plugin_path = path
	copy.midi_map = inst.midi_map
	copy.midi_controller(3, 21, 0)
	_check(copy.get_parameter(1) == 0.0, "a midi_map set on another instrument maps the same controller")
	midi.panic()
	input.queue_free()
	await _unrig(r)


# What a plugin costs: a playing instrument reports its DSP load (above 0, far below 100 % for the
# test instrument), a peak at least as high; register_monitor() puts it in Debugger > Monitors
# (Performance's custom monitors), reading the same value; freeing the instrument takes it out.
func _test_dsp_monitor(path: String) -> void:
	var r := await _rig(path, CLAP_ID, "Inst_Dsp")
	var inst: ClapInstrument = r.inst
	var midi: MidiDriver = r.midi
	midi.set_channel_target(1, inst)
	midi.note_on(1, 60, 100)
	await get_tree().create_timer(1.0).timeout
	var load: float = inst.get_dsp_load()
	var peak: float = inst.get_dsp_peak()
	_check(load > 0.0 and load < 50.0 and peak >= load, "DSP load %.4f %%, peak %.4f %%" % [load, peak])
	inst.register_monitor("Test Synth")
	var id := &"Plugins/Test Synth (DSP %)"
	_check(Performance.has_custom_monitor(id), "register_monitor(): in Debugger > Monitors as %s" % id)
	var shown: float = Performance.get_custom_monitor(id)
	_check(shown > 0.0 and absf(shown - inst.get_dsp_load()) < 1.0, "the monitor reads the plugin's load (%.4f %%)" % shown)
	midi.panic()
	await _unrig(r)
	r.clear()
	inst = null
	await get_tree().process_frame
	await get_tree().process_frame
	_check(not Performance.has_custom_monitor(id), "freeing the instrument takes its monitor out")
