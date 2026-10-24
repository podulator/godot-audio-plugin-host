extends Node
# Cross-platform test: the same checks on Linux, Windows and macOS, in CI and locally.
#   godot --headless --path demo res://tests/platform_test.tscn
#
# Uses only plugins built from this repository (examples/SConstruct test_plugin=yes), installed
# in res://bin/clap and res://bin/vst3 by tests/platform/run.sh, and loaded by RELATIVE name, so
# the plugin-folder logic is tested on each OS as well. Results go to $PLATFORM_TEST_OUT (or
# user://platform_test.txt) as well as stdout: on Windows the normal Godot binary is a GUI program, and its stdout may not
# reach the shell that started it.

const TestUtilScript = preload("res://tests/test_util.gd")
const RATE := 44100.0
const GAIN_CLAP := 1 # gaph_test.clap: out = main * gain + sidechain * mix
const MIX_CLAP := 2
const SYSTEM_PLUGINS := ["audio/plugin_host/clap/use_system_paths", "audio/plugin_host/vst3/use_system_paths"]

var failures := 0
var lines := PackedStringArray()
var playback: AudioStreamGeneratorPlayback
var level := 0.5
var phase := 0.0


func _ready() -> void:
	# Only the plugins this test installed: a machine's own plugins must not change the result.
	for s in SYSTEM_PLUGINS:
		ProjectSettings.set_setting(s, false)
	_log("platform: %s %s, Godot %s" % [OS.get_name(), Engine.get_architecture_name(), Engine.get_version_info().string])
	await _test_loading()
	await _test_gain_and_timing()
	await _test_sidechain()
	_test_state()
	_test_menu()
	_log("PLATFORM %s" % ("OK" if failures == 0 else "FAILED (%d)" % failures))
	# PLATFORM_TEST_OUT: where to write the results (an absolute path), since user:// lives in a
	# different place on each OS.
	var out := OS.get_environment("PLATFORM_TEST_OUT")
	var f := FileAccess.open(out if out != "" else "user://platform_test.txt", FileAccess.WRITE)
	f.store_string("\n".join(lines) + "\n")
	f.close()
	get_tree().quit(1 if failures else 0)


func _process(_delta: float) -> void:
	if playback: # A constant level, so every output sample is exactly predictable.
		for i in playback.get_frames_available():
			playback.push_frame(Vector2(level, -level))


func _log(line: String) -> void:
	print(line)
	lines.append(line)


func _check(ok: bool, what: String) -> void:
	_log(("PASS  " if ok else "FAIL  ") + what)
	if not ok:
		failures += 1


# A bus holding `effects` then a capture, fed by a constant-level generator player.
func _bus(bus_name: String, effects: Array) -> Dictionary:
	var bus := AudioServer.bus_count
	AudioServer.add_bus(bus)
	AudioServer.set_bus_name(bus, bus_name)
	for fx in effects:
		AudioServer.add_bus_effect(bus, fx)
	var cap := AudioEffectCapture.new()
	cap.buffer_length = 1.0
	AudioServer.add_bus_effect(bus, cap)
	var gen := AudioStreamGenerator.new()
	gen.mix_rate = RATE
	gen.buffer_length = 0.2
	var player := AudioStreamPlayer.new()
	player.stream = gen
	player.bus = bus_name
	add_child(player)
	player.play()
	playback = player.get_stream_playback()
	return {"bus": bus_name, "cap": cap, "player": player}


func _end_bus(b: Dictionary) -> void:
	playback = null
	await TestUtilScript.free_generator_player(b.player)
	AudioServer.remove_bus(AudioServer.get_bus_index(b.bus))


# Every frame the capture receives during p_seconds, collected as it arrives.
func _collect(cap: AudioEffectCapture, p_seconds: float) -> PackedVector2Array:
	var frames := PackedVector2Array()
	cap.clear_buffer()
	var t0 := Time.get_ticks_usec()
	while Time.get_ticks_usec() - t0 < p_seconds * 1e6:
		await get_tree().process_frame
		frames.append_array(cap.get_buffer(cap.get_frames_available()))
	return frames


# The last captured frame, after letting the audio settle.
func _settled(cap: AudioEffectCapture) -> Vector2:
	await get_tree().create_timer(0.4).timeout
	var frames := cap.get_buffer(cap.get_frames_available())
	return frames[frames.size() - 1] if frames.size() > 0 else Vector2(INF, INF)


func _clap(name: String) -> ClapPluginEffect:
	var fx := ClapPluginEffect.new()
	fx.plugin_path = name # relative: found in res://bin/clap
	fx.is_plugin_loaded()
	return fx


# By class name rather than Vst3PluginEffect.new(): on Android there is no VST3 extension, and a
# script naming a class that does not exist would not even load.
func _vst3(name: String) -> AudioEffect:
	var fx: AudioEffect = ClassDB.instantiate("Vst3PluginEffect")
	fx.plugin_path = name # relative: found in res://bin/vst3
	fx.is_plugin_loaded()
	return fx


func _has_vst3() -> bool:
	return ClassDB.class_exists("Vst3PluginEffect")


func _test_loading() -> void:
	var effects: Array = [_clap("gaph_test.clap"), _clap("clap_gain.clap")]
	if _has_vst3():
		effects.append(_vst3("vst3_gain.vst3"))
	else:
		_log("INFO  no VST3 extension on %s: CLAP only" % OS.get_name())
	for fx in effects:
		_check(fx.is_plugin_loaded(), "%s loads by relative name from res://bin (%s)" % [fx.plugin_path, fx.get_load_error()])
	await get_tree().process_frame


