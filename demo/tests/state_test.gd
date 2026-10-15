extends Node

# By path, not class_name: a new class_name is only known after a project import.
const TestUtilScript = preload("res://tests/test_util.gd")
# Plugin state, both formats:  godot --headless --path demo res://tests/state_test.tscn
# Every check here uses plugin_state ALONE (param_values emptied), so a pass proves the state
# itself carries the settings, not the parameter mirror next to it.

# Our own test plugins (built by tests/run_all.sh): Gain is id 1 in both. The value set halves the
# level in each: 0.5 for CLAP, a normalised 0.25 for VST3 (0.5 is unity there).
const RATE := 44100.0
const GAIN_ID := 1
const HALF := {"clap": 0.5, "vst3": 0.25}

var failures := 0
var phase := 0.0
var playback: AudioStreamGeneratorPlayback


func _ready() -> void:
	var clap := TestUtilScript.own_plugin("gaph_test.clap")
	var vst3 := TestUtilScript.own_plugin("gaph_test_vst3.vst3")
	if not FileAccess.file_exists(clap) or not DirAccess.dir_exists_absolute(vst3):
		print("SKIP  test plugins not built in " + clap.get_base_dir())
	else:
		await _test_live(ClapPluginEffect, clap, "clap")
		await _test_live(Vst3PluginEffect, vst3, "vst3")
		_test_wrong_format(clap, vst3)
		_test_probe_only(clap)
	print("STATE %s" % ("OK" if failures == 0 else "FAILED (%d)" % failures))
	get_tree().quit(1 if failures else 0)


func _process(_delta: float) -> void:
	if playback:
		for i in playback.get_frames_available():
			var s := sin(phase * TAU) * 0.5
			playback.push_frame(Vector2(s, s))
			phase = fmod(phase + 440.0 / RATE, 1.0)


func _check(ok: bool, what: String) -> void:
	print(("PASS  " if ok else "FAIL  ") + what)
	if not ok:
		failures += 1


func _peak(cap: AudioEffectCapture) -> float:
	await get_tree().create_timer(0.3).timeout
	cap.clear_buffer()
	await get_tree().create_timer(0.3).timeout
	var peak := 0.0
	for f in cap.get_buffer(cap.get_frames_available()):
		peak = maxf(peak, absf(f.x))
	return peak


# Saves `fx` to a .tres with param_values removed, and loads it back. The values are cleared
# AFTER the state is set, because setting a state refreshes the values from it (a preset moves
# the sliders), which would otherwise put them straight back.
func _state_only_copy(fx: AudioEffect, file: String) -> AudioEffect:
	var bytes: PackedByteArray = fx.plugin_state
	var copy: AudioEffect = fx.duplicate()
	copy.plugin_state = bytes
	copy.param_values = {}
	ResourceSaver.save(copy, file)
	var text := FileAccess.get_file_as_string(file)
	_check(text.contains("plugin_state = PackedByteArray("), "%s: plugin_state is written into the .tres" % file.get_file())
	_check(not text.contains("param_values = {\n"), "%s: and no param_values are" % file.get_file())
	return ResourceLoader.load(file, "", ResourceLoader.CACHE_MODE_IGNORE)


func _test_live(cls, path: String, fmt: String) -> void:
	var bus := AudioServer.bus_count
	AudioServer.add_bus(bus)
	AudioServer.set_bus_name(bus, "State")
	var cap := AudioEffectCapture.new()
	var gen := AudioStreamGenerator.new()
	gen.mix_rate = RATE
	var player := AudioStreamPlayer.new()
	player.stream = gen
	player.bus = "State"
	add_child(player)

	# 1. A live instance, gain turned down, state taken from it.
	var a: AudioEffect = cls.new()
	a.plugin_path = path
	AudioServer.add_bus_effect(bus, a)
	AudioServer.add_bus_effect(bus, cap)
	player.play()
	playback = player.get_stream_playback()
	a.set_parameter(GAIN_ID, HALF[fmt])
	var quiet := await _peak(cap)
	var bytes: PackedByteArray = a.plugin_state
	_check(bytes.size() > 0, "%s: state saved from the live instance (%d bytes)" % [fmt, bytes.size()])

	# 2. A fresh copy from the state alone, on the same bus in place of the original.
	var b := _state_only_copy(a, "user://state_%s.tres" % fmt)
	AudioServer.remove_bus_effect(bus, 0)
	AudioServer.add_bus_effect(bus, b, 0)
	var restored := await _peak(cap)
	_check(absf(restored - quiet) < 0.003 and absf(quiet - 0.25) < 0.005,
			"%s: state alone restores the sound: %.4f vs %.4f (gain %s)" % [fmt, restored, quiet, b.get_parameter_text(GAIN_ID)])
	_check(is_equal_approx(b.get_parameter(GAIN_ID), HALF[fmt]), "%s: inspector value refreshed from the state (%.3f)" % [fmt, b.get_parameter(GAIN_ID)])

	playback = null
	await TestUtilScript.free_generator_player(player) # never queue_free a playing generator player
	AudioServer.remove_bus(AudioServer.get_bus_index("State"))


# A CLAP state given to the VST3 host must be refused, not misread.
func _test_wrong_format(clap: String, vst3: String) -> void:
	var c := ClapPluginEffect.new()
	c.plugin_path = clap
	var v := Vst3PluginEffect.new()
	v.plugin_path = vst3
	v.set_parameter(GAIN_ID, 0.75)
	var before: float = v.get_parameter(GAIN_ID)
	v.plugin_state = c.plugin_state
	_check(is_equal_approx(v.get_parameter(GAIN_ID), before), "a CLAP state is refused by the VST3 host (magic header check)")


# No bus at all: the state comes from the probe, which must have heard set_parameter().
func _test_probe_only(clap: String) -> void:
	var fx := ClapPluginEffect.new()
	fx.plugin_path = clap
	fx.set_parameter(GAIN_ID, 1.5)
	var copy := _state_only_copy(fx, "user://state_probe.tres")
	_check(is_equal_approx(copy.get_parameter(GAIN_ID), 1.5),
			"no bus: Gain 1.5 survives through the probe's state (got %.3f, %s)" % [copy.get_parameter(GAIN_ID), copy.get_parameter_text(GAIN_ID)])
