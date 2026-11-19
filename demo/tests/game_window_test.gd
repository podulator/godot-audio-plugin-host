extends Node
# Plugin windows in a running GAME (not the editor): open_editor() from a script opens the plugin's
# own GUI in its own window, and close_editor() closes it. Opens a REAL window, so it is run only
# under Xvfb (tests/run_all.sh starts one on a display of its own), never on the user's screen.
# Prints GAMEWINDOW lines; the caller checks the window on the X server too (xwininfo).
#   GAMEWINDOW_PLUGIN: a CLAP plugin with a GUI (default: Dragonfly Hall Reverb).

func _ready() -> void:
	await get_tree().process_frame
	var path := OS.get_environment("GAMEWINDOW_PLUGIN")
	var fx := ClapPluginEffect.new()
	fx.plugin_path = path if not path.is_empty() else "/usr/lib/clap/DragonflyHallReverb.clap"
	if not fx.is_plugin_loaded():
		print("GAMEWINDOW SKIP plugin not loaded: ", fx.get_load_error())
		get_tree().quit()
		return
	print("GAMEWINDOW editor_hint=%s" % Engine.is_editor_hint())
	var opened := fx.open_editor()
	print("GAMEWINDOW opened=%s is_open=%s" % [opened, fx.is_editor_open()])
	for i in 60: # a second: let the GUI map and draw, then the caller looks at the X server
		await get_tree().process_frame
	print("GAMEWINDOW ready")
	OS.delay_msec(1500) # the caller's xwininfo runs during this
	fx.close_editor()
	for i in 10:
		await get_tree().process_frame
	print("GAMEWINDOW closed is_open=%s" % fx.is_editor_open())
	get_tree().quit()