# Gain, exact, then a scheduled 100 ms dip, exact to the sample, in each format.
func _test_gain_and_timing() -> void:
	var rate := AudioServer.get_mix_rate()
	var cases := [
		["clap", _clap("gaph_test.clap"), GAIN_CLAP, 0.5, 0.0, 1.0], # format, fx, id, half, zero, unity
	]
	if _has_vst3():
		cases.append(["vst3", _vst3("vst3_gain.vst3"), 0, 0.25, 0.0, 0.5])
	for c in cases:
		var fx: AudioEffect = c[1]
		var b := _bus("Gain_" + c[0], [fx])
		level = 0.5
		fx.set_parameter(c[2], c[3])
		var v := await _settled(b.cap)
		_check(v == Vector2(0.25, -0.25), "%s: gain x0.5 is exact (%s)" % [c[0], v])

		fx.set_parameter(c[2], c[5])
		# Settle the effect's BlockClock first (src/core/block_clock.h): it learns when its audio
		# plays from the EARLIEST block arrival seen, so on a stream only a moment old a still
		# earlier block can arrive mid-dip and move its end (seen once on CI's Windows runner: 22
		# samples, 0.5 ms). A game's buses run for a while before anything is scheduled.
		await get_tree().create_timer(1.2).timeout
		b.cap.clear_buffer()
		fx.set_parameter_at(c[2], c[4], 0.1)
		fx.set_parameter_at(c[2], c[5], 0.2)
		var frames := await _collect(b.cap, 0.8)
		var silent := 0
		var first := -1
		var last := -1
		for i in frames.size():
			if frames[i].x == 0.0:
				silent += 1
				last = i
				first = i if first < 0 else first
		var want := int(round(rate * 0.1))
		var where := "%d silent of %d frames, at %d..%d" % [silent, frames.size(), first, last]
		if abs(silent - want) <= 1:
			_check(true, "%s: a scheduled 100 ms dip lasts %d samples, want %d (%s)" % [c[0], silent, want, where])
		else:
			# Headless Godot mixes with its dummy driver, which paces itself by sleeping. On a busy
			# or virtual machine the sleeps overrun and fewer samples are mixed per second than the
			# mix rate, while scheduling runs in real time. Each block then stands for more real
			# time than it holds samples, and changes scheduled within one gap all land on the next
			# block's first sample (a dip's start and end together: no dip at all). No host can do
			# better with a stream that is not real time, so there the test reports, not fails. At
			# full speed (within 10%), a wrong dip is a real failure.
			var speed := (await _collect(b.cap, 2.5)).size() / (2.5 * rate)
			if abs(speed - 1.0) <= 0.1:
				_check(false, "%s: a scheduled 100 ms dip lasts %d samples, want %d, with the audio at %d%% of real time (%s)" % [c[0], silent, want, roundi(speed * 100), where])
			else:
				_log("INFO  %s: dip not checked: the audio driver runs at %d%% of real time here, not 100%% (%s)" % [c[0], roundi(speed * 100), where])
		await _end_bus(b)


# Sidechain through a send: the test plugin with gain 0 and mix 1 outputs the sidechain alone.
func _test_sidechain() -> void:
	var fx := _clap("gaph_test.clap")
	fx.sidechain = "platform_side"
	fx.set_parameter(GAIN_CLAP, 0.0)
	fx.set_parameter(MIX_CLAP, 1.0)
	var main := _bus("SideMain", [fx])
	var send := ClapSidechainSend.new()
	send.channel = "platform_side"
	# The source bus gets a higher index than SideMain, so it is mixed first (same block).
	var src := AudioServer.bus_count
	AudioServer.add_bus(src)
	AudioServer.set_bus_name(src, "SideSrc")
	AudioServer.add_bus_effect(src, send)
	var gen := AudioStreamGenerator.new()
	gen.mix_rate = RATE
	var side_player := AudioStreamPlayer.new()
	side_player.stream = gen
	side_player.bus = "SideSrc"
	add_child(side_player)
	side_player.play()
	var side_pb: AudioStreamGeneratorPlayback = side_player.get_stream_playback()
	for i in 30:
		for j in side_pb.get_frames_available():
			side_pb.push_frame(Vector2(0.125, 0.125))
		await get_tree().process_frame
	var v: Vector2 = main.cap.get_buffer(main.cap.get_frames_available())[-1]
	_check(v == Vector2(0.125, 0.125), "clap: the sidechain reaches the plugin through a send, exactly (%s)" % v)
	side_pb = null
	await TestUtilScript.free_generator_player(side_player)
	AudioServer.remove_bus(AudioServer.get_bus_index("SideSrc"))
	await _end_bus(main)


# Plugin state: saved from one effect, restored into a fresh one.
func _test_state() -> void:
	var a := _clap("gaph_test.clap")
	a.set_parameter(GAIN_CLAP, 0.75)
	var bytes: PackedByteArray = a.plugin_state
	var b := _clap("gaph_test.clap")
	b.plugin_state = bytes
	_check(bytes.size() == 16 and is_equal_approx(b.get_parameter(GAIN_CLAP), 0.75),
			"clap: plugin state round-trips (%d bytes, gain %.2f)" % [bytes.size(), b.get_parameter(GAIN_CLAP)])


# The per-plugin Add Effect classes (forced on with GAPH_MENU_CLASSES=1; editor-only otherwise).
func _test_menu() -> void:
	if OS.get_environment("GAPH_MENU_CLASSES").is_empty():
		_log("SKIP  menu classes: run with GAPH_MENU_CLASSES=1")
		return
	for c in ["Clap_GAPHTestPlugin", "Clap_ExampleGain"] + (["Vst3_ExampleGain"] if _has_vst3() else []):
		_check(ClassDB.class_exists(c), "Add Effect menu class %s is registered" % c)
