extends Node

# By path, not class_name: a new class_name is only known after a project import.
const TestUtilScript = preload("res://tests/test_util.gd")
# Sidechain through real Godot buses, both formats:
#   godot --headless --path demo res://tests/sidechain_test.tscn
# Main bus: a constant 0.5 -> our test plugin (sidechain "side") -> capture.
# Side bus: a constant 0.125 -> send "side".
# The test plugins output main * gain + sidechain * mix. With gain 0 and mix 1 (the same values in
# both formats) the main bus carries exactly the Side bus's level, so every check is exact. Run
# with the Side bus mixed first (same block) and second (one block late): both must work.

const RATE := 44100.0
const GAIN_ID := 1
const MIX_ID := 2
const MAIN_LEVEL := 0.5
const SIDE_LEVEL := 0.125

var failures := 0
var main_pb: AudioStreamGeneratorPlayback
var side_pb: AudioStreamGeneratorPlayback


func _ready() -> void:
	var paths := {"clap": TestUtilScript.own_plugin("gaph_test.clap"), "vst3": TestUtilScript.own_plugin("gaph_test_vst3.vst3")}
	if not FileAccess.file_exists(paths.clap) or not DirAccess.dir_exists_absolute(paths.vst3):
		print("SKIP  test plugins not built in " + paths.clap.get_base_dir())
		print("SIDECHAIN OK (1 skipped)")
		get_tree().quit(0)
		return
	for fmt in (OS.get_environment("SC_ORDER").split(",") if OS.get_environment("SC_ORDER") != "" else ["clap", "vst3"]):
		for side_first in [true, false]:
			await _run(fmt, paths[fmt], side_first)
	print("SIDECHAIN %s" % ("OK" if failures == 0 else "FAILED (%d)" % failures))
	get_tree().quit(1 if failures else 0)


func _process(_delta: float) -> void:
	# Constant levels, so every captured frame is exactly predictable.
	if main_pb:
		for i in main_pb.get_frames_available():
			main_pb.push_frame(Vector2(MAIN_LEVEL, MAIN_LEVEL))
	if side_pb:
		for i in side_pb.get_frames_available():
			side_pb.push_frame(Vector2(SIDE_LEVEL, SIDE_LEVEL))


func _player(bus_name: String) -> AudioStreamGeneratorPlayback:
	var gen := AudioStreamGenerator.new()
	gen.mix_rate = RATE
	gen.buffer_length = 0.2
	var p := AudioStreamPlayer.new()
	p.stream = gen
	p.bus = bus_name
	add_child(p)
	p.play()
	return p.get_stream_playback()


# The last frame the capture holds after a short wait: the bus's settled level.
func _level(cap: AudioEffectCapture) -> Vector2:
	cap.clear_buffer()
	await get_tree().create_timer(0.3).timeout
	var frames := cap.get_buffer(cap.get_frames_available())
	return frames[-1] if frames.size() > 0 else Vector2(INF, INF)


func _run(fmt: String, path: String, side_first: bool) -> void:
	var fx: AudioEffect
	var send: AudioEffect
	if fmt == "clap":
		fx = ClapPluginEffect.new()
		send = ClapSidechainSend.new()
	else:
		fx = Vst3PluginEffect.new()
		send = Vst3SidechainSend.new()
	fx.plugin_path = path
	fx.sidechain = "side_" + fmt
	send.channel = "side_" + fmt
	fx.set_parameter(GAIN_ID, 0.0)
	fx.set_parameter(MIX_ID, 1.0)

	# Bus order decides who runs first: AudioServer mixes from the highest index down.
	var names := ["Main", "Side"] if side_first else ["Side", "Main"]
	for n in names:
		AudioServer.add_bus(AudioServer.bus_count)
		AudioServer.set_bus_name(AudioServer.bus_count - 1, n)
	var main := AudioServer.get_bus_index("Main")
	var side := AudioServer.get_bus_index("Side")
	AudioServer.add_bus_effect(main, fx)
	var cap := AudioEffectCapture.new()
	cap.buffer_length = 1.0
	AudioServer.add_bus_effect(main, cap)
	AudioServer.add_bus_effect(side, send)
	main_pb = _player("Main")
	side_pb = _player("Side")

	var where := "Side bus %d %s Main bus %d" % [side, "mixed before" if side_first else "mixed after", main]
	var v := await _level(cap)
	_check(v == Vector2(SIDE_LEVEL, SIDE_LEVEL), "%s, %s: the sidechain reaches the plugin, exactly (%s)" % [fmt, where, v])

	# The source going away: its player stopped outright, and the Side bus left to switch itself
	# off (audio/buses/channel_disable_time, 2 s by default). The plugin must then hear silence.
	# This does NOT by itself prove the stale-data guard: the bus sends zeros for those 2 s first,
	# so even a reader looping its last block would loop silence. The guard has its own direct test
	# in tests/core/test_core.cpp.
	if side_first:
		for c in get_children():
			if c is AudioStreamPlayer and c.bus == "Side":
				c.stop()
		side_pb = null
		await get_tree().create_timer(2.5).timeout
		v = await _level(cap)
		_check(v == Vector2.ZERO, "%s: Side player stopped, bus switched off: the sidechain falls silent (%s)" % [fmt, v])

	main_pb = null
	side_pb = null
	for c in get_children():
		if c is AudioStreamPlayer:
			await TestUtilScript.free_generator_player(c) # never queue_free a playing generator player
		else:
			c.queue_free()
	AudioServer.remove_bus(AudioServer.get_bus_index("Side"))
	AudioServer.remove_bus(AudioServer.get_bus_index("Main"))


func _check(ok: bool, what: String) -> void:
	print(("PASS  " if ok else "FAIL  ") + what)
	if not ok:
		failures += 1
