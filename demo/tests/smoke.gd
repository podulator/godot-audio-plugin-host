extends Node
# Headless smoke test for the shared core, run as a scene:
#   godot --headless --path demo res://tests/smoke.tscn
# Both extensions load side by side, both effects sit on one bus with no plugin loaded, and a
# sine played through that bus must come out of an AudioEffectCapture unchanged.
#
# "1 ObjectDB instance was leaked at exit" is Godot's own AudioStreamGeneratorPlayback, and
# appears with no extension loaded at all (checked against Godot 4.7 with built-in effects).

const RATE := 44100.0
const FREQ := 440.0

var clap_fx: AudioEffect
var vst3_fx: AudioEffect
var capture: AudioEffectCapture
var playback: AudioStreamGeneratorPlayback
var phase := 0.0
var elapsed := 0.0
var captured := PackedVector2Array()
var failures := 0


func _ready() -> void:
	for cls in ["ClapPluginEffect", "Vst3PluginEffect"]:
		_check(ClassDB.class_exists(cls), "class %s registered" % cls)
	if failures:
		_finish()
		return

	var bus := AudioServer.bus_count
	AudioServer.add_bus(bus)
	AudioServer.set_bus_name(bus, "PluginTest")
	clap_fx = ClassDB.instantiate("ClapPluginEffect")
	vst3_fx = ClassDB.instantiate("Vst3PluginEffect")
	capture = AudioEffectCapture.new()
	AudioServer.add_bus_effect(bus, clap_fx)
	AudioServer.add_bus_effect(bus, vst3_fx)
	AudioServer.add_bus_effect(bus, capture)

	var gen := AudioStreamGenerator.new()
	gen.mix_rate = RATE
	gen.buffer_length = 0.5
	var player := AudioStreamPlayer.new()
	player.stream = gen
	player.bus = "PluginTest"
	add_child(player)
	player.play()
	playback = player.get_stream_playback()
	_fill()


func _process(delta: float) -> void:
	if playback == null:
		return
	_fill()
	var n := capture.get_frames_available()
	if n > 0:
		captured.append_array(capture.get_buffer(n))
	elapsed += delta
	if elapsed > 1.5:
		set_process(false)
		_evaluate()


func _fill() -> void:
	var n := playback.get_frames_available()
	for i in n:
		var s := sin(phase * TAU) * 0.5
		playback.push_frame(Vector2(s, -s))
		phase = fmod(phase + FREQ / RATE, 1.0)


func _evaluate() -> void:
	_check(clap_fx.get_instance_count() == 1, "one CLAP instance on a stereo bus (got %d)" % clap_fx.get_instance_count())
	_check(vst3_fx.get_instance_count() == 1, "one VST3 instance on a stereo bus (got %d)" % vst3_fx.get_instance_count())
	_check(clap_fx.get_frames_processed() > 0, "CLAP effect processed %d frames" % clap_fx.get_frames_processed())
	_check(vst3_fx.get_frames_processed() > 0, "VST3 effect processed %d frames" % vst3_fx.get_frames_processed())

	var peak := 0.0
	var mirrored := true
	for f in captured:
		peak = max(peak, absf(f.x))
		if absf(f.x + f.y) > 1e-6:
			mirrored = false
	_check(captured.size() > 1000, "captured %d frames" % captured.size())
	_check(absf(peak - 0.5) < 0.01, "sine peak survived passthrough (peak %.4f)" % peak)
	_check(mirrored, "left/right kept their order (right == -left)")
	_finish()


func _check(ok: bool, what: String) -> void:
	print(("PASS  " if ok else "FAIL  ") + what)
	if not ok:
		failures += 1


func _finish() -> void:
	print("SMOKE %s" % ("OK" if failures == 0 else "FAILED (%d)" % failures))
	get_tree().quit(1 if failures else 0)
