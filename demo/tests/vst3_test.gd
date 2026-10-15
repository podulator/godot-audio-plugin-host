extends Node
# Headless test of the VST3 extension: godot --headless --path demo res://tests/vst3_test.tscn
# Mirrors clap_test.gd, so a difference between the two points at the format, not at Godot. Uses our
# own VST3 test plugin (tests/plugins/gaph_test_vst3.cpp), built by tests/run_all.sh.

const TestUtilScript = preload("res://tests/test_util.gd")
const RATE := 44100.0

var failures := 0
var skipped := 0
var phase := 0.0
var playback: AudioStreamGeneratorPlayback
var player: AudioStreamPlayer


func _ready() -> void:
	var path := TestUtilScript.own_plugin("gaph_test_vst3.vst3")
	if DirAccess.dir_exists_absolute(path):
		_test_scan(path.get_base_dir())
		await _test_own_plugin(path)
	else:
		_skip("test plugin not built at " + path)
	print("VST3 %s (%d skipped)" % ["OK" if failures == 0 else "FAILED (%d)" % failures, skipped])
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


func _skip(what: String) -> void:
	print("SKIP  " + what)
	skipped += 1


func _test_scan(extra_dir: String) -> void:
	var found: Array = Vst3PluginEffect.scan_plugins(PackedStringArray([extra_dir]))
	var names := found.map(func(d): return d.get("name", "?"))
	print("      scan found %d effects: %s" % [found.size(), ", ".join(names)])
	_check("GAPH Test VST3" in names, "scan_plugins() finds the test plugin through an extra folder")
	var own: Array = found.filter(func(d): return d.get("name") == "GAPH Test VST3")
	_check(own.size() == 1 and own[0].has("from_moduleinfo"), "scan reports how it learned about each plugin (from_moduleinfo = %s)" % (str(own[0].get("from_moduleinfo")) if own.size() else "?"))
	_check(not ("OneTrick URCHIN" in names), "scan_plugins() leaves out the URCHIN instrument (if installed)")


func _peak_over(cap: AudioEffectCapture, seconds: float) -> float:
	cap.clear_buffer()
	await get_tree().create_timer(seconds).timeout
	var peak := 0.0
	for f in cap.get_buffer(cap.get_frames_available()):
		peak = maxf(peak, maxf(absf(f.x), absf(f.y)))
	return peak


# out = main * gain + sidechain * mix; Gain (id 1) normalised, 0.5 = unity.
func _test_own_plugin(path: String) -> void:
	var fx := Vst3PluginEffect.new()
	fx.plugin_path = path
	_check(fx.is_plugin_loaded(), "the test plugin loads (%s)" % fx.get_load_error())
	var info: Dictionary = fx.get_plugin_info()
	_check(info.get("name") == "GAPH Test VST3", "plugin info name is %s" % info.get("name"))
	_check(info.get("inputs", []).size() == 2, "plugin info shows main + sidechain buses")
	_check("Fx" in info.get("features", PackedStringArray()), "features come from VST3 subcategories (%s)" % ", ".join(info.get("features", [])))

	var gain_id := fx.find_parameter("Gain")
	_check(gain_id == 1, "find_parameter('Gain') -> %d" % gain_id)
	_check(fx.get_parameter_text(gain_id) == "1.000 x", "default gain text is '%s' (value text + units)" % fx.get_parameter_text(gain_id))

	var bus := AudioServer.bus_count
	AudioServer.add_bus(bus)
	AudioServer.set_bus_name(bus, "Vst3")
	AudioServer.add_bus_effect(bus, fx)
	var cap := AudioEffectCapture.new()
	cap.buffer_length = 1.0
	AudioServer.add_bus_effect(bus, cap)
	var gen := AudioStreamGenerator.new()
	gen.mix_rate = RATE
	gen.buffer_length = 0.2
	player = AudioStreamPlayer.new()
	player.stream = gen
	player.bus = "Vst3"
	add_child(player)
	player.play()
	playback = player.get_stream_playback()

	var unity := await _peak_over(cap, 0.4)
	_check(fx.get_instance_count() == 1, "one instance on the stereo bus")
	_check(absf(unity - 0.5) < 0.01, "silent sidechain = null test: peak %.4f (want 0.5)" % unity)

	fx.set("parameters/Gain", 0.25)
	await get_tree().create_timer(0.3).timeout
	var half := await _peak_over(cap, 0.3)
	_check(absf(half - unity * 0.5) < 0.005, "Gain 0.25 (0.5 x) halves the level: %.4f -> %.4f (%s)" % [unity, half, fx.get_parameter_text(gain_id)])

	var file := "user://vst3_test_plugin.tres"
	ResourceSaver.save(fx, file)
	var loaded: Vst3PluginEffect = ResourceLoader.load(file, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(loaded.is_plugin_loaded(), "saved .tres reloads the plugin")
	_check(is_equal_approx(loaded.get_parameter(gain_id), 0.25), "saved .tres keeps Gain = %.3f" % loaded.get_parameter(gain_id))

	# Swapping the plugin while audio runs: unload (passthrough), then load again.
	fx.plugin_path = ""
	fx.is_plugin_loaded()
	var through := await _peak_over(cap, 0.3)
	_check(absf(through - 0.5) < 0.01, "unloading mid-stream falls back to passthrough (peak %.4f)" % through)
	fx.plugin_path = path
	_check(fx.is_plugin_loaded(), "reloading mid-stream works")
	AudioServer.remove_bus(AudioServer.get_bus_index("Vst3"))
