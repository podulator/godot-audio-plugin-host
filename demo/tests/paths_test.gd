extends Node
# Plugin search folders and portable paths (godot_common/plugin_paths.h):
#   godot --headless --path demo res://tests/paths_test.tscn
# Copies Dragonfly Hall into res://bin/clap for the test and removes it afterwards.

const SRC := "/usr/lib/clap/DragonflyHallReverb.clap"
const SYS := "audio/plugin_host/clap/use_system_paths"
var failures := 0


func _ready() -> void:
	if not FileAccess.file_exists(SRC):
		print("SKIP  Dragonfly Hall not installed")
		print("PATHS OK (1 skipped)")
		get_tree().quit(0)
		return
	var bin := ProjectSettings.globalize_path("res://bin/clap")
	DirAccess.make_dir_recursive_absolute(bin)
	DirAccess.copy_absolute(SRC, bin.path_join("DragonflyHallReverb.clap"))
	_check(ProjectSettings.get_setting("audio/plugin_host/clap/search_paths") == PackedStringArray(["res://bin/clap"]),
			"the search_paths setting exists, defaulting to res://bin/clap")

	ProjectSettings.set_setting(SYS, false) # Project folder only, as on a player's machine.
	var rel := _load("DragonflyHallReverb.clap")
	_check(rel.is_plugin_loaded(), "a relative path loads from res://bin/clap with system folders off")
	var moved := _load("/home/someone-else/plugins/DragonflyHallReverb.clap")
	_check(moved.is_plugin_loaded(), "an absolute path from another machine falls back to its file name")

	DirAccess.remove_absolute(bin.path_join("DragonflyHallReverb.clap"))
	var gone := _load("DragonflyHallReverb.clap")
	_check(not gone.is_plugin_loaded() and gone.get_load_error().contains("not found in any plugin folder"),
			"without the project copy, it fails cleanly: '%s'" % gone.get_load_error())

	# Both copies present, system folders on: the project's comes first.
	DirAccess.copy_absolute(SRC, bin.path_join("DragonflyHallReverb.clap"))
	ProjectSettings.set_setting(SYS, true)
	var both := _load("DragonflyHallReverb.clap")
	var found := ""
	for p in ClapPluginEffect.scan_plugins():
		if p.get("name") == "Dragonfly Hall Reverb":
			found = p.path
			break
	_check(both.is_plugin_loaded() and found.begins_with(bin), "the project's copy wins over the system one (%s)" % found)

	DirAccess.remove_absolute(bin.path_join("DragonflyHallReverb.clap"))
	DirAccess.remove_absolute(bin)
	DirAccess.remove_absolute(bin.get_base_dir())
	print("PATHS %s" % ("OK" if failures == 0 else "FAILED (%d)" % failures))
	get_tree().quit(1 if failures else 0)


func _load(path: String) -> ClapPluginEffect:
	var fx := ClapPluginEffect.new()
	fx.plugin_path = path
	fx.is_plugin_loaded() # forces the deferred load now
	return fx


func _check(ok: bool, what: String) -> void:
	print(("PASS  " if ok else "FAIL  ") + what)
	if not ok:
		failures += 1
