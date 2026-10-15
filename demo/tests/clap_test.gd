extends Node
# Headless test of the CLAP extension against real plugins:
#   godot --headless --path demo res://tests/clap_test.tscn
# Our own test plugin always (built by tests/run_all.sh); Dragonfly Reverb when installed.

const TestUtilScript = preload("res://tests/test_util.gd")

const RATE := 44100.0
const HALL_ID := "michaelwillis.dragonfly.hall"

var failures := 0
var skipped := 0
var phase := 0.0
var amplitude := 0.5
var playback: AudioStreamGeneratorPlayback
var player: AudioStreamPlayer


func _ready() -> void:
	await _test_scan()
	await _test_own_plugin()
	await _test_reverb_tail()
	print("CLAP %s (%d skipped)" % ["OK" if failures == 0 else "FAILED (%d)" % failures, skipped])
	get_tree().quit(1 if failures else 0)


func _process(_delta: float) -> void:
	# Keep the generator fed. amplitude 0 means silence, i.e. the input has stopped.
	if playback:
		for i in playback.get_frames_available():
			var s := sin(phase * TAU) * amplitude
			playback.push_frame(Vector2(s, s))
			phase = fmod(phase + 440.0 / RATE, 1.0)


# A bus named `name` holding `fx` followed by a capture, with the sine player routed into it.
func _make_bus(bus_name: String, fx: AudioEffect) -> AudioEffectCapture:
	var bus := AudioServer.bus_count
	AudioServer.add_bus(bus)
	AudioServer.set_bus_name(bus, bus_name)
	AudioServer.add_bus_effect(bus, fx)
	var cap := AudioEffectCapture.new()
	cap.buffer_length = 1.0
	AudioServer.add_bus_effect(bus, cap)
	if player == null:
		var gen := AudioStreamGenerator.new()
		gen.mix_rate = RATE
		gen.buffer_length = 0.2
		player = AudioStreamPlayer.new()
		player.stream = gen
		add_child(player)
	player.stop()
	player.bus = bus_name
	player.play()
	playback = player.get_stream_playback()
	return cap


# Peak level arriving at `cap` over the next `seconds`, after throwing away what came before.
func _peak_over(cap: AudioEffectCapture, seconds: float) -> float:
	cap.clear_buffer()
	await get_tree().create_timer(seconds).timeout
	var peak := 0.0
	for f in cap.get_buffer(cap.get_frames_available()):
		peak = maxf(peak, maxf(absf(f.x), absf(f.y)))
	return peak


func _check(ok: bool, what: String) -> void:
	print(("PASS  " if ok else "FAIL  ") + what)
	if not ok:
		failures += 1


func _skip(what: String) -> void:
	print("SKIP  " + what)
	skipped += 1


func _test_scan() -> void:
	var found: Array = ClapPluginEffect.scan_plugins()
	var ids := found.map(func(d): return d.get("id", ""))
	print("      scan found %d plugins: %s" % [found.size(), ", ".join(ids)])
	if FileAccess.file_exists("/usr/lib/clap/DragonflyHallReverb.clap"):
		_check(HALL_ID in ids, "scan_plugins() finds Dragonfly Hall in /usr/lib/clap")
	else:
		_skip("Dragonfly not installed")
	await get_tree().process_frame


# Our CLAP test plugin (tests/plugins/gaph_test_plugin.c) on a real bus: out = main * gain +
# sidechain * mix, defaults a null test. Gain and Sidechain Mix are ids 1 and 2.
func _test_own_plugin() -> void:
	var path := TestUtilScript.own_plugin("gaph_test.clap")
	if not FileAccess.file_exists(path):
		_skip("test plugin not built at " + path)
		return

	var fx := ClapPluginEffect.new()
	fx.plugin_path = path
	_check(fx.is_plugin_loaded(), "the test plugin loads (%s)" % fx.get_load_error())
	var info: Dictionary = fx.get_plugin_info()
	_check(info.get("id") == "org.godot-audio-plugin-host.test", "plugin info id is %s" % info.get("id"))
	_check(info.get("inputs", []).size() == 2, "plugin info shows main + sidechain inputs")
	_check(fx.get_parameters().size() == 2, "parameters exposed (%d)" % fx.get_parameters().size())

	var gain_id := fx.find_parameter("Gain")
	_check(gain_id == 1, "find_parameter('Gain') -> %d" % gain_id)
	_check(fx.get_parameter_text(gain_id) == "1.000 x", "default gain text is '%s'" % fx.get_parameter_text(gain_id))
	_check("parameters/Gain" in fx.get_property_list().map(func(p): return p.name), "inspector property 'parameters/Gain' exists")

	amplitude = 0.5
	var cap := _make_bus("TestPlugin", fx)
	var unity := await _peak_over(cap, 0.4)
	_check(fx.get_instance_count() == 1, "one instance on the stereo bus")
	_check(absf(unity - 0.5) < 0.01, "silent sidechain = null test: peak %.4f (want 0.5)" % unity)

	# Set through the inspector property this time, so _set() is exercised as well. The wait lets
	# the change through Godot's buffers before measuring.
	fx.set("parameters/Gain", 0.5)
	await get_tree().create_timer(0.3).timeout
	var half := await _peak_over(cap, 0.3)
	_check(absf(half - unity * 0.5) < 0.005, "Gain 0.5 halves the level: %.4f -> %.4f (%s)" % [unity, half, fx.get_parameter_text(gain_id)])
	_check(fx.param_values.has(gain_id), "the change is in the stored param_values")

	# Round-trip through a saved resource: the value must survive by id.
	var file := "user://clap_test_plugin.tres"
	ResourceSaver.save(fx, file)
	var loaded: ClapPluginEffect = ResourceLoader.load(file, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(loaded.is_plugin_loaded(), "saved .tres reloads the plugin")
	_check(is_equal_approx(loaded.get_parameter(gain_id), 0.5), "saved .tres keeps Gain = %.3f" % loaded.get_parameter(gain_id))
	AudioServer.remove_bus(AudioServer.get_bus_index("TestPlugin"))


func _test_reverb_tail() -> void:
	if not FileAccess.file_exists("/usr/lib/clap/DragonflyHallReverb.clap"):
		_skip("Dragonfly Hall not installed")
		return
	var fx := ClapPluginEffect.new()
	fx.plugin_path = "/usr/lib/clap/DragonflyHallReverb.clap"
	_check(fx.is_plugin_loaded(), "Dragonfly Hall loads")

	amplitude = 0.5
	var cap := _make_bus("Hall", fx)
	await get_tree().create_timer(0.3).timeout
	# Stop the source completely. Only _process_silence() returning true keeps our effect running
	# now, and only a working reverb puts anything into the bus.
	player.stop()
	playback = null
	await get_tree().create_timer(0.1).timeout
	var tail := await _peak_over(cap, 0.4)
	_check(tail > 0.001, "tail rings on after the source stops (peak %.4f)" % tail)
	AudioServer.remove_bus(AudioServer.get_bus_index("Hall"))
