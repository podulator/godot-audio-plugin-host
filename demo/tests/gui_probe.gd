@tool
extends Node
# Opens a plugin's window inside the real editor (via tests/editor_probe.sh), keeps it open for a
# while, and closes it. Environment: GUIPROBE_PLUGIN (path), GUIPROBE_FORMAT (clap|vst3|clap_instrument),
# GUIPROBE_SECONDS. Opens a REAL window: on the user's screen unless DISPLAY points elsewhere (Xvfb), so ask first.

func _ready() -> void:
	if not Engine.is_editor_hint():
		return
	await get_tree().process_frame
	var path := OS.get_environment("GUIPROBE_PLUGIN")
	var format := OS.get_environment("GUIPROBE_FORMAT")
	# clap, vst3, or clap_instrument: a ClapInstrument, whose window works the same way.
	var fx: Resource = ClapInstrument.new() if format == "clap_instrument" else (Vst3PluginEffect.new() if format == "vst3" else ClapPluginEffect.new())
	fx.plugin_path = path if path != "" else "/usr/lib/clap/DragonflyHallReverb.clap"
	fx.editor_closed.connect(func(): print("GUIPROBE editor_closed signal"))
	var t0 := Time.get_ticks_msec()
	var opened: bool = fx.open_editor()
	print("GUIPROBE plugin=%s loaded=%s opened=%s is_open=%s (%d ms)" % [fx.get_plugin_info().get("name", "?"), fx.is_plugin_loaded(), opened, fx.is_editor_open(), Time.get_ticks_msec() - t0])
	if opened:
		var seconds := float(OS.get_environment("GUIPROBE_SECONDS")) if OS.get_environment("GUIPROBE_SECONDS") != "" else 10.0
		var until := Time.get_ticks_msec() + int(seconds * 1000)
		while Time.get_ticks_msec() < until and fx.is_editor_open():
			await get_tree().process_frame
		print("GUIPROBE still open after the wait: ", fx.is_editor_open())
		fx.close_editor()
		print("GUIPROBE closed, is_open=", fx.is_editor_open())
	get_tree().quit()
