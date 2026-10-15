extends Node
# Both extensions, chained on ONE bus in one process:
#   godot --headless --path demo res://tests/both_test.tscn
# Our CLAP test plugin, then our VST3 one (built by tests/run_all.sh). If the two hosts shared
# state, or one's symbols leaked into the other (RTLD_GLOBAL), this is where it shows. Each is set
# to a gain of 0.5 (CLAP takes 0.5 directly, VST3 a normalised 0.25), so the bus must come out at
# exactly a quarter.

const TestUtilScript = preload("res://tests/test_util.gd")
const RATE := 44100.0
var phase := 0.0
var playback: AudioStreamGeneratorPlayback
var failures := 0


func _ready() -> void:
	var clap_path := TestUtilScript.own_plugin("gaph_test.clap")
	var vst3_path := TestUtilScript.own_plugin("gaph_test_vst3.vst3")
	if not FileAccess.file_exists(clap_path) or not DirAccess.dir_exists_absolute(vst3_path):
		print("SKIP  test plugins not built in " + clap_path.get_base_dir())
		print("BOTH OK (1 skipped)")
		get_tree().quit(0)
		return
	var clap := ClapPluginEffect.new()
	clap.plugin_path = clap_path
	var vst3 := Vst3PluginEffect.new()
	vst3.plugin_path = vst3_path
	_check(clap.is_plugin_loaded() and vst3.is_plugin_loaded(), "a CLAP and a VST3 plugin both load in one process")

	var bus := AudioServer.bus_count
	AudioServer.add_bus(bus)
	AudioServer.set_bus_name(bus, "Both")
	AudioServer.add_bus_effect(bus, clap)
	AudioServer.add_bus_effect(bus, vst3)
	var cap := AudioEffectCapture.new()
	AudioServer.add_bus_effect(bus, cap)
	var gen := AudioStreamGenerator.new()
	gen.mix_rate = RATE
	var player := AudioStreamPlayer.new()
	player.stream = gen
	player.bus = "Both"
	add_child(player)
	player.play()
	playback = player.get_stream_playback()

	var unity := await _peak(cap)
	_check(absf(unity - 0.5) < 0.01, "chained null test: peak %.4f (want 0.5)" % unity)
	clap.set_parameter(clap.find_parameter("Gain"), 0.5)
	vst3.set_parameter(vst3.find_parameter("Gain"), 0.25)
	var clap_text := clap.get_parameter_text(clap.find_parameter("Gain"))
	var vst3_text := vst3.get_parameter_text(vst3.find_parameter("Gain"))
	_check(clap_text == vst3_text, "both formats describe the value the same way: '%s' / '%s'" % [clap_text, vst3_text])
	await get_tree().create_timer(0.3).timeout
	var both := await _peak(cap)
	_check(absf(both - unity * 0.25) < 0.005, "0.5 x 0.5 in total: peak %.4f (want %.4f)" % [both, unity * 0.25])
	print("BOTH %s" % ("OK" if failures == 0 else "FAILED (%d)" % failures))
	get_tree().quit(1 if failures else 0)


func _process(_delta: float) -> void:
	if playback:
		for i in playback.get_frames_available():
			var s := sin(phase * TAU) * 0.5
			playback.push_frame(Vector2(s, s))
			phase = fmod(phase + 440.0 / RATE, 1.0)


func _peak(cap: AudioEffectCapture) -> float:
	cap.clear_buffer()
	await get_tree().create_timer(0.3).timeout
	var peak := 0.0
	for f in cap.get_buffer(cap.get_frames_available()):
		peak = maxf(peak, absf(f.x))
	return peak


func _check(ok: bool, what: String) -> void:
	print(("PASS  " if ok else "FAIL  ") + what)
	if not ok:
		failures += 1
