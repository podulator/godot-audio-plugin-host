extends Node
# Per-plugin "Add Effect" entries. Run twice:
#   GAPH_MENU_CLASSES=1 CLAP_PATH=... GAPH_VST3_PATH=... godot ... res://tests/menu_test.tscn
#       -> entries present, and each creates the GENERIC effect with the path set.
#   godot ... res://tests/menu_test.tscn  (a plain game run)
#       -> no entries at all: they are editor-only.

var failures := 0


func _ready() -> void:
	var forced := not OS.get_environment("GAPH_MENU_CLASSES").is_empty()
	# Exactly what the bus panel's menu does (editor_audio_buses.cpp).
	var entries: Array = Array(ClassDB.get_inheriters_from_class("AudioEffect")).filter(
			func(c): return ClassDB.can_instantiate(c) and (c.begins_with("Clap_") or c.begins_with("Vst3_")))
	entries.sort()
	print("      menu entries: ", ", ".join(entries))

	if not forced:
		_check(entries.is_empty(), "plain game run: no per-plugin classes registered")
		_finish()
		return

	# Our own plugins always (run_all.sh points CLAP_PATH and GAPH_VST3_PATH at examples/build);
	# Dragonfly only where it is installed.
	var own := OS.get_environment("CLAP_PATH")
	var expect := {
		"Clap_GAPHTestPlugin": FileAccess.file_exists(own.path_join("gaph_test.clap")),
		"Clap_ExampleGain": FileAccess.file_exists(own.path_join("clap_gain.clap")),
		"Vst3_GAPHTestVST3": DirAccess.dir_exists_absolute(OS.get_environment("GAPH_VST3_PATH").path_join("gaph_test_vst3.vst3")),
		"Vst3_ExampleGain": DirAccess.dir_exists_absolute(OS.get_environment("GAPH_VST3_PATH").path_join("vst3_gain.vst3")),
		"Clap_DragonflyHallReverb": FileAccess.file_exists("/usr/lib/clap/DragonflyHallReverb.clap"),
	}
	for want in expect:
		if expect[want]:
			_check(want in entries, "%s is in the Add Effect menu" % want)
		else:
			print("SKIP  %s: plugin not installed" % want)
	_check(not entries.any(func(c): return c.contains("URCHIN")), "the URCHIN instrument is left out")

	for name in entries:
		var fx: AudioEffect = ClassDB.instantiate(name)
		var generic := "ClapPluginEffect" if name.begins_with("Clap_") else "Vst3PluginEffect"
		var ok: bool = fx != null and fx.get_class() == generic and fx.is_plugin_loaded()
		_check(ok, "%s creates a %s with the plugin loaded (%s)" % [name, generic, fx.get_plugin_info().get("name", "?") if fx else "null"])

	if entries.is_empty():
		print("SKIP  no plugins installed: nothing to save")
		_finish()
		return
	var first: AudioEffect = ClassDB.instantiate(entries[0])
	ResourceSaver.save(first, "user://menu_test.tres")
	var first_line := FileAccess.get_file_as_string("user://menu_test.tres").split("\n")[0]
	_check(first_line.contains('type="ClapPluginEffect"') or first_line.contains('type="Vst3PluginEffect"'), "a saved scene refers to the generic class: %s" % first_line)
	_finish()


func _check(ok: bool, what: String) -> void:
	print(("PASS  " if ok else "FAIL  ") + what)
	if not ok:
		failures += 1


func _finish() -> void:
	print("MENU %s" % ("OK" if failures == 0 else "FAILED (%d)" % failures))
	get_tree().quit(1 if failures else 0)
