extends Node
# Crash protection (core/crash_guard.h), one step per run, driven by tests/run_all.sh:
#   CRASHTEST_STEP=plugin  the test instrument crashes in its process() (GAPH_TEST_CRASH): the
#                          program dies, and the guard marks that plugin
#   CRASHTEST_STEP=check   next start: the marked plugin is refused (its load error says why);
#                          clear_crashed_plugin() lets it load again
#   CRASHTEST_STEP=godot   a crash in Godot's own code (OS.crash): nothing gets marked
const TestUtilScript = preload("res://tests/test_util.gd")

func _ready() -> void:
	var path := TestUtilScript.own_plugin("gaph_test_instrument.clap")
	var step := OS.get_environment("CRASHTEST_STEP")
	if step == "plugin" or step == "godot":
		var inst := ClapInstrument.new()
		inst.plugin_path = path
		var player := AudioStreamPlayer.new()
		player.stream = inst
		add_child(player)
		player.play()
		var midi := MidiDriver.create(1)
		add_child(midi)
		midi.set_channel_target(1, inst)
		await get_tree().create_timer(0.3).timeout
		print("CRASHTEST crashing now (%s)" % step)
		if step == "godot":
			OS.crash("crash test: in Godot's own code")
		midi.note_on(1, 60, 100)
		await get_tree().create_timer(3.0).timeout
		print("CRASHTEST still alive: no crash happened")
		get_tree().quit(1)
		return
	var marked := ClapPluginEffect.get_crashed_plugins()
	print("CRASHTEST marked: ", marked)
	var inst := ClapInstrument.new()
	inst.plugin_path = path
	print("CRASHTEST refused=%s error=%s" % [not inst.is_plugin_loaded(), inst.get_load_error()])
	for m in marked:
		ClapPluginEffect.clear_crashed_plugin(m)
	var again := ClapInstrument.new()
	again.plugin_path = path
	print("CRASHTEST after clearing: loaded=%s marked=%s" % [again.is_plugin_loaded(), ClapPluginEffect.get_crashed_plugins()])
	get_tree().quit()
